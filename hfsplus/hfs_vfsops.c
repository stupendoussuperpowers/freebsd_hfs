#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/vnode.h>

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
	struct buf *bp;
	struct cdev *dev;
	HFSMasterDirectoryBlock *mdbp;
	int ronly;
	int mntwrapper;
	struct ucred *cred;
	u_int64_t disksize;
	u_int64_t blkcnt;
	u_int32_t blksize;

	u_int secsize;
	off_t medsize;

	daddr_t mdb_offset;

	dev = devvp->v_rdev;
	cred = p ? p->td_proc->p_ucred : NOCRED;
	mntwrapper = 0;
	/*
	 * Disallow multiple mounts of the same device.
	 * Disallow mounting of a device that is currently in use
	 * (except for root, which might share swap device for miniroot).
	 * Flush out any old buffers remaining from a previous use.
	 */
	if (dev->si_mountpt != NULL) {
		printf("Returning EBUSY\n");
		return EBUSY;
	}
	// if (vcount(devvp) > 1)
	//  return (EBUSY);

	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	retval = vinvalbuf(devvp, V_SAVE, 0, 0);
	VOP_UNLOCK(devvp);
	if (retval) {
		printf("vinvalbuf: %d\n", retval);
		return retval;
	}

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	// #if defined(__FreeBSD__) && __FreeBSD_version >= 501103 /* YYY no
	// bump */ 	retval = 	    VOP_OPEN(devvp, ronly ? FREAD :
	// FREAD | FWRITE, FSCRED, p, NULL); #else
	retval =
	    VOP_OPEN(devvp, ronly ? FREAD : FREAD | FWRITE, FSCRED, p, NULL);
	// #endif
	VOP_UNLOCK(devvp);
	if (retval) {
		printf("VOPOPEN retval: %d\n", retval);
		return retval;
	}

	bp = NULL;
	hfsmp = NULL;
	mdbp = NULL;

	/* Get the real physical block size. */
	printf("Trying first vop ioctl\n");
	if (VOP_IOCTL(devvp, DIOCGSECTORSIZE, (caddr_t)&secsize, 0, cred, p)) {
		retval = ENXIO;
		printf("first vop ioctl: %d\n", retval);
		goto error_exit;
	}
	printf("Trying second vop ioctl\n");
	if (VOP_IOCTL(devvp, DIOCGMEDIASIZE, (caddr_t)&medsize, 0, cred, p)) {
		retval = ENXIO;
		printf("second vop ioctl: %d\n", retval);
		goto error_exit;
	}
	printf("secsize: %d | medsize: %ld\n", secsize, medsize);
	blksize = secsize;
	blkcnt = medsize / secsize;
	disksize = medsize;

	printf("No panics so far.\n");
	printf("blksize: %d | HFS_PRI_SECTOR: %d \n", blksize, 1024 / blksize);

	mdb_offset = HFS_PRI_SECTOR(blksize);
	printf("mdb offset: %ld\n", mdb_offset);

	
	if ((retval = meta_bread(devvp, HFS_PRI_SECTOR(blksize), blksize, cred,
				 &bp))) {
		printf("meta bread retval: %d\n", retval);
		goto error_exit;
	}

	return (45);

	mdbp = (HFSMasterDirectoryBlock *)MALLOC(kMDBSize, M_TEMP, M_WAITOK);
	bcopy(bp->b_data + HFS_PRI_OFFSET(blksize), mdbp, kMDBSize);
	brelse(bp);

	bp = NULL;

	hfsmp = (struct hfsmount *)MALLOC(sizeof(struct hfsmount), M_HFSMNT,
					  M_WAITOK);
#if HFS_DIAGNOSTIC
	printf("hfsmount: allocated struct hfsmount at %p\n", hfsmp);
#endif
	bzero(hfsmp, sizeof(struct hfsmount));

	mtx_init(&hfsmp->hfs_renamelock, "hfs rename lock", NULL, MTX_DEF);

	/*
	 *  Init the volume information structure
	 */
	mp->mnt_data = (qaddr_t)hfsmp;
	hfsmp->hfs_mp = mp;		  /* Make VFSTOHFS work */
	hfsmp->hfs_vcb.vcb_hfsmp = hfsmp; /* Make VCBTOHFS work */
	hfsmp->hfs_raw_dev = devvp->v_rdev;
	hfsmp->hfs_devvp = devvp;
	hfsmp->hfs_phys_block_size = blksize;
	hfsmp->hfs_phys_block_count = blkcnt;
	hfsmp->hfs_media_writeable = 1;
	hfsmp->hfs_fs_ronly = ronly;
	hfsmp->hfs_unknownpermissions =
	    ((mp->mnt_flag & MNT_UNKNOWNPERMISSIONS) != 0);
#ifdef DARWIN_QUOTA
	for (i = 0; i < MAXQUOTAS; i++)
		hfsmp->hfs_qfiles[i].qf_vp = NULLVP;
#endif

	int error;
	struct hfs_mount_args *args = NULL;
	vfs_getopt(mp->mnt_optnew, "hfs_uid", (void **)&args->hfs_uid, &error);
	printf("HFS_Uid: %u", (unsigned int)args->hfs_uid);

	//  if (args) {
	// hfsmp->hfs_uid =
	//     (args->hfs_uid == (uid_t)VNOVAL) ? UNKNOWNUID : args->hfs_uid;
	// if (hfsmp->hfs_uid == 0xfffffffd)
	//   hfsmp->hfs_uid = UNKNOWNUID;
	// hfsmp->hfs_gid =
	//     (args->hfs_gid == (gid_t)VNOVAL) ? UNKNOWNGID : args->hfs_gid;
	// if (hfsmp->hfs_gid == 0xfffffffd)
	//   hfsmp->hfs_gid = UNKNOWNGID;
	// if (args->hfs_mask != (mode_t)VNOVAL) {
	//   hfsmp->hfs_dir_mask = args->hfs_mask & ALLPERMS;
	//   if (args->flags & HFSFSMNT_NOXONFILES) {
	//     hfsmp->hfs_file_mask = (args->hfs_mask & DEFFILEMODE);
	//   } else {
	//     hfsmp->hfs_file_mask = args->hfs_mask & ALLPERMS;
	//   }
	// } else {
	//   hfsmp->hfs_dir_mask = UNKNOWNPERMISSIONS & ALLPERMS; /* 0777:
	//   rwx---rwx
	//   */ hfsmp->hfs_file_mask =
	//       UNKNOWNPERMISSIONS & DEFFILEMODE; /* 0666: no --x by default?
	//       */
	// }
	// if ((args->flags != (int)VNOVAL) && (args->flags & HFSFSMNT_WRAPPER))
	//   mntwrapper = 1;
	//} else {
	/* Even w/o explicit mount arguments, MNT_UNKNOWNPERMISSIONS requires
	 * setting up uid, gid, and mask: */
	if (mp->mnt_flag & MNT_UNKNOWNPERMISSIONS) {
		hfsmp->hfs_uid = UNKNOWNUID;
		hfsmp->hfs_gid = UNKNOWNGID;
		hfsmp->hfs_dir_mask =
		    UNKNOWNPERMISSIONS & ALLPERMS; /* 0777: rwx---rwx */
		hfsmp->hfs_file_mask =
		    UNKNOWNPERMISSIONS &
		    DEFFILEMODE; /* 0666: no --x by default? */
	}
	// }

#ifdef DARWIN
	/* Find out if disk media is writable. */
	if (VOP_IOCTL(devvp, DKIOCISWRITABLE, (caddr_t)&iswritable, 0, cred,
		      p) == 0) {
		if (iswritable)
			hfsmp->hfs_media_writeable = 1;
		else
			hfsmp->hfs_media_writeable = 0;
	}
#else
	hfsmp->hfs_media_writeable =
	    1; /* YYY how to know if media is writable? */
#endif

	/* Mount a standard HFS disk */
	if ((SWAP_BE16(mdbp->drSigWord) == kHFSSigWord) &&
	    (mntwrapper ||
	     (SWAP_BE16(mdbp->drEmbedSigWord) != kHFSPlusSigWord))) {
#ifndef __FreeBSD__ /* YYY bug: must check if mounting root */
		if (devvp == rootvp) {
			retval =
			    EINVAL; /* Cannot root from HFS standard disks */
			goto error_exit;
		}
#endif
		/* HFS disks can only use 512 byte physical blocks */
		if (blksize > kHFSBlockSize) {
#ifdef DARWIN
			blksize = kHFSBlockSize;
			if (VOP_IOCTL(devvp, DKIOCSETBLOCKSIZE,
				      (caddr_t)&blksize, FWRITE, cred, p)) {
				retval = ENXIO;
				goto error_exit;
			}
			if (VOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT,
				      (caddr_t)&blkcnt, 0, cred, p)) {
				retval = ENXIO;
				goto error_exit;
			}
			devvp->v_specsize = blksize;
			hfsmp->hfs_phys_block_size = blksize;
			hfsmp->hfs_phys_block_count = blkcnt;
#else  /* !DARWIN */
			printf(
			    "HFS Mount: unsupported physical block size (%u)\n",
			    (u_int)blksize);
			retval = EINVAL;
			goto error_exit;
#endif /* DARWIN */
		}
		if (args) {
			hfsmp->hfs_encoding = args->hfs_encoding;
			HFSTOVCB(hfsmp)->volumeNameEncodingHint =
			    args->hfs_encoding;

			/* establish the timezone */
			gTimeZone = args->hfs_timezone;
		}

		retval = hfs_getconverter(hfsmp->hfs_encoding,
					  &hfsmp->hfs_get_unicode,
					  &hfsmp->hfs_get_hfsname);
		if (retval)
			goto error_exit;

		retval = hfs_MountHFSVolume(hfsmp, mdbp, p);
		if (retval)
			(void)hfs_relconverter(hfsmp->hfs_encoding);
	} else /* Mount an HFS Plus disk */
	{
		HFSPlusVolumeHeader *vhp;
		off_t embeddedOffset;
#ifdef DARWIN_JOURNAL
		int jnl_disable = 0;
#endif

		/* Get the embedded Volume Header */
		if (SWAP_BE16(mdbp->drEmbedSigWord) == kHFSPlusSigWord) {
			embeddedOffset =
			    SWAP_BE16(mdbp->drAlBlSt) * kHFSBlockSize;
			embeddedOffset +=
			    (u_int64_t)SWAP_BE16(
				mdbp->drEmbedExtent.startBlock) *
			    (u_int64_t)SWAP_BE32(mdbp->drAlBlkSiz);

			/*
			 * If the embedded volume doesn't start on a block
			 * boundary, then switch the device to a 512-byte
			 * block size so everything will line up on a block
			 * boundary.
			 */
			if ((embeddedOffset % blksize) != 0) {
#ifdef DARWIN
				printf(
				    "HFS Mount: embedded volume offset not"
				    " a multiple of physical block size (%d);"
				    " switching to 512\n",
				    blksize);
				blksize = 512;
				if (VOP_IOCTL(devvp, DKIOCSETBLOCKSIZE,
					      (caddr_t)&blksize, FWRITE, cred,
					      p)) {
					retval = ENXIO;
					goto error_exit;
				}
				if (VOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT,
					      (caddr_t)&blkcnt, 0, cred, p)) {
					retval = ENXIO;
					goto error_exit;
				}
				/* XXX do we need to call vfs_init_io_attributes
				 * again? */
				devvp->v_specsize = blksize;
				/* Note: relative block count adjustment */
				hfsmp->hfs_phys_block_count *=
				    hfsmp->hfs_phys_block_size / blksize;
				hfsmp->hfs_phys_block_size = blksize;
#else  /* !DARWIN */
				printf("HFS Mount: embedded volume offset not"
				       " a multiple of physical block size\n");
				retval = EINVAL;
				goto error_exit;
#endif /* DARWIN */
			}

			disksize = (u_int64_t)SWAP_BE16(
				       mdbp->drEmbedExtent.blockCount) *
				   (u_int64_t)SWAP_BE32(mdbp->drAlBlkSiz);

			hfsmp->hfs_phys_block_count = disksize / blksize;

			mdb_offset = (embeddedOffset / blksize) +
				     HFS_PRI_SECTOR(blksize);
			retval =
			    meta_bread(devvp, mdb_offset, blksize, cred, &bp);
			if (retval)
				goto error_exit;
			bcopy(bp->b_data + HFS_PRI_OFFSET(blksize), mdbp, 512);
			brelse(bp);
			bp = NULL;
			vhp = (HFSPlusVolumeHeader *)mdbp;
		} else /* pure HFS+ */
		{
			embeddedOffset = 0;
			vhp = (HFSPlusVolumeHeader *)mdbp;
		}

#ifdef DARWIN_JOURNAL
		// XXXdbg
		//
		hfsmp->jnl = NULL;
		hfsmp->jvp = NULL;
		if (args != NULL && (args->flags & HFSFSMNT_EXTENDED_ARGS) &&
		    args->journal_disable) {
			jnl_disable = 1;
		}
#endif

#ifdef DARWIN_JOURNAL
		//
		// We only initialize the journal here if the last person
		// to mount this volume was journaling aware.  Otherwise
		// we delay journal initialization until later at the end
		// of hfs_MountHFSPlusVolume() because the last person who
		// mounted it could have messed things up behind our back
		// (so we need to go find the .journal file, make sure it's
		// the right size, re-sync up if it was moved, etc).
		//
		if ((SWAP_BE32(vhp->lastMountedVersion) == kHFSJMountVersion) &&
		    (SWAP_BE32(vhp->attributes) & kHFSVolumeJournaledMask) &&
		    !jnl_disable) {
			// if we're able to init the journal, mark the mount
			// point as journaled.
			//
			if (hfs_early_journal_init(hfsmp, vhp, args,
						   embeddedOffset, mdb_offset,
						   mdbp, cred) == 0) {
				mp->mnt_flag |= MNT_JOURNALED;
			} else {
				retval = EINVAL;
				goto error_exit;
			}
		}
		// XXXdbg
#endif /* DARWIN_JOURNAL */

		(void)hfs_getconverter(0, &hfsmp->hfs_get_unicode,
				       &hfsmp->hfs_get_hfsname);

		retval = hfs_MountHFSPlusVolume(hfsmp, vhp, embeddedOffset,
						disksize, p, args);
#ifdef DARWIN
		/*
		 * If the backend didn't like our physical blocksize
		 * then retry with physical blocksize of 512.
		 */
		if ((retval == ENXIO) && (blksize > 512) &&
		    (blksize != minblksize)) {
			printf("HFS Mount: could not use physical block size "
			       "(%d) switching to 512\n",
			       blksize);
			blksize = 512;
			if (VOP_IOCTL(devvp, DKIOCSETBLOCKSIZE,
				      (caddr_t)&blksize, FWRITE, cred, p)) {
				retval = ENXIO;
				goto error_exit;
			}
			if (VOP_IOCTL(devvp, DKIOCGETBLOCKCOUNT,
				      (caddr_t)&blkcnt, 0, cred, p)) {
				retval = ENXIO;
				goto error_exit;
			}
			devvp->v_specsize = blksize;
			/* Note: relative block count adjustment (in case this
			 * is an embedded volume). */
			hfsmp->hfs_phys_block_count *=
			    hfsmp->hfs_phys_block_size / blksize;
			hfsmp->hfs_phys_block_size = blksize;

			/* Try again with a smaller block size... */
			retval = hfs_MountHFSPlusVolume(
			    hfsmp, vhp, embeddedOffset, disksize, p, args);
		}
#endif /* DARWIN */
		if (retval)
			(void)hfs_relconverter(0);
	}

	if (retval)
		goto error_exit;

#ifdef DARWIN
	mp->mnt_stat.f_fsid.val[0] = (long)dev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_maxsymlinklen = 0;
	devvp->v_specflags |= SI_MOUNTEDON;
#else
	vfs_getnewfsid(mp);
	mp->mnt_flag |= MNT_LOCAL;
	devvp->v_rdev->si_mountpt = mp; /* used by vfs_mountedon() */
#endif

	if (ronly == 0)
		(void)hfs_flushvolumeheader(hfsmp, MNT_WAIT, 0);
	free(mdbp, M_TEMP);
	return 0;

error_exit:
	if (bp)
		brelse(bp);
	if (mdbp)
		free(mdbp, M_TEMP);
	(void)VOP_CLOSE(devvp, ronly ? FREAD : FREAD | FWRITE, cred, p);
#ifdef DARWIN_JOURNAL
	if (hfsmp && hfsmp->jvp && hfsmp->jvp != hfsmp->hfs_devvp) {
		(void)VOP_CLOSE(hfsmp->jvp, ronly ? FREAD : FREAD | FWRITE,
				cred, p);
		hfsmp->jvp = NULL;
	}
#endif
	if (hfsmp) {
		mtx_destroy(&hfsmp->hfs_renamelock);
		free(hfsmp, M_HFSMNT);
		mp->mnt_data = (qaddr_t)0;
	}
	return retval;
}

static int hfs_mount(struct mount *mp) {
	printf("[ENTER] ---hfs_mount---\n");
	// struct hfsmount *hfsmp = NULL;
	struct vnode *devvp; //, *rootvp;
	// struct hfs_mount_args args;
	// struct vfsoptlist *opts;
	// struct vfsoptlist *optsold;
	struct nameidata ndp;
	// size_t size;
	int retval = E_NONE;
	// int flags;
	mode_t accessmode;
	char *path;
	char *from;

	vfs_getopt(mp->mnt_optnew, "fspath", (void **)&path, NULL);
	vfs_getopt(mp->mnt_optnew, "from", (void **)&from, NULL);

	proc_t *p = curthread;

	struct vfsopt *tempopt;
	printf("\n");
	TAILQ_FOREACH(tempopt, mp->mnt_optnew, link) {
		printf("%s\n", tempopt->name);
	}

#ifndef DARWIN
	//
	// Use NULL path to indicate we are mounting the root filesystem.
	//
	if (path == NULL) {
		// if ((retval = bdevvp(rootdev, &rootvp))) {
		//  printf("hfs_mountroot: can't find rootvp\n");
		//  return (retval);
		// }

		// if ((retval = hfs_mountfs(rootvp, mp)) != 0)
		//     return retval;

		// (void)VFS_STATFS(mp, &mp->mnt_stat, p);
		return 0;
	}
#endif

	// if ((retval = copyin(data, (caddr_t)&args, sizeof(args))))
	//  goto error_exit;
	//
	// If updating, check whether changing from read-only to
	// read/write; if there is no device name, that's all we do.

	printf("mnt_flags: %lu, MNT_UPDATE: %llu\n", mp->mnt_flag, MNT_UPDATE);

	/*
		if (mp->mnt_flag & MNT_UPDATE) {
			hfsmp = VFSTOHFS(mp);
			if ((hfsmp->hfs_fs_ronly == 0) && (mp->mnt_flag &
	MNT_RDONLY)) {
				// use VFS_SYNC to push out System (btree) files
				retval = VFS_SYNC(mp, MNT_WAIT,
	p->td_proc->p_ucred, p); if (retval && ((mp->mnt_flag & MNT_FORCE) ==
	0)) goto error_exit;

				flags = WRITECLOSE;
				if (mp->mnt_flag & MNT_FORCE)
					flags |= FORCECLOSE;

				if ((retval = hfs_flushfiles(mp, flags, p)))
					goto error_exit;
				hfsmp->hfs_fs_ronly = 1;
				retval = hfs_flushvolumeheader(hfsmp, MNT_WAIT,
	0);

				// also get the volume bitmap blocks
				if (!retval) {
					vn_lock(
					    hfsmp->hfs_devvp,
					    LK_EXCLUSIVE |
						LK_RETRY); // VOP_FSYNC takes vp
	locked retval = VOP_FSYNC(hfsmp->hfs_devvp, NOCRED, MNT_WAIT, p);
					VOP_UNLOCK(hfsmp->hfs_devvp);
				}

				if (retval) {
					hfsmp->hfs_fs_ronly = 0;
					goto error_exit;
				}
			}

			if ((mp->mnt_flag & MNT_RELOAD) &&
			    (retval = hfs_reload(mp, p->td_ucred, p))) {
				goto error_exit;
			}

			if (hfsmp->hfs_fs_ronly &&
			    (mp->mnt_kern_flag & MNTK_WANTRDWR)) {
				//
				// If upgrade to read-write by non-root, then
	verify
				// that user has necessary permissions on the
	device.
				//
				if (p->td_proc->p_ucred->cr_uid != 0) {
					devvp = hfsmp->hfs_devvp;
					vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
					if ((retval =
						 VOP_ACCESS(devvp, VREAD |
	VWRITE, p->td_proc->p_ucred, p))) { VOP_UNLOCK(devvp); goto error_exit;
					}
					VOP_UNLOCK(devvp);
				}
				retval = hfs_flushvolumeheader(hfsmp, MNT_WAIT,
	0);

				if (retval != E_NONE)
					goto error_exit;

				// only change hfs_fs_ronly after a successfull
	write hfsmp->hfs_fs_ronly = 0;
			}

			if ((hfsmp->hfs_fs_ronly == 0) &&
			    (HFSTOVCB(hfsmp)->vcbSigWord == kHFSPlusSigWord)) {
				// setup private/hidden directory for unlinked
	files hfsmp->hfs_private_metadata_dir =
				    FindMetaDataDirectory(HFSTOVCB(hfsmp));
	#ifdef DARWIN_JOURNAL
				if (hfsmp->jnl)
					hfs_remove_orphans(hfsmp);
	#endif
			}

			if (args.fspec == 0) {
				//
				// Process export requests.
				//
				return vfs_export(mp, &args.export);
			}
		}
	*/
	//
	// Not an update, or updating the name: look up the name
	// and verify that it refers to a sensible block device.
	//
	NDINIT(&ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, from);
	retval = namei(&ndp);
	printf("retval namei(): %d\n", retval);
	if (retval != E_NONE) {
		// DBG_ERR(("hfs_mount: CAN'T GET DEVICE: %s, %x\n", args.fspec,
		//	 ndp->ni_vp->v_rdev));
		// goto error_exit;
		return (retval);
	}

	devvp = ndp.ni_vp;
	printf("devvp: %p\n", devvp);

	NDFREE_PNBUF(&ndp);
	if (!vn_isdisk_error(devvp, &retval)) {
		printf("v_type: %d | VBLK: %d\n", devvp->v_type, VBLK);
		printf("vn_isdisk: %d\n", retval);
		vrele(devvp);
		return (retval);
	}
	/*
	#endif
	*/
	//
	// If mount by non-root, then verify that user has
	// necessary
	// permissions on the device.
	//
	printf("p->td_proc->p_ucred->cr_uid: %d\n",
	       p->td_proc->p_ucred->cr_uid);
	if (p->td_proc->p_ucred->cr_uid != 0) {
		accessmode = VREAD;
		if ((mp->mnt_flag & MNT_RDONLY) == 0)
			accessmode |= VWRITE;
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		if ((retval = VOP_ACCESS(devvp, accessmode, p->td_proc->p_ucred,
					 p))) {
			vput(devvp);
			return (retval);
		}
		VOP_UNLOCK(devvp);
	}

	if ((mp->mnt_flag & MNT_UPDATE) == 0) {
		printf("Not update\n");
		retval = hfs_mountfs(devvp, mp);
		if (retval != E_NONE) {
			printf("hfs_mountfs: %d\n", retval);
			vrele(devvp);
		}
	} else {
		printf("Change fs\n");
		// if (devvp != hfsmp->hfs_devvp) {
		// 	printf("devvp!=hfsmp->hfs_devvp\n");
		// 	retval = EINVAL; // needs translation
		// } else {
		// 	printf("trying hfs_changefs\n");
		// 	retval = hfs_changefs(mp, &args, p);
		// 	printf("changefs: %d", retval);
		// }
		vrele(devvp);
	}

	if (retval != E_NONE) {
		return (retval);
	}

	/*
		    (void)copyinstr(args.fspec, mp->mnt_stat.f_mntfromname,
	   MNAMELEN - 1, &size); bzero(mp->mnt_stat.f_mntfromname + size,
	   MNAMELEN - size); (void)hfs_statfs(mp, &mp->mnt_stat, p); return
	   E_NONE; error_exit:
		    */
	return (45);
}

static int hfs_cmount(struct mntarg *ma, void *data, uint64_t flags) {
	printf("---hfs_cmount---\n");

	return (45);
}

static int hfs_root(struct mount *mp, int flags, struct vnode **vpp) {
	printf("---hfs_root---\n");

	return 0;
}

static int hfs_statfs(struct mount *mp, struct statfs *sbp) {
	printf("---hfs_statfs---\n");
	sbp->f_bsize = 4096;
	sbp->f_blocks = 1000;
	sbp->f_bfree = 500;
	sbp->f_bavail = 500;
	sbp->f_files = 100;
	sbp->f_ffree = 50;

	return 0;
}

static int hfs_sync(struct mount *mp, int waitfor) {
	printf("---hfs_sync---\n");

	return 0;
}

static int hfs_unmount(struct mount *mp, int mntflags) {
	printf("---hfs_unmount---\n");

	free(mp->mnt_data, M_HFSMNT);
	mp->mnt_data = NULL;

	return 0;
}

static int hfs_vget(struct mount *mp, ino_t ino, int flags,
		    struct vnode **vpp) {
	cnid_t cnid = ino;

	/* Check for cnids that should't be exported. */
	if ((cnid < kHFSFirstUserCatalogNodeID) &&
	    (cnid != kHFSRootFolderID && cnid != kHFSRootParentID)) {
		return ENOENT;
	}
	/* Don't export HFS Private Data dir. */
	if (cnid == VFSTOHFS(mp)->hfs_privdir_desc.cd_cnid)
		return ENOENT;

	/* YYY flags should be passed down to vget() */
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
	retval = bread(hfsmp->hfs_devvp, HFS_PRI_SECTOR(sectorsize), sectorsize,
		       NOCRED, &bp);
	if (retval) {
		if (bp)
			brelse(bp);
		return retval;
	}

	// DBG_ASSERT(bp != NULL);
	// DBG_ASSERT(bp->b_data != NULL);
	// DBG_ASSERT(bp->b_bcount == sectorsize);

#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl)
		panic("hfs: standard hfs volumes should not be journaled!\n");
#endif

	mdb = (HFSMasterDirectoryBlock *)(bp->b_data +
					  HFS_PRI_OFFSET(sectorsize));

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

		altIDSector =
		    HFS_ALT_SECTOR(sectorsize, hfsmp->hfs_phys_block_count);

		if (meta_bread(hfsmp->hfs_devvp, altIDSector, sectorsize,
			       NOCRED, &alt_bp) == 0) {
			bcopy(mdb, alt_bp->b_data + HFS_ALT_OFFSET(sectorsize),
			      kMDBSize);

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
	// int critical = 0;

	if (vcb->vcbSigWord == kHFSSigWord)
		return hfs_flushMDB(hfsmp, waitfor, altflush);

	// if (altflush)
	// critical = 1;
	sectorsize = hfsmp->hfs_phys_block_size;
	priIDSector =
	    (vcb->hfsPlusIOPosOffset / sectorsize) + HFS_PRI_SECTOR(sectorsize);

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

	retval =
	    meta_bread(hfsmp->hfs_devvp, priIDSector, sectorsize, NOCRED, &bp);
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

	volumeHeader = (HFSPlusVolumeHeader *)((char *)bp->b_data +
					       HFS_PRI_OFFSET(sectorsize));

	/*
	 * For embedded HFS+ volumes, update create date if it changed
	 * (ie from a setattrlist call)
	 */
	if ((vcb->hfsPlusIOPosOffset != 0) &&
	    (SWAP_BE32(volumeHeader->createDate) != vcb->localCreateDate)) {
		struct buf *bp2;
		HFSMasterDirectoryBlock *mdb;

		retval =
		    meta_bread(hfsmp->hfs_devvp, HFS_PRI_SECTOR(sectorsize),
			       sectorsize, NOCRED, &bp2);
		if (retval) {
			if (bp2)
				brelse(bp2);
			retval = 0;
		} else {
			mdb = (HFSMasterDirectoryBlock *)(bp2->b_data +
							  HFS_PRI_OFFSET(
							      sectorsize));

			if (SWAP_BE32(mdb->drCrDate) != vcb->localCreateDate) {
#ifdef DARWIN_JOURNAL
				// XXXdbg
				if (hfsmp->jnl)
					journal_modify_block_start(hfsmp->jnl,
								   bp2);
#endif

				mdb->drCrDate = SWAP_BE32(
				    vcb->localCreateDate); /* pick up the new
							      create date */

#ifdef DARWIN_JOURNAL
				// XXXdbg
				if (hfsmp->jnl) {
					journal_modify_block_end(hfsmp->jnl,
								 bp2);
				} else
#endif
				{
					(void)VOP_BWRITE(
					    bp2); /* write out the changes */
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
	volumeHeader->attributes =
	    SWAP_BE32((SWAP_BE32(volumeHeader->attributes) & 0xFFFF0000) +
		      (UInt16)vcb->vcbAtrb);
	volumeHeader->journalInfoBlock = SWAP_BE32(vcb->vcbJinfoBlock);
#ifdef DARWIN_JOURNAL
	if (hfsmp->jnl) {
		volumeHeader->lastMountedVersion = SWAP_BE32(kHFSJMountVersion);
	} else
#endif
	{
		volumeHeader->lastMountedVersion =
		    SWAP_BE32(kHFSPlusMountVersion);
	}
	volumeHeader->createDate = SWAP_BE32(
	    vcb->localCreateDate); /* volume create date is in local time */
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

	if (bcmp(vcb->vcbFndrInfo, volumeHeader->finderInfo,
		 sizeof(volumeHeader->finderInfo)) != 0) {
		// critical = 1;
		bcopy(vcb->vcbFndrInfo, volumeHeader->finderInfo,
		      sizeof(volumeHeader->finderInfo));
	}

	/* Sync Extents over-flow file meta data */
	fp = VTOF(vcb->extentsRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		volumeHeader->extentsFile.extents[i].startBlock =
		    SWAP_BE32(fp->ff_extents[i].startBlock);
		volumeHeader->extentsFile.extents[i].blockCount =
		    SWAP_BE32(fp->ff_extents[i].blockCount);
	}
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	volumeHeader->extentsFile.logicalSize = SWAP_BE64(fp->ff_size);
	volumeHeader->extentsFile.totalBlocks = SWAP_BE32(fp->ff_blocks);
	volumeHeader->extentsFile.clumpSize = SWAP_BE32(fp->ff_clumpsize);

	/* Sync Catalog file meta data */
	fp = VTOF(vcb->catalogRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		volumeHeader->catalogFile.extents[i].startBlock =
		    SWAP_BE32(fp->ff_extents[i].startBlock);
		volumeHeader->catalogFile.extents[i].blockCount =
		    SWAP_BE32(fp->ff_extents[i].blockCount);
	}
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	volumeHeader->catalogFile.logicalSize = SWAP_BE64(fp->ff_size);
	volumeHeader->catalogFile.totalBlocks = SWAP_BE32(fp->ff_blocks);
	volumeHeader->catalogFile.clumpSize = SWAP_BE32(fp->ff_clumpsize);

	/* Sync Allocation file meta data */
	fp = VTOF(vcb->allocationsRefNum);
	for (i = 0; i < kHFSPlusExtentDensity; i++) {
		volumeHeader->allocationFile.extents[i].startBlock =
		    SWAP_BE32(fp->ff_extents[i].startBlock);
		volumeHeader->allocationFile.extents[i].blockCount =
		    SWAP_BE32(fp->ff_extents[i].blockCount);
	}
	FTOC(fp)->c_flag &= ~C_MODIFIED;
	volumeHeader->allocationFile.logicalSize = SWAP_BE64(fp->ff_size);
	volumeHeader->allocationFile.totalBlocks = SWAP_BE32(fp->ff_blocks);
	volumeHeader->allocationFile.clumpSize = SWAP_BE32(fp->ff_clumpsize);

	/* If requested, flush out the alternate volume header */
	if (altflush) {
		struct buf *alt_bp = NULL;
		u_long altIDSector;

		altIDSector =
		    (vcb->hfsPlusIOPosOffset / sectorsize) +
		    HFS_ALT_SECTOR(sectorsize, hfsmp->hfs_phys_block_count);

		if (meta_bread(hfsmp->hfs_devvp, altIDSector, sectorsize,
			       NOCRED, &alt_bp) == 0) {
#ifdef DARWIN_JOURNAL
			if (hfsmp->jnl)
				journal_modify_block_start(hfsmp->jnl, alt_bp);
#endif

			bcopy(volumeHeader,
			      alt_bp->b_data + HFS_ALT_OFFSET(sectorsize),
			      kMDBSize);

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
				(void)VOP_IOCTL(hfsmp->hfs_devvp,
						DKIOCSYNCHRONIZECACHE, NULL,
						FWRITE, NOCRED, current_proc());
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

//
//  Should the above two function be in utils instead? I guess so?
//  hfs_flushvolumeheaders
//  hfs_flushMDB
//

static struct vfsops hfs_vfsops = {
    .vfs_mount = hfs_mount,
    .vfs_cmount = hfs_cmount,
    .vfs_root = hfs_root,
    .vfs_statfs = hfs_statfs,
    .vfs_sync = hfs_sync,
    .vfs_unmount = hfs_unmount,
    .vfs_vget = hfs_vget,
};

VFS_SET(hfs_vfsops, hfs, 0);
