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

int hfs_bmap(struct vop_bmap_args *);
int hfs_strategy(struct vop_strategy_args *);
int hfs_reclaim(struct vop_reclaim_args *);
int hfs_inactive(struct vop_inactive_args *);
int hfs_read(struct vop_read_args *);
int hfs_ioctl(struct vop_ioctl_args *);

static struct dirent dot = {
	.d_fileno = 1,
	.d_off = 0,
	.d_reclen = _GENERIC_DIRLEN(1),
	.d_namlen = 1,
	.d_name = ".",
};

static struct dirent dotdot = { .d_fileno = 1, .d_off = 0, .d_reclen = _GENERIC_DIRLEN(2), .d_namlen = 2, .d_name = ".." };

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

static int
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
hfs_getattr(struct vop_getattr_args *ap)
{
	/* {
		struct vnode *a_vp;
		struct vattr *a_vap;
		struct ucred *a_cred;
		proc_t *a_td;
	} */
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct vattr *vap = ap->a_vap;
	struct timeval tv;

	getmicrotime(&tv);
	CTIMES(cp, &tv, &tv);

	vap->va_type = vp->v_type;
	/*
	 * [2856576]  Since we are dynamically changing the owner, also
	 * effectively turn off the set-user-id and set-group-id bits,
	 * just like chmod(2) would when changing ownership.  This prevents
	 * a security hole where set-user-id programs run as whoever is
	 * logged on (or root if nobody is logged in yet!)
	 */
	vap->va_mode = (cp->c_uid == UNKNOWNUID) ? cp->c_mode & ~(S_ISUID | S_ISGID) : cp->c_mode;
	vap->va_nlink = cp->c_nlink;
#ifdef DARWIN
	vap->va_uid = (cp->c_uid == UNKNOWNUID) ? console_user : cp->c_uid;
#else
	vap->va_uid = (cp->c_uid == UNKNOWNUID) ? 0 : cp->c_uid;
#endif
	vap->va_gid = cp->c_gid;
	vap->va_fsid = dev2udev(cp->c_dev);
	/*
	 * Exporting file IDs from HFS Plus:
	 *
	 * For "normal" files the c_fileid is the same value as the
	 * c_cnid.  But for hard link files, they are different - the
	 * c_cnid belongs to the active directory entry (ie the link)
	 * and the c_fileid is for the actual inode (ie the data file).
	 *
	 * The stat call (getattr) will always return the c_fileid
	 * and Carbon APIs, which are hardlink-ignorant, will always
	 * receive the c_cnid (from getattrlist).
	 */
	vap->va_fileid = cp->c_fileid;
	vap->va_atime.tv_sec = cp->c_atime;
	vap->va_atime.tv_nsec = 0;
	vap->va_mtime.tv_sec = cp->c_mtime;
	vap->va_mtime.tv_nsec = cp->c_mtime_nsec;
	vap->va_ctime.tv_sec = cp->c_ctime;
	vap->va_ctime.tv_nsec = 0;
	vap->va_gen = 0;
	vap->va_flags = cp->c_xflags;
	vap->va_rdev = 0;
	vap->va_blocksize = VTOVFS(vp)->mnt_stat.f_iosize;
	vap->va_filerev = 0;
	vap->va_spare = 0;
	if (vp->v_type == VDIR) {
		vap->va_size = cp->c_nlink * AVERAGE_HFSDIRENTRY_SIZE;
		vap->va_bytes = 0;
	} else {
		vap->va_size = VTOF(vp)->ff_size;
		vap->va_bytes = (u_quad_t)cp->c_blocks * (u_quad_t)VTOVCB(vp)->blockSize;
		if (vp->v_type == VBLK || vp->v_type == VCHR)
			vap->va_rdev = cp->c_rdev;
	}
	return (0);
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
	.vop_create = ((void *)(uintptr_t)log_notsupp),
	.vop_deleteextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_fsync = ((void *)(uintptr_t)log_notsupp),
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
	.vop_mkdir = ((void *)(uintptr_t)log_notsupp),
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
	.vop_setattr = ((void *)(uintptr_t)log_notsupp),
	.vop_setextattr = ((void *)(uintptr_t)log_notsupp),
	.vop_setlabel = ((void *)(uintptr_t)log_notsupp),
	.vop_strategy = hfs_strategy,
	.vop_symlink = ((void *)(uintptr_t)log_notsupp),
	.vop_unlock = hfs_unlock,
	.vop_whiteout = ((void *)(uintptr_t)log_notsupp),
	.vop_write = ((void *)(uintptr_t)log_notsupp),
	.vop_vptofh = ((void *)(uintptr_t)log_notsupp),
	.vop_add_writecount = ((void *)(uintptr_t)log_notsupp),
	.vop_vput_pair = ((void *)(uintptr_t)log_notsupp),
	.vop_set_text = ((void *)(uintptr_t)log_notsupp),
	.vop_unset_text = ((void *)(uintptr_t)log_notsupp),
	.vop_unp_bind = ((void *)(uintptr_t)log_notsupp),
	.vop_unp_connect = ((void *)(uintptr_t)log_notsupp),
	.vop_unp_detach = ((void *)(uintptr_t)log_notsupp),
};

VFS_VOP_VECTOR_REGISTER(hfs_vnodeops);
