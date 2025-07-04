/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
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

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#ifdef DARWIN_UBC
#include <sys/ubc.h>
#endif
#ifdef DARWIN_QUOTA
#include <sys/quota.h>
#endif

#ifdef DARWIN
#include <miscfs/specfs/specdev.h>
#endif

#include <hfsplus/hfs.h>
#include <hfsplus/hfs_catalog.h>
#include <hfsplus/hfs_cnode.h>
#include <hfsplus/hfs_quota.h>

// #ifdef DARWIN
// extern int prtactive;
// #endif

extern void hfs_relnamehints(struct cnode *dcp);

static MALLOC_DEFINE(M_HFSNODE, "HFS cnode", "HFS cnode data");
static MALLOC_DEFINE(M_HFSFORK, "HFS fork", "HFS fork data");

/*
 * Last reference to an cnode.  If necessary, write or delete it.
 */
int
hfs_inactive(struct vop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct hfsmount *hfsmp = VTOHFS(vp);
	proc_t *p = curthread;
	struct timeval tv;
	int error = 0;
	int recycle = 0;
	int forkcount = 0;
	int truncated = 0;
#ifdef DARWIN_JOURNAL
	int started_tr = 0, grabbed_lock = 0;
#endif

	/*
	 * Ignore nodes related to stale file handles.
	 */
	if (cp->c_mode == 0)
		goto out;

	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		goto out;

	if (cp->c_datafork)
		++forkcount;
	if (cp->c_rsrcfork)
		++forkcount;

	/* If needed, get rid of any fork's data for a deleted file */
	if ((cp->c_flag & C_DELETED) && vp->v_type == VREG && (VTOF(vp)->ff_blocks != 0)) {
		error = hfs_truncate(vp, (off_t)0, IO_NDELAY, NOCRED, p);
		truncated = 1;
		// have to do this to prevent the lost ubc_info panic
		SET(cp->c_flag, C_TRANSIT);
		recycle = 1;
		if (error)
			goto out;
	}

	/*
	 * Check for a postponed deletion.
	 * (only delete cnode when the last fork goes inactive)
	 */
	if ((cp->c_flag & C_DELETED) && (forkcount <= 1)) {
		/*
		 * Mark cnode in transit so that one can get this
		 * cnode from cnode hash.
		 */
		SET(cp->c_flag, C_TRANSIT);
		cp->c_flag &= ~C_DELETED;
		cp->c_rdev = 0;

#ifdef DARWIN_JOURNAL
		// XXXdbg
		hfs_global_shared_lock_acquire(hfsmp);
		grabbed_lock = 1;
		if (hfsmp->jnl) {
			if (journal_start_transaction(hfsmp->jnl) != 0) {
				error = EINVAL;
				goto out;
			}
			started_tr = 1;
		}
#endif

		/* Lock catalog b-tree */
		error = hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_EXCLUSIVE, p);
		if (error)
			goto out;

		if (cp->c_blocks > 0)
			printf("hfs_inactive: attempting to delete a non-empty "
			       "file!");

		/*
		 * The descriptor name may be zero,
		 * in which case the fileid is used.
		 */
		error = cat_delete(hfsmp, &cp->c_desc, &cp->c_attr);

		if (error && truncated && (error != ENXIO))
			printf("hfs_inactive: couldn't delete a truncated file!");

		/* Update HFS Private Data dir */
		if (error == 0) {
			hfsmp->hfs_privdir_attr.ca_entries--;
			(void)cat_update(hfsmp, &hfsmp->hfs_privdir_desc, &hfsmp->hfs_privdir_attr, NULL, NULL);
		}

		/* Unlock catalog b-tree */
		(void)hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);

		if (error)
			goto out;

#define QUOTA 0
#if QUOTA
		(void)hfs_chkiq(cp, -1, NOCRED, 0);
#endif /* QUOTA */

		cp->c_mode = 0;
		cp->c_flag |= C_NOEXISTS | C_CHANGE | C_UPDATE;

		if (error == 0)
			hfs_volupdate(hfsmp, VOL_RMFILE, 0);
	}

	/* Push any defered access times to disk */
	if (cp->c_flag & C_ATIMEMOD) {
		cp->c_flag &= ~C_ATIMEMOD;
		if (HFSTOVCB(hfsmp)->vcbSigWord == kHFSPlusSigWord)
			cp->c_flag |= C_MODIFIED;
	}

	if (cp->c_flag & (C_ACCESS | C_CHANGE | C_MODIFIED | C_UPDATE)) {
		getmicrotime(&tv);
		hfs_update(vp, &tv, &tv, 0);
	}
out:
#ifdef DARWIN_JOURNAL
	// XXXdbg - have to do this because a goto could have come here
	if (started_tr) {
		journal_end_transaction(hfsmp->jnl);
		started_tr = 0;
	}
	if (grabbed_lock) {
		hfs_global_shared_lock_release(hfsmp);
	}
#endif
	/*
	 * If we are done with the vnode, reclaim it
	 * so that it can be reused immediately.
	 */
	if (cp->c_mode == 0 || recycle)
		vrecycle(vp);

	return (error);
}

/*
 * Reclaim a cnode so that it can be used for other purposes.
 */
int
hfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct vnode *devvp = NULL;
	struct filefork *fp = NULL;
	struct filefork *altfp = NULL;

	devvp = cp->c_devvp; /* For later releasing */

	/*
	 * Find file fork for this vnode (if any)
	 * Also check if another fork is active
	 */
	if ((fp = cp->c_datafork) && (cp->c_vp == vp)) {
		cp->c_datafork = NULL;
		cp->c_vp = NULL;
		altfp = cp->c_rsrcfork;
	} else if ((fp = cp->c_rsrcfork) && (cp->c_rsrc_vp == vp)) {
		cp->c_rsrcfork = NULL;
		cp->c_rsrc_vp = NULL;
		altfp = cp->c_datafork;
	} else {
		cp->c_vp = NULL;
		fp = NULL;
		altfp = NULL;
	}

	/*
	 * On the last fork, remove the cnode from its hash chain.
	 */
	if (altfp == NULL)
		hfs_chashremove(cp);

	/* Release the file fork and related data (can block) */
	if (fp) {
		fp->ff_cp = NULL;
		/* Dump cached symlink data */
		if ((vp->v_type == VLNK) && (fp->ff_symlinkptr != NULL)) {
			free(fp->ff_symlinkptr, M_TEMP);
			fp->ff_symlinkptr = NULL;
		}
		free(fp, M_HFSFORK);
		fp = NULL;
	}

	/*
	 * Purge old data structures associated with the cnode.
	 */
	cache_purge(vp);
	if (devvp && altfp == NULL) {
		cp->c_devvp = NULL;
		vrele(devvp);
	}

	vp->v_lock = *vp->v_vnlock;
	vp->v_vnlock = &vp->v_lock; /* so that lock assertions won't panic */
	/*
	 * If there was only one active fork then we can release the cnode.
	 */
	if (altfp == NULL) {
#if QUOTA
		for (i = 0; i < MAXQUOTAS; i++) {
			if (cp->c_dquot[i] != NODQUOT) {
				dqreclaim(vp, cp->c_dquot[i]);
				cp->c_dquot[i] = NODQUOT;
			}
		}
#endif /* QUOTA */
		/*
		 * Free any left over directory indices
		 */
		if (vp->v_type == VDIR)
			hfs_relnamehints(cp);

		/*
		 * If the descriptor has a name then release it
		 */
		if (cp->c_desc.cd_flags & CD_HASBUF) {
			char *nameptr;

			nameptr = cp->c_desc.cd_nameptr;
			cp->c_desc.cd_nameptr = 0;
			cp->c_desc.cd_flags &= ~CD_HASBUF;
			cp->c_desc.cd_namelen = 0;
			free(nameptr, M_TEMP);
		}
		CLR(cp->c_flag, (C_ALLOC | C_TRANSIT));
		if (ISSET(cp->c_flag, C_WALLOC) || ISSET(cp->c_flag, C_WTRANSIT))
			wakeup(cp);
		free(cp, M_HFSNODE);
	}

	vp->v_data = 0;

	return (0);
}

/*
 * get a cnode
 *
 * called by hfs_lookup and hfs_vget (descp == NULL)
 *
 * returns a locked vnode for cnode for given cnid/fileid
 */
int
hfs_getcnode(struct hfsmount *hfsmp, cnid_t cnid, struct cat_desc *descp, int wantrsrc, struct cat_attr *attrp, struct cat_fork *forkp, struct vnode **vpp)
{
	struct cdev *dev = hfsmp->hfs_raw_dev;
	struct vnode *vp = NULL;
	struct vnode *rvp = NULL;
	struct vnode *new_vp = NULL;
	struct cnode *cp = NULL;
	proc_t *p = curthread;
	int retval = E_NONE;

	/* Check if unmount in progress */
	if (HFSTOVFS(hfsmp)->mnt_kern_flag & MNTK_UNMOUNT) {
		*vpp = NULL;
		return (EPERM);
	}

	/*
	 * Check the hash for an active cnode
	 */
	cp = hfs_chashget(dev, cnid, wantrsrc, &vp, &rvp);
	if (cp != NULL) {
		/* hide open files that have been deleted */
		if ((hfsmp->hfs_private_metadata_dir != 0) && (cp->c_parentcnid == hfsmp->hfs_private_metadata_dir) && (cp->c_nlink == 0)) {
			retval = ENOENT;
			goto exit;
		}

		if (wantrsrc && rvp != NULL) {
			vp = rvp;
			rvp = NULL;
			goto done;
		}
		if (!wantrsrc && vp != NULL) {
			/* Hardlinks need an updated catalog descriptor */
			if (descp && cp->c_flag & C_HARDLINK) {
				replace_desc(cp, descp);
			}
			/* We have a vnode so we're done. */
			goto done;
		}
	}

	/*
	 * There was no active vnode so get a new one.
	 * Use the existing cnode (if any).
	 */
	if (descp != NULL) {
		/*
		 * hfs_lookup case, use descp, attrp and forkp
		 */
		retval = hfs_getnewvnode(hfsmp, cp, descp, wantrsrc, attrp, forkp, &new_vp);
	} else {
		struct cat_desc cndesc = { 0 };
		struct cat_attr cnattr = { 0 };
		struct cat_fork cnfork = { 0 };

		/*
		 * hfs_vget case, need to lookup entry (by file id)
		 */
		if (cnid == kRootParID) {
			static char hfs_rootname[] = "/";

			cndesc.cd_nameptr = &hfs_rootname[0];
			cndesc.cd_namelen = 1;
			cndesc.cd_parentcnid = kRootParID;
			cndesc.cd_cnid = kRootParID;
			cndesc.cd_flags = CD_ISDIR;

			cnattr.ca_fileid = kRootParID;
			cnattr.ca_nlink = 2;
			cnattr.ca_mode = (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO);
		} else {
			/* Lock catalog b-tree */
			retval = hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_SHARED, p);
			if (retval)
				goto exit;

			retval = cat_idlookup(hfsmp, cnid, &cndesc, &cnattr, &cnfork);

			/* Unlock catalog b-tree */
			(void)hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);
			if (retval)
				goto exit;

			/* Hide open files that have been deleted */
			if ((hfsmp->hfs_private_metadata_dir != 0) && (cndesc.cd_parentcnid == hfsmp->hfs_private_metadata_dir)) {
				cat_releasedesc(&cndesc);
				retval = ENOENT;
				goto exit;
			}
		}

		retval = hfs_getnewvnode(hfsmp, cp, &cndesc, 0, &cnattr, &cnfork, &new_vp);

		/* Hardlinks may need an updated catalog descriptor */
		if (retval == 0 && new_vp && (VTOC(new_vp)->c_flag & C_HARDLINK) && cndesc.cd_nameptr && cndesc.cd_namelen > 0) {
			replace_desc(VTOC(new_vp), &cndesc);
		}
		cat_releasedesc(&cndesc);
	}
exit:
	/* Release reference taken on opposite vnode (if any). */
	if (vp)
		vput(vp);
	else if (rvp)
		vput(rvp);

	if (retval) {
		*vpp = NULL;
		return (retval);
	}
	vp = new_vp;
done:
	/* The cnode's vnode should be in vp. */
	if (vp == NULL)
		panic("hfs_getcnode: missing vp!");

#ifdef DARWIN_UBC
	UBCINFOCHECK("hfs_getcnode", vp);
#endif
	*vpp = vp;
	return (0);
}

/*
 * hfs_getnewvnode - get new default vnode
 *
 * the vnode is returned locked
 */
// extern vnodeopv_desc hfs_vnodeop;
// extern vnodeopv_desc hfs_specop_p;
// extern vnodeopv_desc hfs_fifoop_p;

int
hfs_getnewvnode(struct hfsmount *hfsmp, struct cnode *cp, struct cat_desc *descp, int wantrsrc, struct cat_attr *attrp, struct cat_fork *forkp,
    struct vnode **vpp)
{
	struct mount *mp = HFSTOVFS(hfsmp);
	struct vnode *vp = NULL;
	struct vnode *rvp = NULL;
	struct vnode *new_vp = NULL;
	struct cnode *cp2 = NULL;
	struct filefork *fp = NULL;
	int allocated = 0;
	int retval;
	struct cdev *dev;

	/* Bail when unmount is in progress */
	if (mp->mnt_kern_flag & MNTK_UNMOUNT) {
		*vpp = NULL;
		return (EPERM);
	}

#define FIFO 0
#if !FIFO
	if (IFTOVT(attrp->ca_mode) == VFIFO) {
		*vpp = NULL;
		return (EOPNOTSUPP);
	}
#endif

	dev = hfsmp->hfs_raw_dev;

	/* If no cnode was passed in then create one */
	if (cp == NULL) {
		cp2 = (struct cnode *)malloc(sizeof(struct cnode), M_HFSNODE, M_WAITOK);
		bzero(cp2, sizeof(struct cnode));
		allocated = 1;
		SET(cp2->c_flag, C_ALLOC);
		cp2->c_cnid = descp->cd_cnid;
		cp2->c_fileid = attrp->ca_fileid;
		cp2->c_dev = dev;

		lockinit(&cp2->c_lock, PVFS, "cnode", VLKTIMEOUT, LK_NOSHARE | LK_NOWITNESS);
		if (lockmgr(&cp2->c_lock, LK_EXCLUSIVE | LK_NOWITNESS, NULL)) {
			panic("hfs_getnewvnode: failed to lock brand new cnode");
		}
		/*
		 * There were several blocking points since we first
		 * checked the hash. Now that we're through blocking,
		 * check the hash again in case we're racing for the
		 * same cnode.
		 */
		cp = hfs_chashget(dev, attrp->ca_fileid, wantrsrc, &vp, &rvp);
		if (cp != NULL) {
			/* We lost the race - use the winner's cnode */
			lockdestroy(&cp2->c_lock);
			free(cp2, M_HFSNODE);
			allocated = 0;
			if (wantrsrc && rvp != NULL) {
				*vpp = rvp;
				return (0);
			}
			if (!wantrsrc && vp != NULL) {
				*vpp = vp;
				return (0);
			}
		} else /* allocated */ {
			cp = cp2;
			hfs_chashinsert(cp);
		}
	}

	/* Allocate a new vnode. If unsuccesful, leave after freeing memory */

	retval = getnewvnode("hfs", mp, &hfs_vnodeops, &new_vp);

	if (retval) {
		if (allocated) {
			hfs_chashremove(cp);
			if (ISSET(cp->c_flag, C_WALLOC)) {
				CLR(cp->c_flag, C_WALLOC);
				wakeup(cp);
			}
			lockdestroy(&cp2->c_lock);
			free(cp2, M_HFSNODE);
			allocated = 0;
		} else if (rvp) {
			vput(rvp);
		} else if (vp) {
			vput(vp);
		}
		*vpp = NULL;
		return (retval);
	}

	if (allocated) {
		bcopy(attrp, &cp->c_attr, sizeof(struct cat_attr));
		bcopy(descp, &cp->c_desc, sizeof(struct cat_desc));
	}

	new_vp->v_data = cp;
	new_vp->v_vnlock = &cp->c_lock;
	new_vp->v_lock = *new_vp->v_vnlock;

	insmntque(new_vp, mp);

	if (wantrsrc && S_ISREG(cp->c_mode)) {
		cp->c_rsrc_vp = new_vp;
	} else {
		cp->c_vp = new_vp;
	}

	/* Release reference taken on opposite vnode (if any). */
	if (rvp) {
		vput(rvp);
	}

	if (vp) {
		vput(vp);
	}

	vp = new_vp;

	/*
	 * If this is a new cnode then initialize it using descp and attrp...
	 */
	if (allocated) {
		/* The name was inherited so clear descriptor state... */
		descp->cd_namelen = 0;
		descp->cd_nameptr = NULL;
		descp->cd_flags &= ~CD_HASBUF;

		/* Tag hardlinks */
		if (IFTOVT(cp->c_mode) == VREG && (descp->cd_cnid != attrp->ca_fileid)) {
			cp->c_flag |= C_HARDLINK;
		}

		/* Take one dev reference for each non-directory cnode */
		if (IFTOVT(cp->c_mode) != VDIR) {
			cp->c_devvp = hfsmp->hfs_devvp;
			VREF(cp->c_devvp);
		}
#if QUOTA
		for (i = 0; i < MAXQUOTAS; i++)
			cp->c_dquot[i] = NODQUOT;
#endif /* QUOTA */
	}

	if (IFTOVT(cp->c_mode) != VDIR) {
		if (forkp && attrp->ca_blocks < forkp->cf_blocks)
			panic("hfs_getnewvnode: bad ca_blocks (too small)");
		/*
		 * Allocate and initialize a file fork...
		 */
		fp = (struct filefork *)malloc(sizeof(struct filefork), M_HFSFORK, M_WAITOK);
		bzero(fp, sizeof(struct filefork));
		fp->ff_cp = cp;
		if (forkp)
			bcopy(forkp, &fp->ff_data, sizeof(HFSPlusForkData));
		if (fp->ff_clumpsize == 0)
			fp->ff_clumpsize = HFSTOVCB(hfsmp)->vcbClpSiz;
		rl_init(&fp->ff_invalidranges);
		if (wantrsrc) {
			if (cp->c_rsrcfork != NULL)
				panic("stale rsrc fork");
			cp->c_rsrcfork = fp;
		} else {
			if (cp->c_datafork != NULL)
				panic("stale data fork");
			cp->c_datafork = fp;
		}
	}

	/*
	 * Finish vnode initialization.
	 * Setting the v_type 'stamps' the vnode as 'complete',
	 * so should be done almost last.
	 *
	 * At this point the vnode should be locked and fully
	 * allocated. And ready to be used or accessed. (though
	 * having it locked prevents most of this, it can still
	 * be accessed through lists and hashes).
	 */
	vp->v_type = IFTOVT(cp->c_mode);

	/* Tag system files */
	if ((descp->cd_cnid < kHFSFirstUserCatalogNodeID) && (vp->v_type == VREG))
		vp->v_vflag |= VV_SYSTEM;
	/* Tag root directory */
	if (cp->c_cnid == kRootDirID)
		vp->v_vflag |= VV_ROOT;

	if (vp->v_type == VCHR || vp->v_type == VBLK) {
		vp->v_op = &dead_vnodeops; // hfs_specop_p;
	} else if (vp->v_type == VFIFO) {
#if FIFO
		vp->v_op = hfs_fifoop_p;
#endif
	}

	/* Vnode is now initialized - see if anyone was waiting for it. */
	CLR(cp->c_flag, C_ALLOC);
	if (ISSET(cp->c_flag, C_WALLOC)) {
		CLR(cp->c_flag, C_WALLOC);
		wakeup((caddr_t)cp);
	}

	vn_set_state(vp, VSTATE_CONSTRUCTED);
	*vpp = vp;

	return (0);
}
