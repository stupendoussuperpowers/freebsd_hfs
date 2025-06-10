#include <sys/types.h>

#include <sys/param.h>

#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <sys/kernel.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

/*
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/dinode.h>
#include <libufs.h>
*/

#include <sys/conf.h>

#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/endian.h>

#include <hfsplus/hfs.h>
#include <hfsplus/hfs_endian.h>
#include <hfsplus/hfs_mount.h>

static MALLOC_DEFINE(M_HFSMNT, "HFS mount", "HFS mount data");

static int hfs_mountfs(struct vnode *devvp, struct mount *mp) {
	proc_t *p = curthread;
	int retval = E_NONE;
	struct hfsmount *hfsmp;
	struct bufobj *bo;
	struct buf *bp;
	HFSMasterDirectoryBlock *mdbp;
	int ronly;
	struct hfs_mount_args *args = NULL;
	int mntwrapper;
	struct ucred *cred;
	u_int64_t disksize;
	u_int64_t blkcnt;
	u_int32_t blksize;

	struct g_consumer *cp;

	u_int secsize;
	off_t medsize;

	daddr_t mdb_offset;

	cred = p ? p->td_proc->p_ucred : NOCRED;
	mntwrapper = 0;
	
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	retval = vinvalbuf(devvp, V_SAVE, 0, 0);
	VOP_UNLOCK(devvp);

	if (retval) {
		return retval;
	}

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	g_topology_lock();
	retval = g_vfs_open(devvp, &cp, "hfs", ronly ? 0 : 1);
	g_topology_unlock();
	VOP_UNLOCK(devvp);

	bo = &devvp->v_bufobj;
	bo->bo_private = cp;
	bo->bo_ops = g_vfs_bufops; 

	mp->mnt_stat.f_iosize = 4096; // 512? Unsure;

	if (retval) {
		return (retval);
	}

	bp = NULL;
	hfsmp = NULL;
	mdbp = NULL;

	secsize = cp->provider->sectorsize;
	medsize = cp->provider->mediasize;

	blksize = secsize;
	blkcnt = medsize / secsize;
	disksize = medsize;

	mdb_offset = HFS_PRI_SECTOR(blksize);

	bread(devvp, mdb_offset, blksize, cred, &bp);	

	mdbp = (HFSMasterDirectoryBlock *)MALLOC(kMDBSize, M_TEMP, M_WAITOK);
	bcopy(bp->b_data + HFS_PRI_OFFSET(blksize), mdbp, kMDBSize);
	brelse(bp);

	bp = NULL;

	hfsmp = (struct hfsmount *)MALLOC(sizeof(struct hfsmount), M_HFSMNT, M_WAITOK);
	bzero(hfsmp, sizeof(struct hfsmount));
	mtx_init(&hfsmp->hfs_renamelock, "hfs rename lock", NULL, MTX_DEF);
	
	/*
	 *  Init the volume information structure
	 */
	
	mp->mnt_data = (qaddr_t)hfsmp;
	hfsmp->hfs_bo = &devvp->v_bufobj;
	hfsmp->hfs_cp = cp;
	hfsmp->hfs_mp = mp;		  /* Make VFSTOHFS work */
	hfsmp->hfs_vcb.vcb_hfsmp = hfsmp; /* Make VCBTOHFS work */
	hfsmp->hfs_raw_dev = devvp->v_rdev;
	hfsmp->hfs_devvp = devvp;
	hfsmp->hfs_phys_block_size = blksize;
	hfsmp->hfs_phys_block_count = blkcnt;
	hfsmp->hfs_media_writeable = 1;
	hfsmp->hfs_fs_ronly = ronly;
	hfsmp->hfs_unknownpermissions = ((mp->mnt_flag & MNT_UNKNOWNPERMISSIONS) != 0);

#ifdef DARWIN_QUOTA
	for (i = 0; i < MAXQUOTAS; i++)
		hfsmp->hfs_qfiles[i].qf_vp = NULLVP;
#endif

	// int error;
	args = (struct hfs_mount_args *)MALLOC(sizeof(struct hfs_mount_args), M_HFSMNT, M_WAITOK);

	char *uidstr, *gidstr;

	retval = vfs_getopt(mp->mnt_optnew, "hfs_uid", (void **)&uidstr, NULL);
	retval = vfs_getopt(mp->mnt_optnew, "hfs_gid", (void **)&gidstr, NULL);

	args->hfs_uid = (uid_t)strtoul(uidstr, NULL, 10);
	args->hfs_gid = (gid_t)strtoul(gidstr, NULL, 10);

	if (args) {
		hfsmp->hfs_uid = (args->hfs_uid == (uid_t)VNOVAL) ? UNKNOWNUID : args->hfs_uid;

		if (hfsmp->hfs_uid == 0xfffffffd)
			hfsmp->hfs_uid = UNKNOWNUID;

		hfsmp->hfs_gid = (args->hfs_gid == (gid_t)VNOVAL) ? UNKNOWNGID : args->hfs_gid;

		if (hfsmp->hfs_gid == 0xfffffffd)
			hfsmp->hfs_gid = UNKNOWNGID;

		if (args->hfs_mask != (mode_t)VNOVAL) {
			hfsmp->hfs_dir_mask = args->hfs_mask & ALLPERMS;
			if (args->flags & HFSFSMNT_NOXONFILES) {
				hfsmp->hfs_file_mask = (args->hfs_mask & DEFFILEMODE);
			} else {
				hfsmp->hfs_file_mask = args->hfs_mask & ALLPERMS;
			}
		} else {
			hfsmp->hfs_dir_mask = UNKNOWNPERMISSIONS & ALLPERMS;
			/* 0777: rwx---rwx */
			hfsmp->hfs_file_mask = UNKNOWNPERMISSIONS & DEFFILEMODE;
			/* 0666: no --x by default? */
		}
		if ((args->flags != (int)VNOVAL) && (args->flags & HFSFSMNT_WRAPPER))
			mntwrapper = 1;
	} else {
		/* Even w/o explicit mount arguments, MNT_UNKNOWNPERMISSIONS
		 * requires setting up uid, gid, and mask: */
		if (mp->mnt_flag & MNT_UNKNOWNPERMISSIONS) {
			hfsmp->hfs_uid = UNKNOWNUID;
			hfsmp->hfs_gid = UNKNOWNGID;
			hfsmp->hfs_dir_mask = UNKNOWNPERMISSIONS & ALLPERMS;
			/* 0777: rwx---rwx */
			hfsmp->hfs_file_mask = UNKNOWNPERMISSIONS & DEFFILEMODE;
			/* 0666 : no-- x by default ? */
		}
	}

	hfsmp->hfs_media_writeable = 1;
	/* YYY how to know if media is writable? */

	int hfsStandard = (SWAP_BE16(mdbp->drSigWord) == kHFSSigWord) && (mntwrapper || (SWAP_BE16(mdbp->drEmbedSigWord) != kHFSPlusSigWord));

	int hfsEmbedded = SWAP_BE16(mdbp->drEmbedSigWord) == kHFSPlusSigWord;

	/* Mount a standard HFS disk */
	if (hfsStandard) {
		/* HFS disks can only use 512 byte physical blocks */
		if (blksize > kHFSBlockSize) {
			printf("HFS Mount: unsupported physical block size (%u)\n", (u_int)blksize);
			retval = EINVAL;
			goto error_exit;
		}
		if (args) {
			hfsmp->hfs_encoding = args->hfs_encoding;
			HFSTOVCB(hfsmp)->volumeNameEncodingHint = args->hfs_encoding;

			/* establish the timezone */
			gTimeZone = args->hfs_timezone;
		}

		retval = hfs_getconverter(hfsmp->hfs_encoding, &hfsmp->hfs_get_unicode, &hfsmp->hfs_get_hfsname);
		if (retval) {
			// return (retval);
			goto error_exit;
		}

		retval = hfs_MountHFSVolume(hfsmp, mdbp, p);
		if (retval) {
			(void)hfs_relconverter(hfsmp->hfs_encoding);
		}
	} else { /* Mount an HFS Plus disk */
		HFSPlusVolumeHeader *vhp;
		off_t embeddedOffset;

#ifdef DARWIN_JOURNAL
		int jnl_disable = 0;
#endif
		/* Get the embedded Volume Header */
		if (hfsEmbedded) {
			embeddedOffset = SWAP_BE16(mdbp->drAlBlSt) * kHFSBlockSize;

			embeddedOffset += (u_int64_t)SWAP_BE16(mdbp->drEmbedExtent.startBlock) * (u_int64_t)SWAP_BE32(mdbp->drAlBlkSiz);

			/*
			 * If the embedded volume doesn't start on a block
			 * boundary, then switch the device to a 512-byte
			 * block size so everything will line up on a block
			 * boundary.
			 */
			if ((embeddedOffset % blksize) != 0) {
				printf("HFS Mount: embedded volume offset not"
				       " a multiple of physical block size\n");
				retval = EINVAL;
				goto error_exit;
			}

			disksize = (u_int64_t)SWAP_BE16(mdbp->drEmbedExtent.blockCount) * (u_int64_t)SWAP_BE32(mdbp->drAlBlkSiz);

			hfsmp->hfs_phys_block_count = disksize / blksize;

			mdb_offset = (embeddedOffset / blksize) + HFS_PRI_SECTOR(blksize);

			retval = bread(devvp, mdb_offset, blksize, cred, &bp);

			if (retval)
				goto error_exit;

			bcopy(bp->b_data + HFS_PRI_OFFSET(blksize), mdbp, 512);
			brelse(bp);
			bp = NULL;
			vhp = (HFSPlusVolumeHeader *)mdbp;
		} else { /* pure HFS+ */
			embeddedOffset = 0;
			vhp = (HFSPlusVolumeHeader *)mdbp;
		}

#ifdef DARWIN_JOURNAL
		// XXXdbg

		hfsmp->jnl = NULL;
		hfsmp->jvp = NULL;
		if (args != NULL && (args->flags & HFSFSMNT_EXTENDED_ARGS) && args->journal_disable) {
			jnl_disable = 1;
		}
#endif

#ifdef DARWIN_JOURNAL
		// We only initialize the journal here if the last person
		// to mount this volume was journaling aware.  Otherwise
		// we delay journal initialization until later at the end
		// of hfs_MountHFSPlusVolume() because the last person who
		// mounted it could have messed things up behind our back
		// (so we need to go find the .journal file, make sure it's
		// the right size, re-sync up if it was moved, etc).

		if ((SWAP_BE32(vhp->lastMountedVersion) == kHFSJMountVersion) && (SWAP_BE32(vhp->attributes) & kHFSVolumeJournaledMask) && !jnl_disable) {
			// if we're able to init the journal, mark the mount
			// point as journaled.

			if (hfs_early_journal_init(hfsmp, vhp, args, embeddedOffset, mdb_offset, mdbp, cred) == 0) {
				mp->mnt_flag |= MNT_JOURNALED;
			} else {
				retval = EINVAL;
				goto error_exit;
			}
		}
		// XXXdbg
#endif /* DARWIN_JOURNAL */

		(void)hfs_getconverter(0, &hfsmp->hfs_get_unicode, &hfsmp->hfs_get_hfsname);

		retval = hfs_MountHFSPlusVolume(hfsmp, vhp, embeddedOffset, disksize, p, args);

		if (retval) {
			(void)hfs_relconverter(0);
		}
	}

	if (retval) {
		goto error_exit;
	}


	vfs_getnewfsid(mp);
	mp->mnt_flag |= MNT_LOCAL;
	devvp->v_rdev->si_mountpt = mp; /* used by vfs_mountedon() */

	if (ronly == 0) {
		(void)hfs_flushvolumeheader(hfsmp, MNT_WAIT, 0);
	}
	
	free(mdbp, M_TEMP);
	free(args, M_HFSMNT);

	return 0;

error_exit:
	if (cp != NULL) {
		g_topology_lock();
		g_vfs_close(cp);
		g_topology_unlock();
	}
	if (bp)
		brelse(bp);
	if (mdbp)
		free(mdbp, M_TEMP);

#ifdef DARWIN_JOURNAL
	if (hfsmp && hfsmp->jvp && hfsmp->jvp != hfsmp->hfs_devvp) {
		(void)VOP_CLOSE(hfsmp->jvp, ronly ? FREAD : FREAD | FWRITE, cred, p);
		hfsmp->jvp = NULL;
	}
#endif

	if (hfsmp) {
		mtx_destroy(&hfsmp->hfs_renamelock);
		free(hfsmp, M_HFSMNT);
		mp->mnt_data = (qaddr_t)0;
	}

	if (args) {
		free(args, M_HFSMNT);
	}
	return retval;
}



static int hfs_mount(struct mount *mp) {
	struct vnode *devvp; //, *rootvp;
	struct nameidata nd, *ndp = &nd;
	int retval = E_NONE;
	mode_t accessmode;
	char *path;
	char *from;
	proc_t *p = curthread;

	// Get fspath, from
	vfs_getopt(mp->mnt_optnew, "fspath", (void **)&path, NULL);

	// Enables us to use strncpy at the end when registering the mount.
	if (strlen(path) >= MNAMELEN)
		return (ENAMETOOLONG);

	int from_size;
	vfs_getopt(mp->mnt_optnew, "from", (void **)&from, &from_size);

	//
	// Use NULL path to indicate we are mounting the root filesystem.
	//
	if (path == NULL) {
		printf("Mounting HFS+ as root not supported.\n");
		return ENOTSUP;
	}

	// 
	// If updating, check whether changing from read-only to
	// read/write; if there is no device name, that's all we do.

	//
	// Not an update, or updating the name: look up the name
	// and verify that it refers to a sensible block device.
	//
	
	NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, from);
	retval = namei(ndp);
	if (retval != E_NONE) {
		return (retval);
	}

	devvp = ndp->ni_vp;
	NDFREE_PNBUF(ndp);

	if (!vn_isdisk_error(devvp, &retval)) {
		vrele(devvp);
		return (retval);
	}
	
	//
	// If mount by non-root, then verify that user has necessary
	// permissions on the device.
	//

	if (p->td_proc->p_ucred->cr_uid != 0) {
		accessmode = VREAD;
		if ((mp->mnt_flag & MNT_RDONLY) == 0)
			accessmode |= VWRITE;
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		if ((retval = VOP_ACCESS(devvp, accessmode, p->td_proc->p_ucred, p))) {
			vput(devvp);
			return (retval);
		}
		VOP_UNLOCK(devvp);
	}

	if ((mp->mnt_flag & MNT_UPDATE) == 0) {
		retval = hfs_mountfs(devvp, mp);
		if (retval != E_NONE) {
			vrele(devvp);
		}
	} else {
		vrele(devvp);
	}

	if (retval != E_NONE) {
		return (retval);
	}

	vfs_mountedfrom(mp, from);
	return (0);
}



static int hfs_root(struct mount *mp, int flags, struct vnode **vpp) {
	struct vnode *nvp;
	int retval;
	UInt32 rootObjID = kRootDirID;

	if ((retval = VFS_VGET(mp, rootObjID, LK_EXCLUSIVE, &nvp))) {
		return (retval);
	}

	*vpp = nvp;
	return (0);
}

static int hfs_statfs(struct mount *mp, struct statfs *sbp) {
	ExtendedVCB* vcb = VFSTOVCB(mp);
	struct hfsmount* hfsmp = VFSTOHFS(mp);
	u_long freeCNIDs;

	freeCNIDs = (u_long)0xFFFFFFFF - (u_long)vcb->vcbNxtCNID;
	
	sbp->f_bsize = vcb->blockSize;
	sbp->f_iosize = hfsmp->hfs_logBlockSize;
	sbp->f_blocks = vcb->totalBlocks;
	sbp->f_bfree = hfs_freeblks(hfsmp, 0);
	sbp->f_bavail = hfs_freeblks(hfsmp, 1);
	sbp->f_files = vcb->totalBlocks - 2;
	sbp->f_ffree = ulmin(freeCNIDs, sbp->f_bavail);

	sbp->f_type = 0;

	if (sbp != &mp->mnt_stat) {
		sbp->f_type = mp->mnt_vfc->vfc_typenum;
		bcopy((caddr_t)mp->mnt_stat.f_mntonname, (caddr_t)&sbp->f_mntonname[0], MNAMELEN);
		bcopy((caddr_t)mp->mnt_stat.f_mntfromname, (caddr_t)&sbp->f_mntfromname[0], MNAMELEN);
	}
	
	return 0;
}

static int hfs_sync(struct mount *mp, int waitfor) {
	printf("--- hfs_sync --- stub --- \n");
	return 0;
}

static int hfs_unmount(struct mount *mp, int mntflags) {
	struct hfsmount *hfsmp = VFSTOHFS(mp);
	int retval = E_NONE;
	int flags;
	int force;
	proc_t *p = curthread;

#ifdef DARWIN_JOURNAL
	int started_tr = 0, grabbed_lock = 0;
#endif

	flags = 0;
	force = 0;
	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
		force = 1;
	}

	if ((retval = hfs_flushfiles(mp, flags, p)) && !force)
		return (retval);

	/*
	 * Flush out the b-trees, volume bitmap and Volume Header
	 */
	if (hfsmp->hfs_fs_ronly == 0) {
#ifdef DARWIN_JOURNAL
		hfs_global_shared_lock_acquire(hfsmp);
		grabbed_lock = 1;
		if (hfsmp->jnl) {
			journal_start_transaction(hfsmp->jnl);
			started_tr = 1;
		}
#endif

		vn_lock(HFSTOVCB(hfsmp)->catalogRefNum, LK_EXCLUSIVE | LK_RETRY);
		retval = VOP_FSYNC(HFSTOVCB(hfsmp)->catalogRefNum, MNT_WAIT, p);
		VOP_UNLOCK(HFSTOVCB(hfsmp)->catalogRefNum);
		if (retval && !force)
			goto err_exit;

		vn_lock(HFSTOVCB(hfsmp)->extentsRefNum, LK_EXCLUSIVE | LK_RETRY);
		retval = VOP_FSYNC(HFSTOVCB(hfsmp)->extentsRefNum, MNT_WAIT, p);
		VOP_UNLOCK(HFSTOVCB(hfsmp)->extentsRefNum);
		if (retval && !force)
			goto err_exit;

		// if we have an allocation file, sync it too so we don't leave dirty
		// blocks around
		if (HFSTOVCB(hfsmp)->allocationsRefNum) {
			vn_lock(HFSTOVCB(hfsmp)->allocationsRefNum, LK_EXCLUSIVE | LK_RETRY);
			retval = VOP_FSYNC(HFSTOVCB(hfsmp)->allocationsRefNum, MNT_WAIT, p);
			VOP_UNLOCK(HFSTOVCB(hfsmp)->allocationsRefNum);
			if (retval && !force)
				goto err_exit;
		}

		vn_lock(hfsmp->hfs_devvp, LK_EXCLUSIVE | LK_RETRY);
		retval = VOP_FSYNC(hfsmp->hfs_devvp, MNT_WAIT, p);
		VOP_UNLOCK(hfsmp->hfs_devvp);	

		if (retval && !force)
			goto err_exit;

		/* See if this volume is damaged, is so do not unmount cleanly */
		if (HFSTOVCB(hfsmp)->vcbFlags & kHFS_DamagedVolume) {
			HFSTOVCB(hfsmp)->vcbAtrb &= ~kHFSVolumeUnmountedMask;
		} else {
			HFSTOVCB(hfsmp)->vcbAtrb |= kHFSVolumeUnmountedMask;
		}

		retval = hfs_flushvolumeheader(hfsmp, MNT_WAIT, 1);
		if (retval) {
			HFSTOVCB(hfsmp)->vcbAtrb &= ~kHFSVolumeUnmountedMask;
			if (!force)
				goto err_exit; /* could not flush everything */
		}

#ifdef DARWIN_JOURNAL
		if (hfsmp->jnl) {
			journal_end_transaction(hfsmp->jnl);
			started_tr = 0;
		}
		if (grabbed_lock) {
			hfs_global_shared_lock_release(hfsmp);
			grabbed_lock = 0;
		}
#endif
	}

#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl) {
		journal_flush(hfsmp->jnl);
	}
#endif
	/*
	 *	Invalidate our caches and release metadata vnodes
	 */
	(void) hfsUnmount(hfsmp, p);

	if (HFSTOVCB(hfsmp)->vcbSigWord == kHFSSigWord){
		(void)hfs_relconverter(hfsmp->hfs_encoding);
	}

#ifdef DARWIN_JOURNAL
	// XXXdbg
	if (hfsmp->jnl) {
		journal_close(hfsmp->jnl);
	}

	if (hfsmp->jvp && hfsmp->jvp != hfsmp->hfs_devvp) {
		retval = VOP_CLOSE(hfsmp->jvp, hfsmp->hfs_fs_ronly ? FREAD : FREAD | FWRITE, NOCRED, p);
		vrele(hfsmp->jvp);
		hfsmp->jvp = NULL;
	}
	// XXXdbg
#endif /* DARWIN_JOURNAL */

	g_topology_lock();
	g_vfs_close(hfsmp->hfs_cp);
	g_topology_unlock();
	
	hfsmp->hfs_cp = NULL;
	vrele(hfsmp->hfs_devvp);

	mtx_destroy(&hfsmp->hfs_renamelock);
	free(hfsmp, M_HFSMNT);

	mp->mnt_data = (qaddr_t)0;

	return (0);

err_exit:
#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl && started_tr) {
		journal_end_transaction(hfsmp->jnl);
	}
	if (grabbed_lock) {
		hfs_global_shared_lock_release(hfsmp);
	}
#endif
	return retval;
}

int hfs_flushfiles(struct mount *mp, int flags, proc_t *p) {
	// register struct hfsmount *hfsmp;
	int error;

	/*
#if QUOTA
	hfsmp = VFSTOHFS(mp);

	if (mp->mnt_flag & MNT_QUOTA) {
		if (error = vflush(mp, NULLVP, SKIPSYSTEM | flags))
			return (error);
		for (i = 0; i < MAXQUOTAS; i++) {
			if (hfsmp->hfs_qfiles[i].qf_vp == NULLVP)
				continue;
			hfs_quotaoff(p, mp, i);
		}
		
		 // Here we fall through to vflush again to ensure
		 // that we have gotten rid of all the system vnodes.
		 
	}
#endif // QUOTA 
*/ 

#ifdef DARWIN
	error = vflush(mp, NULLVP, (SKIPSYSTEM | SKIPSWAP | flags));
	error = vflush(mp, NULLVP, (SKIPSYSTEM | flags));
#else
	error = vflush(mp, 0, (SKIPSYSTEM | flags), p);
#endif

	return (error);
}

static int hfs_vget(struct mount *mp, ino_t ino, int flags, struct vnode **vpp) {
	cnid_t cnid = ino;

	/* Check for cnids that should't be exported. */
	if ((cnid < kHFSFirstUserCatalogNodeID) && (cnid != kHFSRootFolderID && cnid != kHFSRootParentID)) {
		return ENOENT;
	}
	/* Don't export HFS Private Data dir. */
	if (cnid == VFSTOHFS(mp)->hfs_privdir_desc.cd_cnid)
		return ENOENT;

	if (flags != LK_EXCLUSIVE)
		printf("hfs_vget: incompatible lock flags (%#x)\n", flags);

	return hfs_getcnode(VFSTOHFS(mp), cnid, NULL, 0, NULL, NULL, vpp);
}

static int hfs_flushMDB(struct hfsmount *hfsmp, int waitfor, int altflush) {
	ExtendedVCB *vcb = HFSTOVCB(hfsmp);
	struct filefork *fp;
	HFSMasterDirectoryBlock *mdb;
	struct buf *bp = NULL;
	int retval;
	int sectorsize;
	ByteCount namelen;

	sectorsize = hfsmp->hfs_phys_block_size;
	retval = bread(hfsmp->hfs_devvp, HFS_PRI_SECTOR(sectorsize), sectorsize, NOCRED, &bp);
	if (retval) {
		if (bp)
			brelse(bp);
		return retval;
	}


#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl)
		panic("hfs: standard hfs volumes should not be journaled!\n");
#endif

	mdb = (HFSMasterDirectoryBlock *)(bp->b_data + HFS_PRI_OFFSET(sectorsize));

	mdb->drCrDate = SWAP_BE32(UTCToLocal(to_hfs_time(vcb->vcbCrDate)));
	mdb->drLsMod = SWAP_BE32(UTCToLocal(to_hfs_time(vcb->vcbLsMod)));
	mdb->drAtrb = SWAP_BE16(vcb->vcbAtrb);
	mdb->drNmFls = SWAP_BE16(vcb->vcbNmFls);
	mdb->drAllocPtr = SWAP_BE16(vcb->nextAllocation);
	mdb->drClpSiz = SWAP_BE32(vcb->vcbClpSiz);
	mdb->drNxtCNID = SWAP_BE32(vcb->vcbNxtCNID);
	mdb->drFreeBks = SWAP_BE16(vcb->freeBlocks);

	namelen = strlen(vcb->vcbVN);
	retval = utf8_to_hfs(vcb, namelen, vcb->vcbVN, mdb->drVN);
	/* Retry with MacRoman in case that's how it was exported. */
	if (retval)
		retval = utf8_to_mac_roman(namelen, vcb->vcbVN, mdb->drVN);

	mdb->drVolBkUp = SWAP_BE32(UTCToLocal(to_hfs_time(vcb->vcbVolBkUp)));
	mdb->drWrCnt = SWAP_BE32(vcb->vcbWrCnt);
	mdb->drNmRtDirs = SWAP_BE16(vcb->vcbNmRtDirs);
	mdb->drFilCnt = SWAP_BE32(vcb->vcbFilCnt);
	mdb->drDirCnt = SWAP_BE32(vcb->vcbDirCnt);

	bcopy(vcb->vcbFndrInfo, mdb->drFndrInfo, sizeof(mdb->drFndrInfo));

	fp = VTOF(vcb->extentsRefNum);
	mdb->drXTExtRec[0].startBlock = SWAP_BE16(fp->ff_extents[0].startBlock);
	mdb->drXTExtRec[0].blockCount = SWAP_BE16(fp->ff_extents[0].blockCount);
	mdb->drXTExtRec[1].startBlock = SWAP_BE16(fp->ff_extents[1].startBlock);
	mdb->drXTExtRec[1].blockCount = SWAP_BE16(fp->ff_extents[1].blockCount);
	mdb->drXTExtRec[2].startBlock = SWAP_BE16(fp->ff_extents[2].startBlock);
	mdb->drXTExtRec[2].blockCount = SWAP_BE16(fp->ff_extents[2].blockCount);
	mdb->drXTFlSize = SWAP_BE32(fp->ff_blocks * vcb->blockSize);
	mdb->drXTClpSiz = SWAP_BE32(fp->ff_clumpsize);

	fp = VTOF(vcb->catalogRefNum);
	mdb->drCTExtRec[0].startBlock = SWAP_BE16(fp->ff_extents[0].startBlock);
	mdb->drCTExtRec[0].blockCount = SWAP_BE16(fp->ff_extents[0].blockCount);
	mdb->drCTExtRec[1].startBlock = SWAP_BE16(fp->ff_extents[1].startBlock);
	mdb->drCTExtRec[1].blockCount = SWAP_BE16(fp->ff_extents[1].blockCount);
	mdb->drCTExtRec[2].startBlock = SWAP_BE16(fp->ff_extents[2].startBlock);
	mdb->drCTExtRec[2].blockCount = SWAP_BE16(fp->ff_extents[2].blockCount);
	mdb->drCTFlSize = SWAP_BE32(fp->ff_blocks * vcb->blockSize);
	mdb->drCTClpSiz = SWAP_BE32(fp->ff_clumpsize);

	/* If requested, flush out the alternate MDB */
	if (altflush) {
		struct buf *alt_bp = NULL;
		u_long altIDSector;

		altIDSector = HFS_ALT_SECTOR(sectorsize, hfsmp->hfs_phys_block_count);

		if (meta_bread(hfsmp->hfs_devvp, altIDSector, sectorsize, NOCRED, &alt_bp) == 0) {
			bcopy(mdb, alt_bp->b_data + HFS_ALT_OFFSET(sectorsize), kMDBSize);

			(void)VOP_BWRITE(alt_bp);
		} else if (alt_bp) {
			brelse(alt_bp);
		}
	}

	if (waitfor != MNT_WAIT)
		bawrite(bp);
	else
		retval = VOP_BWRITE(bp);

	MarkVCBClean(vcb);

	return retval;
}

int hfs_flushvolumeheader(struct hfsmount *hfsmp, int waitfor, int altflush) {
	ExtendedVCB *vcb = HFSTOVCB(hfsmp);
	struct filefork *fp;
	HFSPlusVolumeHeader *volumeHeader;
	int retval;
	struct buf *bp;
	int i;
	int sectorsize;
	int priIDSector;

	if (vcb->vcbSigWord == kHFSSigWord)
		return hfs_flushMDB(hfsmp, waitfor, altflush);

	sectorsize = hfsmp->hfs_phys_block_size;
	priIDSector = (vcb->hfsPlusIOPosOffset / sectorsize) + HFS_PRI_SECTOR(sectorsize);

#ifdef DARWIN_JOURNAL
	// XXXdbg
	hfs_global_shared_lock_acquire(hfsmp);
	if (hfsmp->jnl) {
		if (journal_start_transaction(hfsmp->jnl) != 0) {
			hfs_global_shared_lock_release(hfsmp);
			return EINVAL;
		}
	}
#endif

	retval = meta_bread(hfsmp->hfs_devvp, priIDSector, sectorsize, NOCRED, &bp);
	if (retval) {
		if (bp)
			brelse(bp);

#ifdef DARWIN_JOURNAL
		if (hfsmp->jnl)
			journal_end_transaction(hfsmp->jnl);
		hfs_global_shared_lock_release(hfsmp);
#endif

		return retval;
	}

#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl)
		journal_modify_block_start(hfsmp->jnl, bp);
#endif

	volumeHeader = (HFSPlusVolumeHeader *)((char *)bp->b_data + HFS_PRI_OFFSET(sectorsize));

	/*
	 * For embedded HFS+ volumes, update create date if it changed
	 * (ie from a setattrlist call)
	 */
	if ((vcb->hfsPlusIOPosOffset != 0) && (SWAP_BE32(volumeHeader->createDate) != vcb->localCreateDate)) {
		struct buf *bp2;
		HFSMasterDirectoryBlock *mdb;

		retval = meta_bread(hfsmp->hfs_devvp, HFS_PRI_SECTOR(sectorsize), sectorsize, NOCRED, &bp2);
		if (retval) {
			if (bp2)
				brelse(bp2);
			retval = 0;
		} else {
			mdb = (HFSMasterDirectoryBlock *)(bp2->b_data + HFS_PRI_OFFSET(sectorsize));

			if (SWAP_BE32(mdb->drCrDate) != vcb->localCreateDate) {
#ifdef DARWIN_JOURNAL
				// XXXdbg
				if (hfsmp->jnl)
					journal_modify_block_start(hfsmp->jnl, bp2);
#endif

				mdb->drCrDate = SWAP_BE32(vcb->localCreateDate); /* pick up the new
										    create date */

#ifdef DARWIN_JOURNAL
				// XXXdbg
				if (hfsmp->jnl) {
					journal_modify_block_end(hfsmp->jnl, bp2);
				} else
#endif
				{
					(void)VOP_BWRITE(bp2); /* write out the changes */
				}
			} else {
				brelse(bp2); /* just release it */
			}
		}
	}

// XXXdbg - only monkey around with the volume signature on non-root volumes
//
#if 0
#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl &&
		hfsmp->hfs_fs_ronly == 0 &&
		(HFSTOVFS(hfsmp)->mnt_flag & MNT_ROOTFS) == 0) {
		
		int old_sig = volumeHeader->signature;

		if (vcb->vcbAtrb & kHFSVolumeUnmountedMask) {
			volumeHeader->signature = kHFSPlusSigWord;
		} else {
			volumeHeader->signature = kHFSJSigWord;
		}

		if (old_sig != volumeHeader->signature) {
			altflush = 1;
		}
	}
#endif
#endif
	// XXXdbg

	/* Note: only update the lower 16 bits worth of attributes */
	volumeHeader->attributes = SWAP_BE32((SWAP_BE32(volumeHeader->attributes) & 0xFFFF0000) + (UInt16)vcb->vcbAtrb);
	volumeHeader->journalInfoBlock = SWAP_BE32(vcb->vcbJinfoBlock);
#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl) {
		volumeHeader->lastMountedVersion = SWAP_BE32(kHFSJMountVersion);
	} else
#endif
	{
		volumeHeader->lastMountedVersion = SWAP_BE32(kHFSPlusMountVersion);
	}
	volumeHeader->createDate = SWAP_BE32(vcb->localCreateDate); /* volume create date is in local time */
	volumeHeader->modifyDate = SWAP_BE32(to_hfs_time(vcb->vcbLsMod));
	volumeHeader->backupDate = SWAP_BE32(to_hfs_time(vcb->vcbVolBkUp));
	volumeHeader->fileCount = SWAP_BE32(vcb->vcbFilCnt);
	volumeHeader->folderCount = SWAP_BE32(vcb->vcbDirCnt);
	volumeHeader->freeBlocks = SWAP_BE32(vcb->freeBlocks);
	volumeHeader->nextAllocation = SWAP_BE32(vcb->nextAllocation);
	volumeHeader->rsrcClumpSize = SWAP_BE32(vcb->vcbClpSiz);
	volumeHeader->dataClumpSize = SWAP_BE32(vcb->vcbClpSiz);
	volumeHeader->nextCatalogID = SWAP_BE32(vcb->vcbNxtCNID);
	volumeHeader->writeCount = SWAP_BE32(vcb->vcbWrCnt);
	volumeHeader->encodingsBitmap = SWAP_BE64(vcb->encodingsBitmap);

	if (bcmp(vcb->vcbFndrInfo, volumeHeader->finderInfo, sizeof(volumeHeader->finderInfo)) != 0) {
		bcopy(vcb->vcbFndrInfo, volumeHeader->finderInfo, sizeof(volumeHeader->finderInfo));
	}

	/* Sync Extents over-flow file meta data */
	fp = VTOF(vcb->extentsRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		volumeHeader->extentsFile.extents[i].startBlock = SWAP_BE32(fp->ff_extents[i].startBlock);
		volumeHeader->extentsFile.extents[i].blockCount = SWAP_BE32(fp->ff_extents[i].blockCount);
	}
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	volumeHeader->extentsFile.logicalSize = SWAP_BE64(fp->ff_size);
	volumeHeader->extentsFile.totalBlocks = SWAP_BE32(fp->ff_blocks);
	volumeHeader->extentsFile.clumpSize = SWAP_BE32(fp->ff_clumpsize);

	/* Sync Catalog file meta data */
	fp = VTOF(vcb->catalogRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		volumeHeader->catalogFile.extents[i].startBlock = SWAP_BE32(fp->ff_extents[i].startBlock);
		volumeHeader->catalogFile.extents[i].blockCount = SWAP_BE32(fp->ff_extents[i].blockCount);
	}
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	volumeHeader->catalogFile.logicalSize = SWAP_BE64(fp->ff_size);
	volumeHeader->catalogFile.totalBlocks = SWAP_BE32(fp->ff_blocks);
	volumeHeader->catalogFile.clumpSize = SWAP_BE32(fp->ff_clumpsize);

	/* Sync Allocation file meta data */
	fp = VTOF(vcb->allocationsRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		volumeHeader->allocationFile.extents[i].startBlock = SWAP_BE32(fp->ff_extents[i].startBlock);
		volumeHeader->allocationFile.extents[i].blockCount = SWAP_BE32(fp->ff_extents[i].blockCount);
	}
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	volumeHeader->allocationFile.logicalSize = SWAP_BE64(fp->ff_size);
	volumeHeader->allocationFile.totalBlocks = SWAP_BE32(fp->ff_blocks);
	volumeHeader->allocationFile.clumpSize = SWAP_BE32(fp->ff_clumpsize);

	/* If requested, flush out the alternate volume header */
	if (altflush) {
		struct buf *alt_bp = NULL;
		u_long altIDSector;

		altIDSector = (vcb->hfsPlusIOPosOffset / sectorsize) + HFS_ALT_SECTOR(sectorsize, hfsmp->hfs_phys_block_count);

		if (meta_bread(hfsmp->hfs_devvp, altIDSector, sectorsize, NOCRED, &alt_bp) == 0) {
#ifdef DARWIN_JOURNAL
			if (hfsmp->jnl)
				journal_modify_block_start(hfsmp->jnl, alt_bp);
#endif

			bcopy(volumeHeader, alt_bp->b_data + HFS_ALT_OFFSET(sectorsize), kMDBSize);

#ifdef DARWIN_JOURNAL
			if (hfsmp->jnl) {
				journal_modify_block_end(hfsmp->jnl, alt_bp);
			} else
#endif
			{
				(void)VOP_BWRITE(alt_bp);
			}
		} else if (alt_bp) {
			brelse(alt_bp);
		}
	}

#ifdef DARWIN_JOURNAL
	// XXXdbg
	if (hfsmp->jnl) {
		journal_modify_block_end(hfsmp->jnl, bp);
		journal_end_transaction(hfsmp->jnl);
	} else
#endif
	{
		if (waitfor != MNT_WAIT) {
			bawrite(bp);
		} else {
			retval = VOP_BWRITE(bp);
#ifdef DARWIN
			/* When critical data changes, flush the device cache */
			if (critical && (retval == 0)) {
				(void)VOP_IOCTL(hfsmp->hfs_devvp, DKIOCSYNCHRONIZECACHE, NULL, FWRITE, NOCRED, current_proc());
			}
#endif
		}
	}
#ifdef DARWIN_JOURNAL
	hfs_global_shared_lock_release(hfsmp);
#endif

	vcb->vcbFlags &= 0x00FF;
	return retval;
}

void hfs_setencodingbits(struct hfsmount *hfsmp, u_int32_t encoding) {
#define kIndexMacUkrainian 48 /* MacUkrainian encoding is 152 */
#define kIndexMacFarsi 49     /* MacFarsi encoding is 140 */

	UInt32 index;

	switch (encoding) {
		case kTextEncodingMacUkrainian:
			index = kIndexMacUkrainian;
			break;
		case kTextEncodingMacFarsi:
			index = kIndexMacFarsi;
			break;
		default:
			index = encoding;
			break;
	}

	if (index < 128) {
		HFSTOVCB(hfsmp)->encodingsBitmap |= (1 << index);
		HFSTOVCB(hfsmp)->vcbFlags |= 0xFF00;
	}
}

int hfs_volupdate(struct hfsmount *hfsmp, enum volop op, int inroot) {
	ExtendedVCB *vcb;

	vcb = HFSTOVCB(hfsmp);
	vcb->vcbFlags |= 0xFF00;
	vcb->vcbLsMod = gettime();

	switch (op) {
		case VOL_UPDATE:
			break;
		case VOL_MKDIR:
			if (vcb->vcbDirCnt != 0xFFFFFFFF)
				++vcb->vcbDirCnt;
			if (inroot && vcb->vcbNmRtDirs != 0xFFFF)
				++vcb->vcbNmRtDirs;
			break;
		case VOL_RMDIR:
			if (vcb->vcbDirCnt != 0)
				--vcb->vcbDirCnt;
			if (inroot && vcb->vcbNmRtDirs != 0xFFFF)
				--vcb->vcbNmRtDirs;
			break;
		case VOL_MKFILE:
			if (vcb->vcbFilCnt != 0xFFFFFFFF)
				++vcb->vcbFilCnt;
			if (inroot && vcb->vcbNmFls != 0xFFFF)
				++vcb->vcbNmFls;
			break;
		case VOL_RMFILE:
			if (vcb->vcbFilCnt != 0)
				--vcb->vcbFilCnt;
			if (inroot && vcb->vcbNmFls != 0xFFFF)
				--vcb->vcbNmFls;
			break;
	}

#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl)
		hfs_flushvolumeheader(hfsmp, 0, 0);
#endif

	return 0;
}

static int hfs_init(struct vfsconf *vfsp) {
	static int done = 0;

	if (done) {
		return (0);
	}

	done = 1;

	hfs_chashinit();
	hfs_converterinit();
	// #if QUOTA
	//	dqinit();
	// #endif
	(void)InitCatalogCache();

	return (0);
}

static int hfs_uninit(struct vfsconf *vfsp) {
	DestroyCatalogCache();
	hfs_converterdestroy();
	hfs_chashdestroy();
	return (0);
}

//
//  Should the above two function be in utils instead? I guess so?
//  hfs_flushvolumeheaders
//  hfs_flushMDB
//

/*
* struct vfsops hfs_vfsops = {
    .vfs_fhtovp = hfs_fhtovp,
    .vfs_vptofh = hfs_vptofh,
}
*
*/

static struct vfsops hfs_vfsops = {
	.vfs_mount = hfs_mount,
	.vfs_root = hfs_root,
	.vfs_statfs = hfs_statfs,
	.vfs_sync = hfs_sync,
	.vfs_unmount = hfs_unmount,
	.vfs_vget = hfs_vget,
	.vfs_init = hfs_init,
	.vfs_uninit = hfs_uninit,
};

VFS_SET(hfs_vfsops, hfs, 0);
