#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/lockf.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <sys/kdb.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h>
#include <vm/vnode_pager.h>

#include <hfsplus/hfs.h>
#include <hfsplus/hfs_catalog.h>
#include <hfsplus/hfs_cnode.h>
#include <hfsplus/hfs_dbg.h>
#include <hfsplus/hfs_endian.h>
#include <hfsplus/hfs_mount.h>
#include <hfsplus/hfs_quota.h>

#include "hfscommon/headers/BTreesInternal.h"
#include "hfscommon/headers/FileMgrInternal.h"

/* hfs_readwrite.c */
int hfs_bmap(struct vop_bmap_args *);
int hfs_strategy(struct vop_strategy_args *);
int hfs_read(struct vop_read_args *);
int hfs_readdir(struct vop_readdir_args *);
int hfs_readlink(struct vop_readlink_args *);
int hfs_ioctl(struct vop_ioctl_args *);

/* hfs_cnode.c */
int hfs_reclaim(struct vop_reclaim_args *);
int hfs_inactive(struct vop_inactive_args *);

/* hfs_attr.c */
int hfs_setattr(struct vop_setattr_args *);
int hfs_getattr(struct vop_getattr_args *);

static int hfs_metasync(struct hfsmount *hfsmp, daddr_t node, proc_t *p);

int
hfs_write_access(struct vnode *vp, struct ucred *cred, Boolean considerFlags)
{
	struct cnode *cp = VTOC(vp);
	gid_t *gp;
	int retval = 0;
	int i;

	/*
	 * Disallow write attempts on read-only file systems;
	 * unless the file is a socket, fifo, or a block or
	 * character device resident on the file system.
	 */
	switch (vp->v_type) {
	case VDIR:
 	case VLNK:
	case VREG:
		if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
			return (EROFS);
        break;
	default:
		break;
 	}
 
	/* If immutable bit set, nobody gets to write it. */
	if (considerFlags && (cp->c_xflags & IMMUTABLE))
		return (EPERM);

	/* Otherwise, user id 0 always gets access. */
	if (cred->cr_uid == 0)
		return (0);

	/* Otherwise, check the owner. */
	if ((retval = hfs_owner_rights(VTOHFS(vp), cp->c_uid, cred, false)) == 0)
		return ((cp->c_mode & S_IWUSR) == S_IWUSR ? 0 : EACCES);
 
	/* Otherwise, check the groups. */
	for (i = 0, gp = cred->cr_groups; i < cred->cr_ngroups; i++, gp++) {
		if (cp->c_gid == *gp)
			return ((cp->c_mode & S_IWGRP) == S_IWGRP ? 0 : EACCES);
 	}
 
	/* Otherwise, check everyone else. */
	return ((cp->c_mode & S_IWOTH) == S_IWOTH ? 0 : EACCES);
}

static int
hfs_open(struct vop_open_args *ap)
{
	/* {
		struct vnode *a_vp;
		int  a_mode;
		struct ucred *a_cred;
		proc_t *a_td;
	} */
	struct vnode *vp = ap->a_vp;

	/*
	 * Files marked append-only must be opened for appending.
	 */
	if ((vp->v_type != VDIR) && (VTOC(vp)->c_xflags & APPEND) && (ap->a_mode & (FWRITE | O_APPEND)) == FWRITE) {
		return (EPERM);
	}

	return (0);
}

static int
hfs_close(struct vop_close_args *ap)
{
	register struct vnode *vp = ap->a_vp;
	register struct cnode *cp = VTOC(vp);
	register struct filefork *fp = VTOF(vp);
	proc_t *p = ap->a_td;
	struct timeval tv;
	off_t leof;
	u_long blks, blocksize;

	int error;

	VI_LOCK(vp);

	if (vp->v_usecount > 1) {
		getmicrotime(&tv);
		CTIMES(cp, &tv, &tv);
	}
	VI_UNLOCK(vp);

	/*
	 * VOP_CLOSE can be called with vp locked (from vclean).
	 * We check for this case using VOP_ISLOCKED and bail.
	 *
	 * XXX During a force unmount we won't do the cleanup below!
	 */
	if (vp->v_type == VDIR || VOP_ISLOCKED(vp))
		return (0);

	leof = fp->ff_size;

	if ((fp->ff_blocks > 0) && !ISSET(cp->c_flag, C_DELETED)) {
		// int our_type = vp->v_type;
		// u_long our_id = vp->v_id;
		//
		vref(vp);
		error = vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		if (error)
			return (0);
		/*
		 * Since we can context switch in vn_lock our vnode
		 * could get recycled (eg umount -f).  Double check
		 * that its still ours.
		 */

		// if (vp->v_type != our_type || vp->v_id != our_id
		//     || cp != VTOC(vp)) {
		//	VOP_UNLOCK(vp);
		//	return (0);
		// }

		cp->c_flag &= ~C_ZFWANTSYNC;
		cp->c_zftimeout = 0;
		blocksize = VTOVCB(vp)->blockSize;
		blks = leof / blocksize;
		if (((off_t)blks * (off_t)blocksize) != leof)
			blks++;
		/*
		 * Shrink the peof to the smallest size neccessary to contain the leof.
		 */
		if (blks < fp->ff_blocks)
			(void)hfs_truncate(vp, leof, IO_NDELAY, ap->a_cred, p);

		/*
		 * If the VOP_TRUNCATE didn't happen to flush the vnode's
		 * information out to disk, force it to be updated now that
		 * all invalid ranges have been zero-filled and validated:
		 */
		if (cp->c_flag & C_MODIFIED) {
			getmicrotime(&tv);
			hfs_update(vp, &tv, &tv, 0);
		}
		VOP_UNLOCK(vp);
		vrele(vp);
	}
	return (0);
}

static int
hfs_pathconf(struct vop_pathconf_args *ap)
{
	/* {
		struct vnode *a_vp;
		int a_name;
		int *a_retval;
	} */
	int retval = 0;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		if (VTOVCB(ap->a_vp)->vcbSigWord == kHFSPlusSigWord)
			*ap->a_retval = HFS_LINK_MAX;
		else
			*ap->a_retval = 1;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = kHFSPlusMaxFileNameBytes; /* max # of characters x max utf8 representation */
		break;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX; /* 1024 */
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 0;
		break;
	default:
		retval = EINVAL;
	}

	return (retval);
}

int
hfs_access(struct vop_access_args *ap)
{
	/* {
		struct vnode *a_vp;
		int a_mode;
		struct ucred *a_cred;
		proc_t *a_td;
	} */
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct ucred *cred = ap->a_cred;
	register gid_t *gp;
	mode_t mode = ap->a_accmode;
	mode_t mask = 0;
	int i;
	// int error;
	// proc_t *p = curthread;

	/*
	 * Disallow write attempts on read-only file systems;
	 * unless the file is a socket, fifo, or a block or
	 * character device resident on the file system.
	 */
	if (mode & VWRITE) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
	}

	/* If immutable bit set, nobody gets to write it. */
	if ((mode & VWRITE) && (cp->c_xflags & IMMUTABLE))
		return (EPERM);

	/* Otherwise, user id 0 always gets access. */
	if (ap->a_cred->cr_uid == 0)
		return (0);

	mask = 0;

	/* Otherwise, check the owner. */
	if (hfs_owner_rights(VTOHFS(vp), cp->c_uid, cred, false) == 0) {
		if (mode & VEXEC)
			mask |= S_IXUSR;
		if (mode & VREAD)
			mask |= S_IRUSR;
		if (mode & VWRITE)
			mask |= S_IWUSR;
		return ((cp->c_mode & mask) == mask ? 0 : EACCES);
	}

	/* Otherwise, check the groups. */
	if (!(VTOVFS(vp)->mnt_flag & MNT_UNKNOWNPERMISSIONS)) {
		for (i = 0, gp = cred->cr_groups; i < cred->cr_ngroups; i++, gp++)
			if (cp->c_gid == *gp) {
				if (mode & VEXEC)
					mask |= S_IXGRP;
				if (mode & VREAD)
					mask |= S_IRGRP;
				if (mode & VWRITE)
					mask |= S_IWGRP;
				return ((cp->c_mode & mask) == mask ? 0 : EACCES);
			}
	}

	/* Otherwise, check everyone else. */
	if (mode & VEXEC)
		mask |= S_IXOTH;
	if (mode & VREAD)
		mask |= S_IROTH;
	if (mode & VWRITE)
		mask |= S_IWOTH;
	return ((cp->c_mode & mask) == mask ? 0 : EACCES);
}

static int
hfs_islocked(struct vop_islocked_args *ap)
{
	/* {
		struct vnode *a_vp;
		proc_t *a_td;
	} */
	// return (lockstatus(&VTOC(ap->a_vp)->c_lock));
	return (lockstatus(ap->a_vp->v_vnlock));
}

static int
hfs_fsync(struct vop_fsync_args *ap)
{
	 /* {
		struct vnode *a_vp;
		struct ucred *a_cred;
		int a_waitfor;
		proc_t *a_td;
	} */ 
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
#ifdef DARWIN_UBC
	struct filefork *fp = NULL;
#endif
	int retval = 0;
	// register struct buf *bp;
	struct timeval tv;
	// struct buf *nbp;
	// struct hfsmount *hfsmp = VTOHFS(ap->a_vp);
	// int s;
	int wait;
	// int retry = 0;

	wait = (ap->a_waitfor == MNT_WAIT);

	/* HFS directories don't have any data blocks. */
	if (vp->v_type == VDIR)
		goto metasync;

	/*
	 * For system files flush the B-tree header and
	 * for regular files write out any clusters
	 */
	if (vp->v_vflag & VV_SYSTEM) {
	    if (VTOF(vp)->fcbBTCBPtr != NULL) {
#ifdef DARWIN_JOURNAL
			// XXXdbg
			if (hfsmp->jnl) {
				if (BTIsDirty(VTOF(vp))) {
					panic("hfs: system file vp 0x%x has dirty blocks (jnl 0x%x)\n",
						  vp, hfsmp->jnl);
				}
			} else
#endif /* DARWIN_JOURNAL */
			{
				BTFlushPath(VTOF(vp));
			}
	    }
	}
#ifdef DARWIN_UBC
	else if (UBCINFOEXISTS(vp))
		(void) cluster_push(vp);
#endif /* DARWIN_UBC */

#ifdef DARWIN_UBC
	/*
	 * When MNT_WAIT is requested and the zero fill timeout
	 * has expired then we must explicitly zero out any areas
	 * that are currently marked invalid (holes).
	 *
	 * Files with NODUMP can bypass zero filling here.
	 */
	if ((wait || (cp->c_flag & C_ZFWANTSYNC)) &&
	    ((cp->c_flags & UF_NODUMP) == 0) &&
	    UBCINFOEXISTS(vp) && (fp = VTOF(vp)) &&
	    cp->c_zftimeout != 0) {
		int devblksize;
		int was_nocache;

		if (gettime() < cp->c_zftimeout) {
			/* Remember that a force sync was requested. */
			cp->c_flag |= C_ZFWANTSYNC;
			goto loop;
		}	
		VOP_DEVBLOCKSIZE(cp->c_devvp, &devblksize);
		was_nocache = ISSET(vp->v_flag, VNOCACHE_DATA);
		SET(vp->v_flag, VNOCACHE_DATA);	/* Don't cache zeros */

		while (!TAILQ_EMPTY(&fp->ff_invalidranges)) {
			struct rl_entry *invalid_range = TAILQ_FIRST(&fp->ff_invalidranges);
			off_t start = invalid_range->rl_start;
			off_t end = invalid_range->rl_end;
    		
			/* The range about to be written must be validated
			 * first, so that VOP_CMAP() will return the
			 * appropriate mapping for the cluster code:
			 */
			rl_remove(start, end, &fp->ff_invalidranges);

			(void) cluster_write(vp, (struct uio *) 0,
					fp->ff_size,
					invalid_range->rl_end + 1,
					invalid_range->rl_start,
					(off_t)0, devblksize,
					IO_HEADZEROFILL | IO_NOZERODIRTY);
			cp->c_flag |= C_MODIFIED;
		}
		(void) cluster_push(vp);
		if (!was_nocache)
			CLR(vp->v_flag, VNOCACHE_DATA);
		cp->c_flag &= ~C_ZFWANTSYNC;
		cp->c_zftimeout = 0;
	}
#endif /* DARWIN_UBC */

	/*
	 * Flush all dirty buffers associated with a vnode.
	 */
#ifdef DARWIN
	VI_LOCK(vp);
loop:
	// s = splbio();
	for (bp = TAILQ_FIRST(&vp->v_dirtyblkhd); bp; bp = nbp) {
		nbp = TAILQ_NEXT(bp, b_vnbufs);
		if (_BUF_LOCK(bp, LK_EXCLUSIVE | LK_NOWAIT))
			continue;
		VI_UNLOCK(vp);
		if ((bp->b_flags & B_DELWRI) == 0)
			panic("hfs_fsync: bp 0x%x not dirty (hfsmp 0x%x)", bp, hfsmp);
#ifdef DARWIN_JOURNAL
		// XXXdbg
		if (hfsmp->jnl && (bp->b_flags & B_LOCKED)) {
			if ((bp->b_flags & B_META) == 0) {
				panic("hfs: bp @ 0x%x is locked but not meta! jnl 0x%x\n",
					  bp, hfsmp->jnl);
			}
			// if journal_active() returns >= 0 then the journal is ok and we 
			// shouldn't do anything to this locked block (because it is part 
			// of a transaction).  otherwise we'll just go through the normal 
			// code path and flush the buffer.
			if (journal_active(hfsmp->jnl) >= 0) {
				continue;
			}
		}
#endif /* DARWIN_JOURNAL */

		bremfree(bp);
#ifdef DARWIN
		bp->b_flags |= B_BUSY;
		/* Clear B_LOCKED, should only be set on meta files */
		bp->b_flags &= ~B_LOCKED;
#endif

		// splx(s);
		/*
		 * Wait for I/O associated with indirect blocks to complete,
		 * since there is no way to quickly wait for them below.
		 */
		if (bp->b_vp != vp) /* YYY */
			printf("hfs_fsync: bp->b_vp != vp\n");
		if (bp->b_vp == vp || ap->a_waitfor == MNT_NOWAIT)
			(void) bawrite(bp);
		else
			(void) bwrite(bp);
		VI_LOCK(vp);
		goto loop;
	}

	if (wait) {
		while (vp->v_numoutput) {
			vp->v_iflag |= VI_BWAIT;
			msleep((caddr_t)&vp->v_numoutput, VI_MTX(vp),
				PRIBIO + 1, "hfs_fsync", 0);
		}

		// XXXdbg -- is checking for hfsmp->jnl == NULL the right
		//           thing to do?
#ifdef DARWIN_JOURNAL
		if (hfsmp->jnl == NULL && vp->v_dirtyblkhd.lh_first) {
#else
		if (!TAILQ_EMPTY(&vp->v_dirtyblkhd)) {
#endif
			/* still have some dirty buffers */
			if (retry++ > 10) {
				vprint("hfs_fsync: dirty", vp);
				// splx(s);
				/*
				 * Looks like the requests are not
				 * getting queued to the driver.
				 * Retrying here causes a cpu bound loop.
				 * Yield to the other threads and hope
				 * for the best.
				 */
				(void)msleep((caddr_t)&vp->v_numoutput, VI_MTX(vp),
					PRIBIO + 1, "hfs_fsync", hz/10);
				retry = 0;
			} else {
				// splx(s);
			}
			/* try again */
			goto loop;
		}
	}
	VI_UNLOCK(vp);
	splx(s);
#else /* !DARWIN */
	vop_stdfsync(ap);
#endif /* DARWIN */

metasync:
   	getmicrotime(&tv);
	if (vp->v_vflag & VV_SYSTEM) {
		if (VTOF(vp)->fcbBTCBPtr != NULL)
			BTSetLastSync(VTOF(vp), tv.tv_sec);
		cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE);
	} else /* User file */ {
		retval = hfs_update(ap->a_vp, &tv, &tv, wait);

		/* When MNT_WAIT is requested push out any delayed meta data */
   		if ((retval == 0) && wait && cp->c_hint &&
   		    !ISSET(cp->c_flag, C_DELETED | C_NOEXISTS)) {
   			hfs_metasync(VTOHFS(vp), cp->c_hint, ap->a_td);
   		}
	}

	return (retval);
}

static int
hfs_metasync(struct hfsmount *hfsmp, daddr_t node, proc_t *p)
{
	struct vnode *vp;
	struct buf *bp;
	struct buf *nbp;
	// int s;

	vp = HFSTOVCB(hfsmp)->catalogRefNum;

#ifdef DARWIN_JOURNAL
	// XXXdbg - don't need to do this on a journaled volume
	if (hfsmp->jnl) {
		return 0;
	}
#endif /* DARWIN_JOURNAL */

	if (hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_EXCLUSIVE, p) != 0)
		return (0);

	/*
	 * Look for a matching node that has been delayed
	 * but is not part of a set (B_LOCKED).
	 */
	// s = splbio();
	VI_LOCK(vp);
	for (bp = TAILQ_FIRST(&vp->v_bufobj.bo_dirty.bv_hd); bp; bp = nbp) {
		nbp = TAILQ_NEXT(bp, b_bobufs);
		if (_BUF_LOCK(bp, LK_EXCLUSIVE | LK_NOWAIT))
			continue;
		VI_UNLOCK(vp);
		if (bp->b_lblkno == node) {

			bremfree(bp);
			// splx(s);
			(void) bwrite(bp);
			goto exit;
		}
		VI_LOCK(vp);
		BUF_UNLOCK(bp);
	}
	VI_UNLOCK(vp);
	// splx(s);
exit:
	(void) hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);

	return (0);
}


int
hfs_update(struct vnode *vp, struct timeval *access, struct timeval *modify, int waitfor)
{
	struct cnode *cp = VTOC(vp);
	proc_t *p;
	struct cat_fork *dataforkp = NULL;
	struct cat_fork *rsrcforkp = NULL;
	struct cat_fork datafork;
	int updateflag;
	struct hfsmount *hfsmp;
	int error;

	hfsmp = VTOHFS(vp);

	/* XXX do we really want to clear the sytem cnode flags here???? */
	if ((vp->v_vflag & VV_SYSTEM) || (VTOVFS(vp)->mnt_flag & MNT_RDONLY) || (cp->c_mode == 0)) {
		cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE);
		return (0);
	}

	updateflag = cp->c_flag & (C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE);

	/* Nothing to update. */
	if (updateflag == 0) {
		return (0);
	}
	/* HFS standard doesn't have access times. */
	if ((updateflag == C_ACCESS) && (VTOVCB(vp)->vcbSigWord == kHFSSigWord)) {
		return (0);
	}
	if (updateflag & C_ACCESS) {
		/*
		 * If only the access time is changing then defer
		 * updating it on-disk util later (in hfs_inactive).
		 * If it was recently updated then skip the update.
		 */
		if (updateflag == C_ACCESS) {
			cp->c_flag &= ~C_ACCESS;

			/* Its going to disk or its sufficiently newer... */
			if ((cp->c_flag & C_ATIMEMOD) || (access->tv_sec > (cp->c_atime + ATIME_ACCURACY))) {
				cp->c_atime = access->tv_sec;
				cp->c_flag |= C_ATIMEMOD;
			}
			return (0);
		} else {
			cp->c_atime = access->tv_sec;
		}
	}
	if (updateflag & C_UPDATE) {
		cp->c_mtime = modify->tv_sec;
		cp->c_mtime_nsec = modify->tv_usec * 1000;
	}
	if (updateflag & C_CHANGE) {
		cp->c_ctime = gettime();
		/*
		 * HFS dates that WE set must be adjusted for DST
		 */
		if ((VTOVCB(vp)->vcbSigWord == kHFSSigWord) && gTimeZone.tz_dsttime) {
			cp->c_ctime += 3600;
			cp->c_mtime = cp->c_ctime;
		}
	}

	if (cp->c_datafork)
		dataforkp = &cp->c_datafork->ff_data;
	if (cp->c_rsrcfork)
		rsrcforkp = &cp->c_rsrcfork->ff_data;

	p = curthread;

	/*
	 * For delayed allocations updates are
	 * postponed until an fsync or the file
	 * gets written to disk.
	 *
	 * Deleted files can defer meta data updates until inactive.
	 */
	if (ISSET(cp->c_flag, C_DELETED) || (dataforkp && cp->c_datafork->ff_unallocblocks) || (rsrcforkp && cp->c_rsrcfork->ff_unallocblocks)) {
		if (updateflag & (C_CHANGE | C_UPDATE))
			hfs_volupdate(hfsmp, VOL_UPDATE, 0);
		cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_UPDATE);
		cp->c_flag |= C_MODIFIED;

		return (0);
	}

#ifdef DARWIN_JOURNAL
	// XXXdbg
	hfs_global_shared_lock_acquire(hfsmp);
	if (hfsmp->jnl) {
		if ((error = journal_start_transaction(hfsmp->jnl)) != 0) {
			hfs_global_shared_lock_release(hfsmp);
			return error;
		}
	}
#endif /* DARWIN_JOURNAL */

	/*
	 * For files with invalid ranges (holes) the on-disk
	 * field representing the size of the file (cf_size)
	 * must be no larger than the start of the first hole.
	 */
	if (dataforkp && !TAILQ_EMPTY(&cp->c_datafork->ff_invalidranges)) {
		bcopy(dataforkp, &datafork, sizeof(datafork));
		datafork.cf_size = TAILQ_FIRST(&cp->c_datafork->ff_invalidranges)->rl_start;
		dataforkp = &datafork;
	}

	/*
	 * Lock the Catalog b-tree file.
	 * A shared lock is sufficient since an update doesn't change
	 * the tree and the lock on vp protects the cnode.
	 */
#if HFS_DIAGNOSTIC /* the b-tree layer expects it locked exclusively */
	error = hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_EXCLUSIVE, p);
#else
	error = hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_SHARED, p);
#endif
	if (error) {
#ifdef DARWIN_JOURNAL
		if (hfsmp->jnl) {
			journal_end_transaction(hfsmp->jnl);
		}
		hfs_global_shared_lock_release(hfsmp);
#endif /* DARWIN_JOURNAL */
		return (error);
	}

	/* XXX - waitfor is not enforced */
	error = cat_update(hfsmp, &cp->c_desc, &cp->c_attr, dataforkp, rsrcforkp);

	/* Unlock the Catalog b-tree file. */
	(void)hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);

	if (updateflag & (C_CHANGE | C_UPDATE))
		hfs_volupdate(hfsmp, VOL_UPDATE, 0);

#ifdef DARWIN_JOURNAL
	// XXXdbg
	if (hfsmp->jnl) {
		journal_end_transaction(hfsmp->jnl);
	}
	hfs_global_shared_lock_release(hfsmp);
#endif /* DARWIN_JOURNAL */

	/* After the updates are finished, clear the flags */
	cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE | C_ATIMEMOD);

	return (error);
}

int
hfs_btsync(struct vnode *vp, int sync_transaction)
{
	struct cnode *cp = VTOC(vp);
	register struct buf *bp;
	struct timeval tv;
	struct buf *nbp;
	struct hfsmount *hfsmp = VTOHFS(vp);

	/*
	 * Flush all dirty buffers associated with b-tree.
	 */
loop:
	VI_LOCK(vp);
	struct bufobj v_bufobj = vp->v_bufobj;

	for (bp = TAILQ_FIRST(&v_bufobj.bo_dirty.bv_hd); bp; bp = nbp) {
		nbp = TAILQ_NEXT(bp, b_bobufs);
		if (_BUF_LOCK(bp, LK_EXCLUSIVE | LK_NOWAIT))
			continue;
		VI_UNLOCK(vp);
		if ((bp->b_flags & B_DELWRI) == 0)
			panic("hfs_btsync: not dirty (bp 0x%p hfsmp 0x%p)", bp, hfsmp);

#ifdef DARWIN_JOURNAL
		// XXXdbg
		if (hfsmp->jnl && (bp->b_flags & B_LOCKED)) {
			if ((bp->b_flags & B_META) == 0) {
				panic("hfs: bp @ 0x%x is locked but not meta! jnl 0x%x\n", bp, hfsmp->jnl);
			}
			// if journal_active() returns >= 0 then the journal is ok and we
			// shouldn't do anything to this locked block (because it is part
			// of a transaction).  otherwise we'll just go through the normal
			// code path and flush the buffer.
			if (journal_active(hfsmp->jnl) >= 0) {
				continue;
			}
		}
#endif /* DARWIN_JOURNAL */

		if (sync_transaction) {
			VI_LOCK(vp);
			BUF_UNLOCK(bp);
			continue;
		}

		bremfree(bp);

		(void)bawrite(bp);

		goto loop;
	}
	VI_UNLOCK(vp);

	getmicrotime(&tv);
	if ((vp->v_vflag & VV_SYSTEM) && (VTOF(vp)->fcbBTCBPtr != NULL))
		(void)BTSetLastSync(VTOF(vp), tv.tv_sec);
	cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE);

	return 0;
}

static int
hfs_makenode(int mode, struct vnode *dvp, struct vnode **vpp, struct componentname *cnp)
{
	struct cnode *cp = NULL;
	struct cnode *dcp = VTOC(dvp);
	struct vnode *tvp = NULL;
	struct hfsmount *hfsmp = VTOHFS(dvp);
	struct timespec ts;
	struct timeval tv;
	proc_t *p = curthread; // cnp->cn_thread;
	struct cat_desc in_desc, out_desc;
	struct cat_attr attr;
	int error; // , started_tr = 0, grabbed_lock = 0;
	int vnodetype;

	tvp = NULL;
	bzero(&out_desc, sizeof(out_desc));

	if ((mode & S_IFMT) == 0)
		mode |= S_IFREG;
	vnodetype = IFTOVT(mode);

	/* Check if unmount in progress */
	if (VTOVFS(dvp)->mnt_kern_flag & MNTK_UNMOUNT) {
		error = EPERM;
		goto exit;
	}	
		
	/* Check if were out of usable disk space. */
	if ((priv_check_cred(cnp->cn_cred, PRIV_VFS_ADMIN) != 0) && (hfs_freeblks(hfsmp, 1) <= 0)) {
		error = ENOSPC;
		//goto exit;
		GOTO(exit);
	}
	

	// Set attrs
	bzero(&attr, sizeof(attr));
	attr.ca_mode = mode;
	attr.ca_nlink = vnodetype == VDIR ? 2 : 1;
	
	// Set time attrs
	getnanotime(&ts);
	attr.ca_mtime = ts.tv_sec;
	attr.ca_mtime_nsec = ts.tv_nsec;
	if ((VTOVCB(dvp)->vcbSigWord == kHFSSigWord) && gTimeZone.tz_dsttime) {
		attr.ca_mtime += 3600;	/* Same as what hfs_update does */
	}
	attr.ca_atime = attr.ca_ctime = attr.ca_itime = attr.ca_mtime;
	
	// Set perms
	if (VTOVFS(dvp)->mnt_flag & MNT_UNKNOWNPERMISSIONS) {
		attr.ca_uid = hfsmp->hfs_uid;
		attr.ca_gid = hfsmp->hfs_gid;
	} else {
		attr.ca_uid = vnodetype == VLNK ? dcp->c_uid : cnp->cn_cred->cr_uid;
		attr.ca_gid = dcp->c_gid;
	}

	// Set finder info?
	
	if (vnodetype == VLNK) {
		struct FndrFileInfo *fip;

		fip = (struct FndrFileInfo *)&attr.ca_finderinfo;
		fip->fdType    = SWAP_BE32(kSymLinkFileType);
		fip->fdCreator = SWAP_BE32(kSymLinkCreator);
	}

	
	if ((attr.ca_mode & S_ISGID) && !groupmember(dcp->c_gid, cnp->cn_cred) /* &&
	    priv_check_cred(cnp->cn_cred, SUSER_ALLOWJAIL) */) {
		attr.ca_mode &= ~S_ISGID;
	}

	if (cnp->cn_flags & ISWHITEOUT) {
		attr.ca_flags |= UF_OPAQUE;
	}

	
	/* Setup the descriptor */
	bzero(&in_desc, sizeof(in_desc));
	in_desc.cd_nameptr = cnp->cn_nameptr;
	in_desc.cd_namelen = cnp->cn_namelen;
	in_desc.cd_parentcnid = dcp->c_cnid;
	in_desc.cd_flags = S_ISDIR(mode) ? CD_ISDIR : 0;

	/* Lock catalog b-tree */
	error = hfs_metafilelocking(VTOHFS(dvp), kHFSCatalogFileID, LK_EXCLUSIVE, p);
	if (error) {
		//goto exit;
		GOTO(exit);
	}

	error = cat_create(hfsmp, &in_desc, &attr, &out_desc);

	/* Unlock catalog b-tree */
	(void) hfs_metafilelocking(VTOHFS(dvp), kHFSCatalogFileID, LK_RELEASE, p);		
	if (error) {
		// goto exit;
		GOTO(exit);
	}

	/* Update the parent directory */
	dcp->c_childhint = out_desc.cd_hint;	/* Cache directory's location */
	dcp->c_nlink++;
	dcp->c_entries++;
	dcp->c_flag |= C_CHANGE | C_UPDATE;
	getmicrotime(&tv);
	(void)hfs_update(dvp, &tv, &tv, 0);

	hfs_volupdate(hfsmp, vnodetype == VDIR ? VOL_MKDIR : VOL_MKFILE,
		(dcp->c_cnid == kHFSRootFolderID));

			
	error = hfs_getnewvnode(hfsmp, NULL, &out_desc, 0, &attr, NULL, &tvp);
	if (error) {
	//	goto exit;
		GOTO(exit);
	}
	/*
	 * restore vtype and mode for VBLK and VCHR
	 */
	if (vnodetype == VBLK || vnodetype == VCHR) {
		struct cnode *cp;

		cp = VTOC(tvp);
		cp->c_mode = mode;
		tvp->v_type = IFTOVT(mode);
		cp->c_flag |= C_CHANGE;
		getmicrotime(&tv);
		if ((error = hfs_update(tvp, &tv, &tv, 1))) {
			vput(tvp);
			//goto exit;
			GOTO(exit);
		}
	}

	*vpp = tvp;
exit:
	cat_releasedesc(&out_desc);

	/*
	 * Check if a file is located in the "Cleanup At Startup"
	 * directory.  If it is then tag it as NODUMP so that we
	 * can be lazy about zero filling data holes.
	 */
	if ((error == 0) && (vnodetype == VREG) &&
	    (dcp->c_desc.cd_nameptr != NULL) &&
	    (strcmp(dcp->c_desc.cd_nameptr, "Cleanup At Startup") == 0)) {
	   	struct vnode *ddvp;
		cnid_t parid;

		parid = dcp->c_parentcnid;
		dvp = NULL;

		/*
		 * The parent of "Cleanup At Startup" should
		 * have the ASCII name of the userid.
		 */
		if (VFS_VGET(HFSTOVFS(hfsmp), parid, LK_EXCLUSIVE, &ddvp) == 0) {
			if (VTOC(ddvp)->c_desc.cd_nameptr &&
			    (cp->c_uid == strtoul(VTOC(ddvp)->c_desc.cd_nameptr, 0, 0))) {
				cp->c_xflags |= UF_NODUMP;
				cp->c_flag |= C_CHANGE;
			}
			vput(ddvp);
		}
	}

	return (error);
}

static int
hfs_mkdir(struct vop_mkdir_args *ap)
{
	/* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vattr *a_vap;
	} */ 
	struct vattr *vap = ap->a_vap;

	return (hfs_makenode(MAKEIMODE(vap->va_type, vap->va_mode),
				ap->a_dvp, ap->a_vpp, ap->a_cnp));
}

static int
hfs_create(struct vop_create_args *ap)
{
	/* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vattr *a_vap;
	} */ 
	struct vattr *vap = ap->a_vap;

	return (hfs_makenode(MAKEIMODE(vap->va_type, vap->va_mode),
				ap->a_dvp, ap->a_vpp, ap->a_cnp));
}

static int
hfs_lock1(struct vop_lock1_args *ap)
{
	/* {
		struct vnode *a_vp;
		int a_flags;
		char *file;
		int line;
	} */
	struct mtx *ilk;

	ilk = VI_MTX(ap->a_vp);
	int retval = (lockmgr_lock_flags(ap->a_vp->v_vnlock, ap->a_flags, &ilk->lock_object, ap->a_file, ap->a_line));

	return (retval);
}

static int
hfs_unlock(struct vop_unlock_args *ap)
{
	/* {
	       struct vnode *a_vp;
	       int a_flags;
	       proc_t *a_td;
       } */
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);

	if (cp == NULL)
		panic("hfs_unlock: cnode in vnode is null\n");

	return (lockmgr(&cp->c_lock, LK_RELEASE, &vp->v_interlock));
}

void
replace_desc(struct cnode *cp, struct cat_desc *cdp)
{
	/* First release allocated name buffer */
	if (cp->c_desc.cd_flags & CD_HASBUF && cp->c_desc.cd_nameptr != 0) {
		char *name = cp->c_desc.cd_nameptr;

		cp->c_desc.cd_nameptr = 0;
		cp->c_desc.cd_namelen = 0;
		cp->c_desc.cd_flags &= ~CD_HASBUF;
		free(name, M_TEMP);
	}
	bcopy(cdp, &cp->c_desc, sizeof(cp->c_desc));

	/* Cnode now owns the name buffer */
	cdp->cd_nameptr = 0;
	cdp->cd_namelen = 0;
	cdp->cd_flags &= ~CD_HASBUF;
}

static int
log_notsupp(struct vop_generic_args *ap)
{
	if (ap->a_desc && ap->a_desc->vdesc_name) {
		printf("Unimplemented vop: %s\n", ap->a_desc->vdesc_name);
	} else {
		printf("Huh?\n");
	}

	return (EOPNOTSUPP);
}

struct vop_vector hfs_vnodeops = {
	.vop_default = &default_vnodeops,

	.vop_getpages =  vnode_pager_local_getpages,
	.vop_getpages_async = vnode_pager_local_getpages_async,

	.vop_access = hfs_access,
	.vop_aclcheck = ((void *)(uintptr_t)log_notsupp),
	.vop_advlock = ((void *)(uintptr_t)log_notsupp),
	.vop_bmap = hfs_bmap,
	.vop_cachedlookup = hfs_cachedlookup,
	.vop_close = hfs_close,
	.vop_closeextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_create = hfs_create,
	.vop_deleteextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_fsync = hfs_fsync,
	.vop_getacl = ((void *)(uintptr_t)log_notsupp),
	.vop_getattr = hfs_getattr,
	.vop_getextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_inactive = hfs_inactive,
	.vop_islocked = hfs_islocked,
	.vop_lock1 = hfs_lock1,
	.vop_ioctl = hfs_ioctl,
	.vop_link = ((void *)(uintptr_t)log_notsupp),
	.vop_listextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_lookup = hfs_lookup,
	.vop_mkdir = hfs_mkdir,
	.vop_mknod = ((void *)(uintptr_t)log_notsupp),
	.vop_open = hfs_open,
	.vop_openextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_pathconf = hfs_pathconf,
	.vop_poll = ((void *)(uintptr_t)log_notsupp),
	.vop_print = ((void *)(uintptr_t)log_notsupp),
	.vop_read = hfs_read,
	.vop_readdir = hfs_readdir,
	.vop_readlink = hfs_readlink,
	.vop_reclaim = hfs_reclaim,
	.vop_remove = ((void *)(uintptr_t)log_notsupp),
	.vop_rename = ((void *)(uintptr_t)log_notsupp),
	.vop_rmdir = ((void *)(uintptr_t)log_notsupp),
	.vop_setacl = ((void *)(uintptr_t)log_notsupp),
	.vop_setattr = hfs_setattr,
	.vop_setextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_setlabel = ((void *)(uintptr_t)log_notsupp),
	.vop_strategy = hfs_strategy,
	.vop_symlink = ((void *)(uintptr_t)log_notsupp),
	.vop_unlock = hfs_unlock,
	.vop_whiteout = ((void *)(uintptr_t)log_notsupp),
	.vop_write = ((void *)(uintptr_t)log_notsupp),
	.vop_vptofh = ((void *)(uintptr_t)log_notsupp),
	// .vop_add_writecount = ((void *)(uintptr_t)log_notsupp),
	// .vop_vput_pair = ((void *)(uintptr_t)log_notsupp),
	.vop_set_text = ((void *)(uintptr_t)log_notsupp),
	.vop_unset_text = ((void *)(uintptr_t)log_notsupp),
	.vop_unp_bind = ((void *)(uintptr_t)log_notsupp),
	.vop_unp_connect = ((void *)(uintptr_t)log_notsupp),
	.vop_unp_detach = ((void *)(uintptr_t)log_notsupp),
};

VFS_VOP_VECTOR_REGISTER(hfs_vnodeops);
