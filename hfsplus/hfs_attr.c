#include <sys/types.h>
#include <sys/param.h>

#include <sys/systm.h>
#include <sys/attr.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <hfsplus/hfs.h>
#include <hfsplus/hfs_cnode.h>
#include <hfsplus/hfs_dbg.h>
#include <hfsplus/hfs_mount.h>


int hfs_write_access(struct vnode*, struct ucred*, Boolean);

static int
hfs_chown(struct vnode *vp, uid_t uid, gid_t gid, 
	  struct ucred *cred, proc_t *p)
{
	struct cnode *cp = VTOC(vp);
	uid_t ouid;
	gid_t ogid;
	int error = 0;
//#if QUOTA
//	register int i;
//	int64_t change;
//#endif /* QUOTA */

	if (VTOVCB(vp)->vcbSigWord != kHFSPlusSigWord)
		return (EOPNOTSUPP);

	if (VTOVFS(vp)->mnt_flag & MNT_UNKNOWNPERMISSIONS)
		return (0);
	
	if (uid == (uid_t)VNOVAL)
		uid = cp->c_uid;
	if (gid == (gid_t)VNOVAL)
		gid = cp->c_gid;
	/*
	 * If we don't own the file, are trying to change the owner
	 * of the file, or are not a member of the target group,
	 * the caller must be superuser or the call fails.
	 */
	if ((cred->cr_uid != cp->c_uid || uid != cp->c_uid ||
	    (gid != cp->c_gid && !groupmember((gid_t)gid, cred))) &&
	    /* (error = suser_cred(cred, SUSER_ALLOWJAIL)) */ 
	    (error = priv_check_cred(cred, PRIV_VFS_ADMIN)))
		return (error);

	ogid = cp->c_gid;
	ouid = cp->c_uid;

	//
	// TODO: Do QUOTA stuff here
	//

	cp->c_gid = gid;
	cp->c_uid = uid;
	
	//
	// TODO: Do QUOTA stuff here
	//


	if (ouid != uid || ogid != gid)
		cp->c_flag |= C_CHANGE;
	if (ouid != uid && cred->cr_uid != 0)
		cp->c_mode &= ~S_ISUID;
	if (ogid != gid && cred->cr_uid != 0)
		cp->c_mode &= ~S_ISGID;
	return (0);
}

static int
hfs_chflags(struct vnode *vp, u_long flags, struct ucred *cred, proc_t *p)
{
	struct cnode *cp = VTOC(vp);
	int retval;

	if (VTOVCB(vp)->vcbSigWord == kHFSSigWord) {
		if ((retval = hfs_write_access(vp, cred, false)) != 0) {
			return retval;
		};
	} else if ((retval = hfs_owner_rights(VTOHFS(vp), cp->c_uid, cred, true)) != 0) {
		return retval;
	};

	if (cred->cr_uid == 0) {
		if ((cp->c_xflags & (SF_IMMUTABLE | SF_APPEND)) &&
			securelevel_gt(cred, 0)) {
			return EPERM;
		};
		cp->c_xflags = flags;
	} else {
		if (cp->c_xflags & (SF_IMMUTABLE | SF_APPEND) ||
			(flags & UF_SETTABLE) != flags) {
			return EPERM;
		};
		cp->c_xflags &= SF_SETTABLE;
		cp->c_xflags |= (flags & UF_SETTABLE);
	}
	cp->c_flag |= C_CHANGE;

	return (0);
}


static int
hfs_chmod(struct vnode *vp, int mode, struct ucred *cred, proc_t *p)
{
	struct cnode *cp = VTOC(vp);
	int error;

	if (VTOVCB(vp)->vcbSigWord != kHFSPlusSigWord)
		return (0);

#ifdef DARWIN_JOURNAL
	// XXXdbg - don't allow modification of the journal or journal_info_block
	if (VTOHFS(vp)->jnl && cp && cp->c_datafork) {
		struct HFSPlusExtentDescriptor *extd;

		extd = &cp->c_datafork->ff_data.cf_extents[0];
		if (extd->startBlock == VTOVCB(vp)->vcbJinfoBlock || extd->startBlock == VTOHFS(vp)->jnl_start) {
			return EPERM;
		}
	}
#endif /* DARWIN_JOURNAL */

#if OVERRIDE_UNKNOWN_PERMISSIONS
	if (VTOVFS(vp)->mnt_flag & MNT_UNKNOWNPERMISSIONS) {
		return (0);
	};
#endif
	if ((error = hfs_owner_rights(VTOHFS(vp), cp->c_uid, cred, true)) != 0)
		return (error);
	if (cred->cr_uid) {
		if (vp->v_type != VDIR && (mode & S_ISTXT))
			return (EFTYPE);
		if (!groupmember(cp->c_gid, cred) && (mode & S_ISGID))
			return (EPERM);
	}
	cp->c_mode &= ~ALLPERMS;
	cp->c_mode |= (mode & ALLPERMS);
	cp->c_flag |= C_CHANGE;
	return (0);
}

int
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
	vap->va_uid = (cp->c_uid == UNKNOWNUID) ? 0 : cp->c_uid;
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

int 
hfs_setattr(struct vop_setattr_args *ap)
{
	/* {
		struct vnode *a_vp;
		struct vattr *a_vap;
		struct ucred *a_cred;
	} */

	
	struct vattr *vap = ap->a_vap;
	struct vnode *vp = ap->a_vp;
	struct cnode *cp = VTOC(vp);
	struct ucred *cred = ap->a_cred;
	proc_t *p = curthread;
	struct timeval atimeval, mtimeval;
	int error;

	/*
	 * Check for unsettable attributes.
	 */
	if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
	    (vap->va_blocksize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
	    ((int)vap->va_bytes != VNOVAL) || (vap->va_gen != VNOVAL)) {
		return (EINVAL);
	}

	if (vap->va_flags != VNOVAL) {
		if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = hfs_chflags(vp, vap->va_flags, cred, p)))
			return (error);
		if (vap->va_flags & (IMMUTABLE | APPEND))
			return (0);
	}

	if (cp->c_xflags & (IMMUTABLE | APPEND))
		return (EPERM);

	//
	// TODO: Journaling stuff goes here
	//
	
	/*
	 * Go through the fields and update iff not VNOVAL.
	 */
	if (vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL) {
		if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = hfs_chown(vp, vap->va_uid, vap->va_gid, cred, p)))
			return (error);
	}
	if (vap->va_size != VNOVAL) {
		/*
		 * Disallow write attempts on read-only file systems;
		 * unless the file is a socket, fifo, or a block or
		 * character device resident on the file system.
		 */
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
 		case VLNK:
		case VREG:
			if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
				return (EROFS);
                	break;
		default:
                	break;
		}
		if ((error = hfs_truncate(vp, vap->va_size, 0, cred, p)))
			return (error);
	}

	cp = VTOC(vp);
	
	if (vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL) {
		if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if (((error = hfs_owner_rights(VTOHFS(vp), cp->c_uid, cred, true)) != 0) &&
		    ((vap->va_vaflags & VA_UTIMES_NULL) == 0 ||
		    (error = VOP_ACCESS(vp, VWRITE, cred, p)))) {
			return (error);
		}
		if (vap->va_atime.tv_sec != VNOVAL)
			cp->c_flag |= C_ACCESS;
		if (vap->va_mtime.tv_sec != VNOVAL) {
			cp->c_flag |= C_CHANGE | C_UPDATE;
			/*
			 * The utimes system call can reset the modification
			 * time but it doesn't know about HFS create times.
			 * So we need to insure that the creation time is
			 * always at least as old as the modification time.
			 */
			if ((VTOVCB(vp)->vcbSigWord == kHFSPlusSigWord) &&
			    (cp->c_cnid != kRootDirID) &&
			    (vap->va_mtime.tv_sec < cp->c_itime)) {
				cp->c_itime = vap->va_mtime.tv_sec;
			}
		}
		atimeval.tv_sec = vap->va_atime.tv_sec;
		atimeval.tv_usec = 0;
		mtimeval.tv_sec = vap->va_mtime.tv_sec;
		mtimeval.tv_usec = 0;
		if ((error = hfs_update(vp, &atimeval, &mtimeval, 1)))
			return (error);
	}
	error = 0;
	if (vap->va_mode != (mode_t)VNOVAL) {
		if (VTOVFS(vp)->mnt_flag & MNT_RDONLY)
			return (EROFS);
		error = hfs_chmod(vp, (int)vap->va_mode, cred, p);
	}
	return (error);

}


