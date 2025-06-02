#include <sys/types.h>

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/lockf.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/unistd.h>

// #include <sys/kdebug.h>

#include <hfsplus/hfs.h>
#include <hfsplus/hfs_catalog.h>
#include <hfsplus/hfs_cnode.h>
#include <hfsplus/hfs_dbg.h>
#include <hfsplus/hfs_mount.h>
#include <hfsplus/hfs_quota.h>
#include <hfsplus/hfs_endian.h>

#include "hfscommon/headers/BTreesInternal.h"
#include "hfscommon/headers/FileMgrInternal.h"

int hfs_bmap(struct vop_bmap_args*);
int hfs_strategy(struct vop_strategy_args*);

int hfs_update(struct vnode *vp, struct timeval *access, struct timeval *modify, int waitfor)
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
	if ((vp->v_vflag & VV_SYSTEM) ||
	    (VTOVFS(vp)->mnt_flag & MNT_RDONLY) ||
	    (cp->c_mode == 0)) {
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
			if ((cp->c_flag & C_ATIMEMOD) ||
			    (access->tv_sec > (cp->c_atime + ATIME_ACCURACY))) {
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

	p = current_proc();

	/*
	 * For delayed allocations updates are
	 * postponed until an fsync or the file
	 * gets written to disk.
	 *
	 * Deleted files can defer meta data updates until inactive.
	 */
	if (ISSET(cp->c_flag, C_DELETED) ||
	    (dataforkp && cp->c_datafork->ff_unallocblocks) ||
	    (rsrcforkp && cp->c_rsrcfork->ff_unallocblocks)) {
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
	(void) hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);

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

int hfs_btsync(struct vnode *vp, int sync_transaction)
{
	struct cnode *cp = VTOC(vp);
	register struct buf *bp;
	struct timeval tv;
	struct buf *nbp;
	struct hfsmount *hfsmp = VTOHFS(vp);
	// int s;

	/*
	 * Flush all dirty buffers associated with b-tree.
	 */
loop:
	// s = splbio();
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

#ifdef DARWIN
		if (sync_transaction && !(bp->b_flags & B_LOCKED)) {
#else
		if (sync_transaction) {
#endif
			VI_LOCK(vp);
			BUF_UNLOCK(bp);
			continue;
		}

		bremfree(bp);
#ifdef DARWIN
		bp->b_flags |= B_BUSY;
		bp->b_flags &= ~B_LOCKED;
#endif

	//	splx(s);

		(void) bawrite(bp);

		goto loop;
	}
	VI_UNLOCK(vp);
	// splx(s);

	getmicrotime(&tv);
	if ((vp->v_vflag & VV_SYSTEM) && (VTOF(vp)->fcbBTCBPtr != NULL))
		(void) BTSetLastSync(VTOF(vp), tv.tv_sec);
	cp->c_flag &= ~(C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE);

	return 0;
}

struct vop_vector hfs_vnodeops = {
	.vop_default =		&default_vnodeops,

	.vop_access =		VOP_EOPNOTSUPP,
	.vop_aclcheck =		VOP_EOPNOTSUPP,
	.vop_advlock =		VOP_EOPNOTSUPP,
	.vop_bmap =		hfs_bmap,
	.vop_cachedlookup =	VOP_EOPNOTSUPP,
	.vop_close =		VOP_EOPNOTSUPP,
	.vop_closeextattr =	VOP_EOPNOTSUPP,
	.vop_create =		VOP_EOPNOTSUPP,
	.vop_deleteextattr =	VOP_EOPNOTSUPP,
	.vop_fsync =		VOP_EOPNOTSUPP,
	.vop_getacl =		VOP_EOPNOTSUPP,
	.vop_getattr =		VOP_EOPNOTSUPP,
	.vop_getextattr =	VOP_EOPNOTSUPP,
	.vop_getwritemount =	VOP_EOPNOTSUPP,
	.vop_inactive =		VOP_EOPNOTSUPP,
	.vop_need_inactive =	VOP_EOPNOTSUPP,
	.vop_islocked =		VOP_EOPNOTSUPP,
	.vop_ioctl =		VOP_EOPNOTSUPP,
	.vop_link =		VOP_EOPNOTSUPP,
	.vop_listextattr =	VOP_EOPNOTSUPP,
	.vop_lock1 =		VOP_EOPNOTSUPP,
	.vop_lookup =		VOP_EOPNOTSUPP,
	.vop_mkdir =		VOP_EOPNOTSUPP,
	.vop_mknod =		VOP_EOPNOTSUPP,
	.vop_open =		VOP_EOPNOTSUPP,
	.vop_openextattr =	VOP_EOPNOTSUPP,
	.vop_pathconf =		VOP_EOPNOTSUPP,
	.vop_poll =		VOP_EOPNOTSUPP,
	.vop_print =		VOP_EOPNOTSUPP,
	.vop_read =		VOP_EOPNOTSUPP,
	.vop_readdir =		VOP_EOPNOTSUPP,
	.vop_readlink =		VOP_EOPNOTSUPP,
	.vop_reclaim =		VOP_EOPNOTSUPP,
	.vop_remove =		VOP_EOPNOTSUPP,
	.vop_rename =		VOP_EOPNOTSUPP,
	.vop_rmdir =		VOP_EOPNOTSUPP,
	.vop_setacl =		VOP_EOPNOTSUPP,
	.vop_setattr =		VOP_EOPNOTSUPP,
	.vop_setextattr =	VOP_EOPNOTSUPP,
	.vop_setlabel =		VOP_EOPNOTSUPP,
	.vop_strategy =		hfs_strategy,
	.vop_symlink =		VOP_EOPNOTSUPP,
	.vop_unlock =		VOP_EOPNOTSUPP,
	.vop_whiteout =		VOP_EOPNOTSUPP,
	.vop_write =		VOP_EOPNOTSUPP,
	.vop_vptofh =		VOP_EOPNOTSUPP,
	.vop_add_writecount =	VOP_EOPNOTSUPP,
	.vop_vput_pair =	VOP_EOPNOTSUPP,
	.vop_set_text =		VOP_EOPNOTSUPP,
	.vop_unset_text = 	VOP_EOPNOTSUPP,
	.vop_unp_bind =		VOP_EOPNOTSUPP,
	.vop_unp_connect =	VOP_EOPNOTSUPP,
	.vop_unp_detach =	VOP_EOPNOTSUPP,
};

VFS_VOP_VECTOR_REGISTER(hfs_vnodeops);
