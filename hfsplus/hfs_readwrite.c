/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*	@(#)hfs_readwrite.c	1.0
 *
 *	(c) 1998-2001 Apple Computer, Inc.  All Rights Reserved
 *
 *	hfs_readwrite.c -- vnode operations to deal with reading and writing
 *files.
 *
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#ifdef DARWIN
#include <miscfs/specfs/specdev.h>
#endif

#define QUOTA 1

#ifdef DARWIN_UBC
#include <sys/ubc.h>

#include <vm/vm_pageout.h>
#endif

#ifdef DARWIN
#include <sys/kdebug.h>
#endif

#include <geom/geom.h>
#include <geom/geom_vfs.h>
#include <hfsplus/hfs.h>
#include <hfsplus/hfs_cnode.h>
#include <hfsplus/hfs_dbg.h>
#include <hfsplus/hfs_endian.h>
#include <hfsplus/hfs_quota.h>

#include "hfscommon/headers/BTreesInternal.h"
#include "hfscommon/headers/FileMgrInternal.h"

#define can_cluster(size) ((((size & (4096 - 1))) == 0) && (size <= (MAXPHYSIO / 2)))

extern int overflow_extents(struct filefork *fp);

enum {
	MAXHFSFILESIZE = 0x7FFFFFFF /* this needs to go in the mount structure */
};

extern u_int32_t GetLogicalBlockSize(struct vnode *vp);

static struct dirent dot = {
	.d_fileno = 1,
	.d_off = 0,
	.d_reclen = _GENERIC_DIRLEN(1),
	.d_namlen = 1,
	.d_name = ".",
};

static struct dirent dotdot = { 
	.d_fileno = 1, 
	.d_off = 0, 
	.d_reclen = _GENERIC_DIRLEN(2), 
	.d_namlen = 2, 
	.d_name = ".." 
};

int
hfs_read(struct vop_read_args *ap)
{
	/* struct vop_read_args {
		struct vnode *a_vp;
		struct uio *a_uio;
		int a_ioflag;
		struct ucred *a_cred;
	} */
	register struct uio *uio = ap->a_uio;
	register struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct filefork *fp = VTOF(vp);
	struct buf *bp;
	daddr_t logBlockNo;
	u_long fragSize, moveSize, startOffset, ioxfersize;
	u_long logBlockSize;
	off_t bytesRemaining;
	int retval = 0;
	off_t filesize;
	// off_t filebytes;

	/* Preflight checks */
	if (vp->v_type != VREG && vp->v_type != VLNK)
		return (EISDIR); /* HFS can only read files */
	if (uio->uio_resid == 0)
		return (0); /* Nothing left to do */
	if (uio->uio_offset < 0)
		return (EINVAL); /* cant read from a negative offset */

	filesize = fp->ff_size;
	// filebytes = (off_t)fp->ff_blocks * (off_t)VTOVCB(vp)->blockSize;
	if (uio->uio_offset > filesize) {
		if ((!ISHFSPLUS(VTOVCB(vp))) && (uio->uio_offset > (off_t)MAXHFSFILESIZE))
			return (EFBIG);
		else
			return (0);
	}

	logBlockSize = GetLogicalBlockSize(vp);

	for (retval = 0, bp = NULL; uio->uio_resid > 0; bp = NULL) {
		if ((bytesRemaining = (filesize - uio->uio_offset)) <= 0)
			break;

		fragSize = logBlockSize;
		logBlockNo = (daddr_t)(uio->uio_offset / logBlockSize);
		startOffset = (u_long)(uio->uio_offset % fragSize);

		ioxfersize = fragSize; /* will always divide allocation block */

		moveSize = ioxfersize;
		moveSize -= startOffset;

		if (bytesRemaining < moveSize)
			moveSize = bytesRemaining;

		if (uio->uio_resid < moveSize) {
			moveSize = uio->uio_resid;
		};
		if (moveSize == 0) {
			break;
		};

		if ((uio->uio_offset + fragSize) >= filesize) {
			retval = bread(vp, logBlockNo, ioxfersize, NOCRED, &bp);
		} else {
			retval = bread(vp, logBlockNo, ioxfersize, NOCRED, &bp);
		};

		if (retval != E_NONE) {
			if (bp) {
				brelse(bp);
				bp = NULL;
			}
			break;
		};

		/*
		 * We should only get non-zero b_resid when an I/O
		 * retval has occurred, which should cause us to break
		 * above. However, if the short read did not cause an
		 * retval, then we want to ensure that we do not uiomove
		 * bad or uninitialized data.
		 */
		ioxfersize -= bp->b_resid;

		if (ioxfersize < moveSize) {
			/* XXX PPD This should take the offset into account, too! */
			if (ioxfersize == 0)
				break;
			moveSize = ioxfersize;
		}
		if ((startOffset + moveSize) > bp->b_bcount)
			panic("hfs_read: bad startOffset or moveSize\n");

		if ((retval = uiomove((caddr_t)bp->b_data + startOffset, (int)moveSize, uio)))
			break;

		if (S_ISREG(cp->c_mode) && (((startOffset + moveSize) == fragSize) || (uio->uio_offset == filesize))) {
			bp->b_flags |= B_AGE;
		};

		brelse(bp);
		/* Start of loop resets bp to NULL before reaching
		 * outside this block... */
	}

	if (bp != NULL) {
		brelse(bp);
	}

	cp->c_flag |= C_ACCESS;

	return (retval);
}

int
hfs_readdir(struct vop_readdir_args *ap)
{
	/* {
		struct vnode *vp;
		struct uio *uio;
		struct ucred *cred;
		int *eofflag;
		int *ncookies;
		u_long **cookies;
	} */
	register struct uio *uio = ap->a_uio;
	struct cnode *cp = VTOC(ap->a_vp);
	struct hfsmount *hfsmp = VTOHFS(ap->a_vp);
	proc_t *p = curthread;
	off_t off = uio->uio_offset;
	int retval = 0;
	int eofflag = 0;
	// void *user_start = NULL;
	// int   user_len;

	if (uio->uio_offset < 0) {
		return (EINVAL);
	}

	if (/*ap->a_eofflag != NULL ||*/ ap->a_cookies != NULL || ap->a_ncookies != NULL) {
		return (EOPNOTSUPP);
	}

	//
	// Add . & .. entries
	// Unlike the original implementation, diroffset needs to be set to 0 for
	// reading actual directory entries. Instead of modifying uio_offset manually,
	// diroffset is "reset" to 0 by subtracting d_reclen of . and .. in
	// hfs_catalog/cat_getdirentries()
	//

	if (uio->uio_offset < dot.d_reclen) {
		dot.d_fileno = cp->c_cnid;
		if ((retval = uiomove((caddr_t)&dot, dot.d_reclen, uio))) {
			goto Exit;
		}
	}

	if (uio->uio_offset < 2 * dotdot.d_reclen) {
		dotdot.d_fileno = cp->c_parentcnid;
		if ((retval = uiomove((caddr_t)&dotdot, dotdot.d_reclen, uio))) {
			goto Exit;
		}
	}

	/* If there are no children then we're done */
	if (cp->c_entries == 0) {
		eofflag = 1;
		retval = 0;
		goto Exit;
	}

	/* Lock catalog b-tree */
	retval = hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_SHARED, p);
	if (retval) {
		goto Exit;
	}

	retval = cat_getdirentries(hfsmp, &cp->c_desc, uio, &eofflag);
	/* Unlock catalog b-tree */
	(void)hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);

	if (retval != E_NONE) {
		goto Exit;
	}

	/* were we already past eof ? */
	if (uio->uio_offset == off) {
		retval = E_NONE;
		goto Exit;
	}

	cp->c_flag |= C_ACCESS;

Exit:
	if (ap->a_eofflag)
		*ap->a_eofflag = eofflag;

	return (retval);
}

int
hfs_readlink(struct vop_readlink_args *ap)
{
	/* {
		struct vnode *a_vp;
		struct uio *a_uio;
		struct ucred *a_cred;
	} */
	int retval;
	struct vnode *vp = ap->a_vp;
	struct filefork *fp = VTOF(vp);

	if (vp->v_type != VLNK)
		return (EINVAL);

	/* Zero length sym links are not allowed */
	if (fp->ff_size == 0 || fp->ff_size > MAXPATHLEN) {
		VTOVCB(vp)->vcbFlags |= kHFS_DamagedVolume;
		return (EINVAL);
	}

	/* Cache the path so we don't waste buffer cache resources */
	if (fp->ff_symlinkptr == NULL) {
		struct buf *bp = NULL;

		fp->ff_symlinkptr = (char *)malloc(fp->ff_size, M_TEMP, M_WAITOK);
		retval = bread(vp, 0, roundup((int)fp->ff_size, VTOHFS(vp)->hfs_phys_block_size), ap->a_cred, &bp);
		if (retval) {
			if (bp)
				brelse(bp);
			if (fp->ff_symlinkptr) {
				free(fp->ff_symlinkptr, M_TEMP);
				fp->ff_symlinkptr = NULL;
			}
			return (retval);
		}
		bcopy(bp->b_data, fp->ff_symlinkptr, (size_t)fp->ff_size);
		if (bp) {
#ifdef DARWIN_JOURNAL
			if (VTOHFS(vp)->jnl && (bp->b_flags & B_LOCKED) == 0) {
				bp->b_flags |= B_INVAL; /* data no longer needed */
			}
#endif
			brelse(bp);
		}
	}
	retval = uiomove((caddr_t)fp->ff_symlinkptr, (int)fp->ff_size, ap->a_uio);

	return (retval);
}

int
hfs_write(struct vop_write_args *ap)
{
	/* struct vop_write_args {
		struct vnode *a_vp;
		struct uio *a_uio;
		int a_ioflag;
		struct ucred *a_cred;
	} */
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct cnode *cp;
	struct filefork *fp;
	struct buf *bp;
	proc_t *p;
	struct timeval tv;
	ExtendedVCB *vcb;
#ifdef DARWIN
	int devBlockSize = 0;
#else
	u_long logBlockSize;
#endif
	daddr_t logBlockNo;
	long fragSize;
	off_t origFileSize, currOffset, writelimit, bytesToAdd;
	off_t actualBytesAdded;
	u_long blkoffset, resid, xfersize, clearSize;
	int eflags, ioflag;
	int retval;
	off_t filebytes;
	u_long fileblocks;
#ifdef DARWIN_JOURNAL
	struct hfsmount *hfsmp;
	int started_tr = 0, grabbed_lock = 0;
#endif

	ioflag = ap->a_ioflag;

	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (E_NONE);
	if (vp->v_type != VREG && vp->v_type != VLNK)
		return (EISDIR); /* Can only write files */

	cp = VTOC(vp);
	fp = VTOF(vp);
	vcb = VTOVCB(vp);
	fileblocks = fp->ff_blocks;
	filebytes = (off_t)fileblocks * (off_t)vcb->blockSize;

	if (ioflag & IO_APPEND)
		uio->uio_offset = fp->ff_size;
	if ((cp->c_xflags & APPEND) && uio->uio_offset != fp->ff_size)
		return (EPERM);

#ifdef DARWIN_JOURNAL
	// XXXdbg - don't allow modification of the journal or
	// journal_info_block
	if (VTOHFS(vp)->jnl && cp->c_datafork) {
		struct HFSPlusExtentDescriptor *extd;

		extd = &cp->c_datafork->ff_data.cf_extents[0];
		if (extd->startBlock == VTOVCB(vp)->vcbJinfoBlock || extd->startBlock == VTOHFS(vp)->jnl_start) {
			return EPERM;
		}
	}
#endif

	writelimit = uio->uio_offset + uio->uio_resid;

	/*
	 * Maybe this should be above the vnode op call, but so long as
	 * file servers have no limits, I don't think it matters.
	 */
	p = uio->uio_td;
	if (vp->v_type == VREG && p != NULL) {
		PROC_LOCK(p->td_proc);
		if (writelimit > lim_cur(p /*->td_proc*/, RLIMIT_FSIZE)) {
			kern_psignal(p->td_proc, SIGXFSZ);
			PROC_UNLOCK(p->td_proc);
			return (EFBIG);
		}
		PROC_UNLOCK(p->td_proc);
	}
	p = curthread;

#ifdef DARWIN
	VOP_DEVBLOCKSIZE(cp->c_devvp, &devBlockSize);
#else
	logBlockSize = GetLogicalBlockSize(vp);
#endif

	resid = uio->uio_resid;
	origFileSize = fp->ff_size;
#ifdef DARWIN_DEFERRED_ALLOCATION
	eflags = kEFDeferMask; /* defer file block allocations */
#else
	eflags = 0;
#endif
	filebytes = (off_t)fp->ff_blocks * (off_t)vcb->blockSize;

	/*
	 * NOTE: In the following loop there are two positions tracked:
	 * currOffset is the current I/O starting offset.  currOffset
	 * is never >LEOF; the LEOF is nudged along with currOffset as
	 * data is zeroed or written. uio->uio_offset is the start of
	 * the current I/O operation.  It may be arbitrarily beyond
	 * currOffset.
	 *
	 * The following is true at all times:
	 *   currOffset <= LEOF <= uio->uio_offset <= writelimit
	 */
	currOffset = qmin(uio->uio_offset, fp->ff_size);

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 0)) | DBG_FUNC_START, (int)uio->uio_offset, uio->uio_resid, (int)fp->ff_size, (int)filebytes, 0);
	retval = 0;

	/* Now test if we need to extend the file */
	/* Doing so will adjust the filebytes for us */

#if QUOTA
	if (writelimit > filebytes) {
		bytesToAdd = writelimit - filebytes;

		retval = hfs_chkdq(cp, (int64_t)(roundup(bytesToAdd, vcb->blockSize)), ap->a_cred, 0);
		if (retval)
			return (retval);
	}
#endif /* QUOTA */

#ifdef DARWIN_JOURNAL
	hfsmp = VTOHFS(vp);
	if (writelimit > filebytes) {
		hfs_global_shared_lock_acquire(hfsmp);
		grabbed_lock = 1;
	}
	if (hfsmp->jnl && (writelimit > filebytes)) {
		if (journal_start_transaction(hfsmp->jnl) != 0) {
			hfs_global_shared_lock_release(hfsmp);
			return EINVAL;
		}
		started_tr = 1;
	}
#endif

	while (writelimit > filebytes) {
		bytesToAdd = writelimit - filebytes;
		if (priv_check_cred(ap->a_cred, PRIV_VFS_ADMIN) != 0)
			eflags |= kEFReserveMask;

		/* lock extents b-tree (also protects volume bitmap) */
		retval = hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_EXCLUSIVE, curthread);
		if (retval != E_NONE)
			break;

		retval = MacToVFSError(ExtendFileC(vcb, (FCB *)fp, bytesToAdd, 0, eflags, &actualBytesAdded));

		(void)hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_RELEASE, p);
		if ((actualBytesAdded == 0) && (retval == E_NONE))
			retval = ENOSPC;
		if (retval != E_NONE)
			break;
		filebytes = (off_t)fp->ff_blocks * (off_t)vcb->blockSize;
		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 0)) | DBG_FUNC_NONE, (int)uio->uio_offset, uio->uio_resid, (int)fp->ff_size, (int)filebytes, 0);
	}

#ifdef DARWIN_JOURNAL
	// XXXdbg
	if (started_tr) {
		hfs_flushvolumeheader(hfsmp, MNT_NOWAIT, 0);
		journal_end_transaction(hfsmp->jnl);
		started_tr = 0;
	}
	if (grabbed_lock) {
		hfs_global_shared_lock_release(hfsmp);
		grabbed_lock = 0;
	}
#endif

#ifdef DARWIN_UBC
	if (UBCISVALID(vp) && retval == E_NONE) {
		// off_t filesize;
		off_t zero_off;
		off_t tail_off;
		off_t inval_start;
		off_t inval_end;
		off_t io_start, io_end;
		int lflag;
		struct rl_entry *invalid_range;

		if (writelimit > fp->ff_size)
			filesize = writelimit;
		else
			filesize = fp->ff_size;

		lflag = (ioflag & IO_SYNC);

		if (uio->uio_offset <= fp->ff_size) {
			zero_off = uio->uio_offset & ~PAGE_MASK_64;

			/* Check to see whether the area between the zero_offset
			   and the start of the transfer to see whether is
			   invalid and should be zero-filled as part of the
			   transfer:
			 */
			if (rl_scan(&fp->ff_invalidranges, zero_off, uio->uio_offset - 1, &invalid_range) != RL_NOOVERLAP)
				lflag |= IO_HEADZEROFILL;
		} else {
			off_t eof_page_base = fp->ff_size & ~PAGE_MASK_64;

			/* The bytes between fp->ff_size and uio->uio_offset
			   must never be read without being zeroed.  The current
			   last block is filled with zeroes if it holds valid
			   data but in all cases merely do a little bookkeeping
			   to track the area from the end of the current last
			   page to the start of the area actually written.  For
			   the same reason only the bytes up to the start of the
			   page where this write will start is invalidated; any
			   remainder before uio->uio_offset is explicitly zeroed
			   as part of the cluster_write.

			   Note that inval_start, the start of the page after
			   the current EOF, may be past the start of the write,
			   in which case the zeroing will be handled by the
			   cluser_write of the actual data.
			 */
			inval_start = (fp->ff_size + (PAGE_SIZE_64 - 1)) & ~PAGE_MASK_64;
			inval_end = uio->uio_offset & ~PAGE_MASK_64;
			zero_off = fp->ff_size;

			if ((fp->ff_size & PAGE_MASK_64) && (rl_scan(&fp->ff_invalidranges, eof_page_base, fp->ff_size - 1, &invalid_range) != RL_NOOVERLAP)) {
				/* The page containing the EOF is not valid, so
				   the entire page must be made inaccessible
				   now.  If the write starts on a page beyond
				   the page containing the eof (inval_end >
				   eof_page_base), add the whole page to the
				   range to be invalidated.  Otherwise (i.e. if
				   the write starts on the same page), zero-fill
				   the entire page explicitly now:
				 */
				if (inval_end > eof_page_base) {
					inval_start = eof_page_base;
				} else {
					zero_off = eof_page_base;
				};
			};

			if (inval_start < inval_end) {
				/* There's some range of data that's going to be
				 * marked invalid */

				if (zero_off < inval_start) {
					/* The pages between inval_start and
					   inval_end are going to be
					   invalidated, and the actual write
					   will start on a page past inval_end.
					   Now's the last chance to zero-fill
					   the page containing the EOF:
					 */
					retval = cluster_write(vp, (struct uio *)0, fp->ff_size, inval_start, zero_off, (off_t)0, devBlockSize,
					    lflag | IO_HEADZEROFILL | IO_NOZERODIRTY);
					if (retval)
						goto ioerr_exit;
				};

				/* Mark the remaining area of the newly
				 * allocated space as invalid: */
				rl_add(inval_start, inval_end - 1, &fp->ff_invalidranges);
				cp->c_zftimeout = gettime() + ZFTIMELIMIT;
				zero_off = fp->ff_size = inval_end;
			};

			if (uio->uio_offset > zero_off)
				lflag |= IO_HEADZEROFILL;
		};

		/* Check to see whether the area between the end of the write
		   and the end of the page it falls in is invalid and should be
		   zero-filled as part of the transfer:
		 */
		tail_off = (writelimit + (PAGE_SIZE_64 - 1)) & ~PAGE_MASK_64;
		if (tail_off > filesize)
			tail_off = filesize;
		if (tail_off > writelimit) {
			if (rl_scan(&fp->ff_invalidranges, writelimit, tail_off - 1, &invalid_range) != RL_NOOVERLAP) {
				lflag |= IO_TAILZEROFILL;
			};
		};

		/*
		 * if the write starts beyond the current EOF (possibly advanced
		 * in the zeroing of the last block, above), then we'll zero
		 * fill from the current EOF to where the write begins:
		 *
		 * NOTE: If (and ONLY if) the portion of the file about to be
		 * written is before the current EOF it might be marked as
		 * invalid now and must be made readable (removed from the
		 * invalid ranges) before cluster_write tries to write it:
		 */
		io_start = (lflag & IO_HEADZEROFILL) ? zero_off : uio->uio_offset;
		io_end = (lflag & IO_TAILZEROFILL) ? tail_off : writelimit;
		if (io_start < fp->ff_size) {
			rl_remove(io_start, io_end - 1, &fp->ff_invalidranges);
		};
		retval = cluster_write(vp, uio, fp->ff_size, filesize, zero_off, tail_off, devBlockSize, lflag | IO_NOZERODIRTY);

		if (uio->uio_offset > fp->ff_size) {
			fp->ff_size = uio->uio_offset;

			ubc_setsize(vp, fp->ff_size); /* XXX check errors */
		}
		if (resid > uio->uio_resid)
			cp->c_flag |= C_CHANGE | C_UPDATE;
	} else {
#endif /* UBC */
		while (retval == E_NONE && uio->uio_resid > 0) {
			logBlockNo = currOffset / logBlockSize;
			blkoffset = currOffset % logBlockSize;

			if ((filebytes - currOffset) < logBlockSize)
				fragSize = filebytes - ((off_t)logBlockNo * logBlockSize);
			else
				fragSize = logBlockSize;
			xfersize = fragSize - blkoffset;

			/* Make any adjustments for boundary conditions */
			if (currOffset + (off_t)xfersize > writelimit)
				xfersize = writelimit - currOffset;

			/*
			 * There is no need to read into bp if:
			 * We start on a block boundary and will overwrite the
			 *whole block
			 *
			 *						OR
			 */
			if ((blkoffset == 0) && (xfersize >= fragSize)) {
				bp = _GETBLK(vp, logBlockNo, fragSize, 0, 0);
				retval = 0;

				if (bp->b_blkno == -1) {
					brelse(bp);
					retval = EIO; /* XXX */
					break;
				}
			} else {
				if (currOffset == fp->ff_size && blkoffset == 0) {
					bp = _GETBLK(vp, logBlockNo, fragSize, 0, 0);
					retval = 0;
					if (bp->b_blkno == -1) {
						brelse(bp);
						retval = EIO; /* XXX */
						break;
					}
				} else {
					/*
					 * This I/O transfer is not sufficiently
					 * aligned, so read the affected block
					 * into a buffer:
					 */
					retval = bread(vp, logBlockNo, fragSize, ap->a_cred, &bp);
					if (retval != E_NONE) {
						if (bp)
							brelse(bp);
						break;
					}
				}
			}

			/* See if we are starting to write within file
			 * boundaries: If not, then we need to present a "hole"
			 * for the area between the current EOF and the start of
			 * the current I/O operation:
			 *
			 * Note that currOffset is only less than uio_offset if
			 * uio_offset > LEOF...
			 */
			if (uio->uio_offset > currOffset) {
				clearSize = qmin(uio->uio_offset - currOffset, xfersize);
				bzero(bp->b_data + blkoffset, clearSize);
				currOffset += clearSize;
				blkoffset += clearSize;
				xfersize -= clearSize;
			}

			if (xfersize > 0) {
				retval = uiomove((caddr_t)bp->b_data + blkoffset, (int)xfersize, uio);
				currOffset += xfersize;
			}

			if (ioflag & IO_SYNC) {
				(void)bwrite(bp);
			} else if ((xfersize + blkoffset) == fragSize) {
				bp->b_flags |= B_AGE;
				bawrite(bp);
			} else {
				bdwrite(bp);
			}

			/* Update the EOF if we just extended the file
			 * (the PEOF has already been moved out and the
			 * block mapping table has been updated):
			 */
			if (currOffset > fp->ff_size) {
				fp->ff_size = currOffset;
#ifdef DARWIN_UBC
				if (UBCISVALID(vp))
					ubc_setsize(vp, fp->ff_size); /* XXX check errors */
#else
			vnode_pager_setsize(vp, fp->ff_size);
#endif
			}
			if (retval || (resid == 0))
				break;
			cp->c_flag |= C_CHANGE | C_UPDATE;
		} /* endwhile */
#ifdef DARWIN_UBC
	}

ioerr_exit:
#endif
	/*
	 * If we successfully wrote any data, and we are not the superuser
	 * we clear the setuid and setgid bits as a precaution against
	 * tampering.
	 */
	if (resid > uio->uio_resid && ap->a_cred && ap->a_cred->cr_uid != 0)
		cp->c_mode &= ~(S_ISUID | S_ISGID);

	if (retval) {
		if (ioflag & IO_UNIT) {
			(void)hfs_truncate(vp, origFileSize, ioflag & IO_SYNC, ap->a_cred, uio->uio_td);
			uio->uio_offset -= resid - uio->uio_resid;
			uio->uio_resid = resid;
			filebytes = (off_t)fp->ff_blocks * (off_t)vcb->blockSize;
		}
	} else if (resid > uio->uio_resid && (ioflag & IO_SYNC)) {
		getmicrotime(&tv);
		retval = hfs_update(vp, &tv, &tv, 1);
	}

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 0)) | DBG_FUNC_END, (int)uio->uio_offset, uio->uio_resid, (int)fp->ff_size, (int)filebytes, 0);

	return (retval);
}

int
hfs_ioctl(struct vop_ioctl_args *ap)
{
	/* {
	       struct vnode *a_vp;
	       int  a_command;
	       caddr_t  a_data;
	       int  a_fflag;
	       struct ucred *a_cred;
	       struct proc *a_p;
       } */
	return EOPNOTSUPP;
}

/*
 * Bmap converts a the logical block number of a file to its physical block
 * number on the disk.
 */

/*
 * vp  - address of vnode file the file
 * bn  - which logical block to convert to a physical block number.
 * vpp - returns the vnode for the block special file holding the filesystem
 *	 containing the file of interest
 * bnp - address of where to return the filesystem physical block number
#% bmap		vp	L L L
#% bmap		vpp	- U -
#
 vop_bmap {
     IN struct vnode *vp;
     IN daddr_t bn;
     OUT struct vnode **vpp;
     IN daddr_t *bnp;
     OUT int *runp;
     */
/*
 * Converts a logical block number to a physical block, and optionally returns
 * the amount of remaining blocks in a run. The logical block is based on
 * hfsNode.logBlockSize. The physical block number is based on the device block
 * size, currently its 512. The block run is returned in logical blocks, and is
 * the REMAINING amount of blocks
 */

int
hfs_bmap(struct vop_bmap_args *ap)
{
	/* struct vop_bmap_args {
		struct vnode *a_vp;
		daddr_t a_bn;
		struct vnode **a_vpp;
		daddr_t *a_bnp;
		int *a_runp;
	} */

	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct filefork *fp = VTOF(vp);
	struct hfsmount *hfsmp = VTOHFS(vp);

	int retval = E_NONE;
	daddr_t logBlockSize;
	size_t bytesContAvail = 0;
	off_t blockposition;
	proc_t *p = NULL;
	int lockExtBtree;
	struct rl_entry *invalid_range;
	enum rl_overlaptype overlaptype;

	/*
	 * Check for underlying vnode requests and ensure that logical
	 * to physical mapping is requested.
	 */

	// This might not be required... It also required changing...
	if (ap->a_vp == NULL)
		ap->a_vp = cp->c_devvp;

	if (ap->a_bnp == NULL)
		return (0);

	/* Only clustered I/O should have delayed allocations. */
	// DBG_ASSERT(fp->ff_unallocblocks == 0);

	logBlockSize = GetLogicalBlockSize(vp);
	blockposition = (off_t)ap->a_bn * (off_t)logBlockSize;

	lockExtBtree = overflow_extents(fp);
	if (lockExtBtree) {
		p = curthread;
		retval = hfs_metafilelocking(hfsmp, kHFSExtentsFileID, LK_EXCLUSIVE | LK_CANRECURSE, p);
		if (retval)
			return (retval);
	}

	retval = MacToVFSError(MapFileBlockC(HFSTOVCB(hfsmp), (FCB *)fp, MAXPHYSIO, blockposition, ap->a_bnp, &bytesContAvail));

	if (lockExtBtree) {
		(void)hfs_metafilelocking(hfsmp, kHFSExtentsFileID, LK_RELEASE, p);
	}

	if (retval == E_NONE) {
		/* Adjust the mapping information for invalid file ranges: */
		overlaptype = rl_scan(&fp->ff_invalidranges, blockposition, blockposition + MAXPHYSIO - 1, &invalid_range);

		if (overlaptype != RL_NOOVERLAP) {
			switch (overlaptype) {
			case RL_MATCHINGOVERLAP:
			case RL_OVERLAPCONTAINSRANGE:
			case RL_OVERLAPSTARTSBEFORE:
				/* There's no valid block for this byte
				 * offset: */

				*ap->a_bnp = (daddr_t)-1;
				bytesContAvail = invalid_range->rl_end + 1 - blockposition;
				break;

			case RL_OVERLAPISCONTAINED:
			case RL_OVERLAPENDSAFTER:
				/* The range of interest hits an invalid
				 * block before the end: */
				if (invalid_range->rl_start == blockposition) {
					/* There's actually no valid
					 * information to be had
					 * starting here: */
					*ap->a_bnp = (daddr_t)-1;
					if ((fp->ff_size > (invalid_range->rl_end + 1)) && (invalid_range->rl_end + 1 - blockposition < bytesContAvail)) {
						bytesContAvail = invalid_range->rl_end + 1 - blockposition;
					};
				} else {
					bytesContAvail = invalid_range->rl_start - blockposition;
				};
				break;
			default:
				break;
			};
			if (bytesContAvail > MAXPHYSIO)
				bytesContAvail = MAXPHYSIO;
		};

		/* Figure out how many read ahead blocks there are */
		if (ap->a_runp != NULL) {
			if (can_cluster(logBlockSize)) {
				/* Make sure this result never goes negative: */
				*ap->a_runp = (bytesContAvail < logBlockSize) ? 0 : (bytesContAvail / logBlockSize) - 1;
			} else {
				*ap->a_runp = 0;
			};
		};
#ifndef DARWIN
		if (ap->a_runb != NULL)
			*ap->a_runb = 0;
#endif
	};

	return (retval);
}

/*
 * Calculate the logical to physical mapping if not done already,
 * then call the device strategy routine.
#
#vop_strategy {
#	IN struct buf *bp;
    */
int
hfs_strategy(struct vop_strategy_args *ap)
{
	/* {
		struct buf   *a_bp;
		struct vnode *a_vp;
	} */

	struct buf *bp = ap->a_bp;
	struct vnode *vp = ap->a_vp;
	int retval = 0;

	if (vp->v_type == VBLK || vp->v_type == VCHR) {
		printf("hfs_strategy: device vnode passed!");
		return (0);
	}

	/*
	 * If we don't already know the filesystem relative block
	 * number then get it using VOP_BMAP().  If VOP_BMAP()
	 * returns the block number as -1 then we've got a hole in
	 * the file.  Although HFS filesystems don't create files with
	 * holes, invalidating of subranges of the file (lazy zero
	 * filling) may create such a situation.
	 */
	if (bp->b_blkno == bp->b_lblkno) {
		if ((retval = VOP_BMAP(vp, bp->b_lblkno, NULL, &bp->b_blkno, NULL, NULL))) {
			bp->b_error = retval;
			bp->b_ioflags |= BIO_ERROR;
			bufdone(bp);
			return (retval);
		}

		if ((long)bp->b_blkno == -1)
			vfs_bio_clrbuf(bp);
	}
	if ((long)bp->b_blkno == -1) {
		bufdone(bp);
		return (0);
	}

	bp->b_iooffset = dbtob(bp->b_blkno);

	BO_STRATEGY(VFSTOHFS(vp->v_mount)->hfs_bo, bp);
	return (0);
}

int
hfs_truncate(struct vnode *vp, off_t length, int flags, struct ucred *cred, struct thread *td)
{
	register struct cnode *cp = VTOC(vp);
	struct filefork *fp = VTOF(vp);
	struct timeval tv;
	int retval;
	off_t bytesToAdd;
	off_t actualBytesAdded;
	off_t filebytes;
	u_long fileblocks;
	int blksize;
	proc_t *p = curthread;

	if (VTOVFS(vp)->mnt_flag & MNT_RDONLY) /* YYY wasn't there */
		return (EROFS);

	if (vp->v_type != VREG && vp->v_type != VLNK)
		return (EISDIR); /* cannot truncate an HFS directory! */

	blksize = VTOVCB(vp)->blockSize;
	fileblocks = fp->ff_blocks;
	filebytes = (off_t)fileblocks * (off_t)blksize;

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 7)) | DBG_FUNC_START, (int)length, (int)fp->ff_size, (int)filebytes, 0, 0);

	if (length < 0)
		return (EINVAL);

	if ((!ISHFSPLUS(VTOVCB(vp))) && (length > (off_t)MAXHFSFILESIZE))
		return (EFBIG);

	getmicrotime(&tv);
	retval = E_NONE;

	/*
	 * We cannot just check if fp->ff_size == length (as an optimization)
	 * since there may be extra physical blocks that also need truncation.
	 */
#if QUOTA
	if ((retval = hfs_getinoquota(cp)))
		return (retval);
#endif /* QUOTA */

	/*
	 * Lengthen the size of the file. We must ensure that the
	 * last byte of the file is allocated. Since the smallest
	 * value of ff_size is 0, length will be at least 1.
	 */
	if (length > fp->ff_size) {
#if QUOTA
		retval = hfs_chkdq(cp, (int64_t)(roundup(length - filebytes, blksize)), cred, 0);
		if (retval)
			goto Err_Exit;
#endif /* QUOTA */
		/*
		 * If we don't have enough physical space then
		 * we need to extend the physical size.
		 */
		if (length > filebytes) {
			int eflags;

			/* All or nothing and don't round up to clumpsize. */
			eflags = kEFAllMask | kEFNoClumpMask;

			if (suser_cred(cred, 0) != 0)
				eflags |= kEFReserveMask; /* keep a reserve */

#ifdef DARWIN_JOURNAL
			// XXXdbg
			hfs_global_shared_lock_acquire(hfsmp);
			if (hfsmp->jnl) {
				if (journal_start_transaction(hfsmp->jnl) != 0) {
					retval = EINVAL;
					goto Err_Exit;
				}
			}
#endif

			/* lock extents b-tree (also protects volume bitmap) */
			retval = hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_EXCLUSIVE, p);
			if (retval) {
#ifdef DARWIN_JOURNAL
				if (hfsmp->jnl) {
					journal_end_transaction(hfsmp->jnl);
				}
				hfs_global_shared_lock_release(hfsmp);
#endif

				goto Err_Exit;
			}

			while ((length > filebytes) && (retval == E_NONE)) {
				bytesToAdd = length - filebytes;
				retval = MacToVFSError(ExtendFileC(VTOVCB(vp), (FCB *)fp, bytesToAdd, 0, eflags, &actualBytesAdded));

				filebytes = (off_t)fp->ff_blocks * (off_t)blksize;
				if (actualBytesAdded == 0 && retval == E_NONE) {
					if (length > filebytes)
						length = filebytes;
					break;
				}
			} /* endwhile */

			(void)hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_RELEASE, p);

#ifdef DARWIN_JOURNAL
			// XXXdbg
			if (hfsmp->jnl) {
				hfs_flushvolumeheader(hfsmp, MNT_NOWAIT, 0);
				journal_end_transaction(hfsmp->jnl);
			}
			hfs_global_shared_lock_release(hfsmp);
#endif

			if (retval)
				goto Err_Exit;

			KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 7)) | DBG_FUNC_NONE, (int)length, (int)fp->ff_size, (int)filebytes, 0, 0);
		}

#ifdef DARWIN /* ... and UBC as well in one cut */
		if (!(ap->a_flags & IO_NOZEROFILL)) {
			if (UBCINFOEXISTS(vp) && retval == E_NONE) {
				struct rl_entry *invalid_range;
				int devBlockSize;
				off_t zero_limit;

				zero_limit = (fp->ff_size + (PAGE_SIZE_64 - 1)) & ~PAGE_MASK_64;
				if (length < zero_limit)
					zero_limit = length;

				if (length > fp->ff_size) {
					/* Extending the file: time to fill out
					 * the current last page w. zeroes? */
					if ((fp->ff_size & PAGE_MASK_64) &&
					    (rl_scan(&fp->ff_invalidranges, fp->ff_size & ~PAGE_MASK_64, fp->ff_size - 1, &invalid_range) == RL_NOOVERLAP)) {
						/* There's some valid data at
						   the start of the (current)
						   last page of the file, so
						   zero out the remainder of
						   that page to ensure the
						   entire page contains valid
						   data.  Since there is no
						   invalid range possible past
						   the (current) eof, there's no
						   need to remove anything from
						   the invalid range list before
						   calling cluster_write():
						 */
						VOP_DEVBLOCKSIZE(cp->c_devvp, &devBlockSize);
						retval = cluster_write(vp, (struct uio *)0, fp->ff_size, zero_limit, fp->ff_size, (off_t)0, devBlockSize,
						    (ap->a_flags & IO_SYNC) | IO_HEADZEROFILL | IO_NOZERODIRTY);
						if (retval)
							goto Err_Exit;

						/* Merely invalidate the
						 * remaining area, if necessary:
						 */
						if (length > zero_limit) {
							rl_add(zero_limit, length - 1, &fp->ff_invalidranges);
							cp->c_zftimeout = gettime() + ZFTIMELIMIT;
						}
					} else {
						/* The page containing the
						   (current) eof is invalid:
						   just add the remainder of the
						   page to the invalid list,
						   along with the area being
						   newly allocated:
						 */
						rl_add(fp->ff_size, length - 1, &fp->ff_invalidranges);
						cp->c_zftimeout = gettime() + ZFTIMELIMIT;
					};
				}
			} else {
				panic("hfs_truncate: invoked on non-UBC "
				      "object?!");
			};
		}
#else  /* !DARWIN */
		{
			int lblksize = GetLogicalBlockSize(vp);
			int blkoff, blkzeros;
			daddr_t lblkno;
			off_t bytestoclear = length - fp->ff_size;
			off_t filepos = fp->ff_size;
			struct buf *bp;

			while (bytestoclear > 0) {
				lblkno = filepos / lblksize;
				blkoff = filepos % lblksize;
				blkzeros = qmin(bytestoclear, lblksize - blkoff);

				if (blkoff == 0 && bytestoclear >= lblksize) {
					bp = _GETBLK(vp, lblkno, lblksize, 0, 0);
				} else {
					retval = bread(vp, lblkno, lblksize, cred, &bp);
					if (retval) {
						brelse(bp);
						goto Err_Exit;
					}
				}
				bzero((char *)bp->b_data + blkoff, blkzeros);
				bp->b_flags |= BX_VNDIRTY | B_AGE;
				if (flags & IO_SYNC)
					bwrite(bp);
				else
					bawrite(bp);
				bytestoclear -= blkzeros;
				filepos += blkzeros;
			}
		}
#endif /* DARWIN */
		cp->c_flag |= C_UPDATE;
		fp->ff_size = length;

#ifdef DARWIN_UBC
		if (UBCISVALID(vp))
			ubc_setsize(vp, fp->ff_size); /* XXX check errors */
#else
		vnode_pager_setsize(vp, fp->ff_size);
#endif

	} else { /* Shorten the size of the file */

		if (fp->ff_size > length) {
#ifdef DARWIN
			/*
			 * Any buffers that are past the truncation point need
			 * to be invalidated (to maintain buffer cache
			 * consistency).  For simplicity, we invalidate all the
			 * buffers by calling vinvalbuf.
			 */
			if (UBCISVALID(vp))
				ubc_setsize(vp, length); /* XXX check errors */

			vflags = ((length > 0) ? V_SAVE : 0) | V_SAVEMETA;
			retval = vinvalbuf(vp, vflags, ap->a_cred, ap->a_p, 0, 0);
#else /* FreeBSD has a better way */
			/*
			 * Logical block size is used since vtruncbuf() operates
			 * on buffers, not on allocation.
			 * NB: vtruncbuf() does vnode_pager_setsize().
			 */
			retval = vtruncbuf(vp, length, GetLogicalBlockSize(vp));
#endif

			/* Any space previously marked as invalid is now
			 * irrelevant: */
			rl_remove(length, fp->ff_size - 1, &fp->ff_invalidranges);
		}

		/*
		 * Account for any unmapped blocks. Note that the new
		 * file length can still end up with unmapped blocks.
		 */
		if (fp->ff_unallocblocks > 0) {
			u_int32_t finalblks;

			/* lock extents b-tree */
			retval = hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_EXCLUSIVE, p);
			if (retval)
				goto Err_Exit;

			VTOVCB(vp)->loanedBlocks -= fp->ff_unallocblocks;
			cp->c_blocks -= fp->ff_unallocblocks;
			fp->ff_blocks -= fp->ff_unallocblocks;
			fp->ff_unallocblocks = 0;

			finalblks = (length + blksize - 1) / blksize;
			if (finalblks > fp->ff_blocks) {
				/* calculate required unmapped blocks */
				fp->ff_unallocblocks = finalblks - fp->ff_blocks;
				VTOVCB(vp)->loanedBlocks += fp->ff_unallocblocks;
				cp->c_blocks += fp->ff_unallocblocks;
				fp->ff_blocks += fp->ff_unallocblocks;
			}
			(void)hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_RELEASE, p);
		}

		/*
		 * For a TBE process the deallocation of the file blocks is
		 * delayed until the file is closed.  And hfs_close calls
		 * truncate with the IO_NDELAY flag set.  So when IO_NDELAY
		 * isn't set, we make sure this isn't a TBE process.
		 */
#ifdef DARWIN
		if ((ap->a_flags & IO_NDELAY) || (!ISSET(ap->a_p->p_flag, P_TBE))) {
#else
		if ((flags & IO_NDELAY)) {
#endif
#if QUOTA
			off_t savedbytes = ((off_t)fp->ff_blocks * (off_t)blksize);
#endif /* QUOTA */
#ifdef DARWIN_JOURNAL
			// XXXdbg
			hfs_global_shared_lock_acquire(hfsmp);
			if (hfsmp->jnl) {
				if (journal_start_transaction(hfsmp->jnl) != 0) {
					retval = EINVAL;
					goto Err_Exit;
				}
			}
#endif

			/* lock extents b-tree (also protects volume bitmap) */
			retval = hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_EXCLUSIVE, p);
			if (retval) {
#ifdef DARWIN_JOURNAL
				if (hfsmp->jnl) {
					journal_end_transaction(hfsmp->jnl);
				}
				hfs_global_shared_lock_release(hfsmp);
#endif
				goto Err_Exit;
			}

			if (fp->ff_unallocblocks == 0)
				retval = MacToVFSError(TruncateFileC(VTOVCB(vp), (FCB *)fp, length, false));

			(void)hfs_metafilelocking(VTOHFS(vp), kHFSExtentsFileID, LK_RELEASE, p);

#ifdef DARWIN_JOURNAL
			// XXXdbg
			if (hfsmp->jnl) {
				hfs_flushvolumeheader(hfsmp, MNT_NOWAIT, 0);
				journal_end_transaction(hfsmp->jnl);
			}
			hfs_global_shared_lock_release(hfsmp);
#endif

			filebytes = (off_t)fp->ff_blocks * (off_t)blksize;
			if (retval)
				goto Err_Exit;
#if QUOTA
			/* These are bytesreleased */
			(void)hfs_chkdq(cp, (int64_t)-(savedbytes - filebytes), NOCRED, 0);
#endif /* QUOTA */
		}
		/* Only set update flag if the logical length changes */
		if (fp->ff_size != length)
			cp->c_flag |= C_UPDATE;
		fp->ff_size = length;
	}
	cp->c_flag |= C_CHANGE;
	retval = hfs_update(vp, &tv, &tv, MNT_WAIT);
	if (retval) {
		KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 7)) | DBG_FUNC_NONE, -1, -1, -1, retval, 0);
	}

Err_Exit:

	KERNEL_DEBUG((FSDBG_CODE(DBG_FSRW, 7)) | DBG_FUNC_END, (int)length, (int)fp->ff_size, (int)filebytes, retval, 0);

	return (retval);
}

void
hfs_bstrategy(struct bufobj *bo, struct buf *bp)
{
	KASSERT(bo->bo_private != NULL, ("bo_private is null."));
	struct vnode *devvp;
	devvp = (struct vnode *)bo->bo_private;
	KASSERT(devvp != NULL, ("devvp is null."));

	VOP_STRATEGY(devvp, bp);
}

/*
 * Intercept B-Tree node writes to unswap them if necessary.
#
#vop_bwrite {
#	IN struct buf *bp;
 */
int
hfs_bwrite(struct buf *bp)
{
	int retval = 0;
	struct vnode *vp = bp->b_vp;
#if BYTE_ORDER == LITTLE_ENDIAN
	BlockDescriptor block;

	/* Trap B-Tree writes */
	if ((VTOC(vp)->c_fileid == kHFSExtentsFileID) || (VTOC(vp)->c_fileid == kHFSCatalogFileID)) {
		/* Swap if the B-Tree node is in native byte order */
		if (((UInt16 *)((char *)bp->b_data + bp->b_bcount - 2))[0] == 0x000e) {
			/* Prepare the block pointer */
			block.blockHeader = bp;
			block.buffer = bp->b_data;
			/* not found in cache ==> came from disk */
			block.blockReadFromDisk = (bp->b_flags & B_CACHE) == 0;
			block.blockSize = bp->b_bcount;

			/* Endian un-swap B-Tree node */
			SWAP_BT_NODE(&block, ISHFSPLUS(VTOVCB(vp)), VTOC(vp)->c_fileid, 1);
		}

		/* We don't check to make sure that it's 0x0e00 because it could
		 * be all zeros */
	}
#endif

	retval = buf_ops_bio.bop_write(bp); /* YYY need buf op stacking */
	return (retval);
}
