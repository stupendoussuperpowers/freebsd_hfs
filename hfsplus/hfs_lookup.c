/*
 * Copyright (c) 1999-2002 Apple Computer, Inc. All rights reserved.
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
/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)hfs_lookup.c	1.0
 *	derived from @(#)ufs_lookup.c	8.15 (Berkeley) 6/16/95
 *
 *	(c) 1998-1999   Apple Computer, Inc.	 All Rights Reserved
 *	(c) 1990, 1992 	NeXT Computer, Inc.	All Rights Reserved
 *
 *
 *	hfs_lookup.c -- code to handle directory traversal on HFS/HFS+ volume
 */
#define LEGACY_FORK_NAMES 0

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/file.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/paths.h>
#include <sys/vnode.h>

#include "hfs.h"
#include "hfs_catalog.h"
#include "hfs_cnode.h"

static int forkcomponent(struct componentname *cnp, int *rsrcfork);

#define _PATH_DATAFORKSPEC "/..namedfork/data"

#ifdef LEGACY_FORK_NAMES
#define LEGACY_RSRCFORKSPEC "/rsrc"
#endif

/*
 * FROM FREEBSD 3.1
 * Convert a component of a pathname into a pointer to a locked cnode.
 * This is a very central and rather complicated routine.
 * If the file system is not maintained in a strict tree hierarchy,
 * this can result in a deadlock situation (see comments in code below).
 *
 * The cnp->cn_nameiop argument is LOOKUP, CREATE, RENAME, or DELETE depending
 * on whether the name is to be looked up, created, renamed, or deleted.
 * When CREATE, RENAME, or DELETE is specified, information usable in
 * creating, renaming, or deleting a directory entry may be calculated.
 * Notice that these are the only operations that can affect the directory of the target.
 *
 * If flag has LOCKPARENT or'ed into it and the target of the pathname
 * exists, lookup returns both the target and its parent directory locked.
 * When creating or renaming and LOCKPARENT is specified, the target may
 * not be ".".	When deleting and LOCKPARENT is specified, the target may
 * be "."., but the caller must check to ensure it does an vrele and vput
 * instead of two vputs.
 *
 * LOCKPARENT and WANTPARENT actually refer to the parent of the last item,
 * so if ISLASTCN is not set, they should be ignored. Also they are mutually exclusive, or
 * WANTPARENT really implies DONTLOCKPARENT. Either of them set means that the calling
 * routine wants to access the parent of the target, locked or unlocked.
 *
 * Keeping the parent locked as long as possible protects from other processes
 * looking up the same item, so it has to be locked until the cnode is totally finished
 *
 * This routine is actually used as VOP_CACHEDLOOKUP method, and the
 * filesystem employs the generic hfs_cache_lookup() as VOP_LOOKUP
 * method.
 *
 * hfs_cache_lookup() performs the following for us:
 *	check that it is a directory
 *	check accessibility of directory
 *	check for modification attempts on read-only mounts
 *	if name found in cache
 *		if at end of path and deleting or creating
 *		drop it
 *		 else
 *		return name.
 *	return VOP_CACHEDLOOKUP()
 *
 * Overall outline of hfs_lookup:
 *
 *	handle simple cases of . and ..
 *	search for name in directory, to found or notfound
 * notfound:
 *	if creating, return locked directory, leaving info on available slots
 *	else return error
 * found:
 *	if at end of path and deleting, return information to allow delete
 *	if at end of path and rewriting (RENAME and LOCKPARENT), lock target
 *	  cnode and return info to allow rewrite
 *	if not at end, add name to cache; if at end and neither creating
 *	  nor deleting, add name to cache
 */

/*
 *	Lookup *nm in directory *pvp, return it in *a_vpp.
 *	**a_vpp is held on exit.
 *	We create a cnode for the file, but we do NOT open the file here.

#% lookup	dvp L ? ?
#% lookup	vpp - L -

	IN struct vnode *dvp - Parent node of file;
	INOUT struct vnode **vpp - node of target file, its a new node if
		the target vnode did not exist;
	IN struct componentname *cnp - Name of file;

 *	When should we lock parent_hp in here ??
 */

int
hfs_lookup(struct vop_lookup_args *ap)
{
	/* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
	} */

	struct vnode *dvp = ap->a_dvp; /* vnode for directory being searched */
	struct cnode *dcp = VTOC(dvp); /* cnode for directory being searched */
	struct vnode **vpp = ap->a_vpp;
	struct vnode *tvp = NULL; /* target vnode */
	struct hfsmount *hfsmp = VTOHFS(dvp);
	struct componentname *cnp = ap->a_cnp;
	struct ucred *cred = cnp->cn_cred;
	proc_t *p = curthread;
	int wantrsrc = 0;
	int forknamelen = 0;
	int flags = cnp->cn_flags;
	int wantparent = flags & (LOCKPARENT | WANTPARENT);
	int nameiop = cnp->cn_nameiop;
	int retval = 0;
	int isDot = FALSE;
	struct cat_desc desc = { 0 };
	struct cat_desc cndesc;
	struct cat_attr attr;
	struct cat_fork fork;

	*vpp = NULL;

	/*
	 * First check to see if it is a . or .., else look it up.
	 */
	if (flags & ISDOTDOT) { /* Wanting the parent */
		goto found;	/* .. is always defined */
	} else if ((cnp->cn_nameptr[0] == '.') && (cnp->cn_namelen == 1)) {
		isDot = TRUE;
		goto found; /* We always know who we are */
	} else {
		/* Check fork suffix to see if we want the resource fork */
		forknamelen = forkcomponent(cnp, &wantrsrc);

		/* No need to go to catalog if there are no children */
		if (dcp->c_entries == 0)
			goto notfound;

		bzero(&cndesc, sizeof(cndesc));
		cndesc.cd_nameptr = cnp->cn_nameptr;
		cndesc.cd_namelen = cnp->cn_namelen;
		cndesc.cd_parentcnid = dcp->c_cnid;
		cndesc.cd_hint = dcp->c_childhint;

		/* Lock catalog b-tree */
		retval = hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_SHARED, p);
		if (retval) {
			goto exit;
		}

		retval = cat_lookup(hfsmp, &cndesc, wantrsrc, &desc, &attr, &fork);

		if (retval == 0 && S_ISREG(attr.ca_mode) && attr.ca_blocks < fork.cf_blocks)
			panic("hfs_lookup: bad ca_blocks (too small)");

		/* Unlock catalog b-tree */
		(void)hfs_metafilelocking(hfsmp, kHFSCatalogFileID, LK_RELEASE, p);
		if (retval == 0) {
			dcp->c_childhint = desc.cd_hint;
			goto found;
		}
	notfound:
		/*
		 * This is a non-existing entry
		 *
		 * If creating, and at end of pathname and current
		 * directory has not been removed, then can consider
		 * allowing file to be created.
		 */
		if ((nameiop == CREATE || nameiop == RENAME ||
			(nameiop == DELETE && (ap->a_cnp->cn_flags & DOWHITEOUT) && (ap->a_cnp->cn_flags & ISWHITEOUT))) &&
		    (flags & ISLASTCN)) {
			/*
			 * Access for write is interpreted as allowing
			 * creation of files in the directory.
			 */
			retval = VOP_ACCESS(dvp, VWRITE, cred, p);
			if (retval) {
				goto exit;
			}

			cnp->cn_flags |= RENAME;
			retval = EJUSTRETURN;
			goto exit;
		}

		/*
		 * Insert name into cache (as non-existent) if appropriate.
		 *
		 * Disable negative caching since HFS is case-insensitive.
		 */
#if 0
		if ((cnp->cn_flags & MAKEENTRY) && nameiop != CREATE)
			cache_enter(dvp, *vpp, cnp);
#endif
		retval = ENOENT;
		goto exit;
	}

found:
	/*
	 * Process any fork specifiers
	 */
	if (forknamelen && S_ISREG(attr.ca_mode)) {
		/* fork names are only for lookups */
		if ((nameiop != LOOKUP) && (nameiop != CREATE)) {
			retval = EPERM;
			goto exit;
		}
		flags |= ISLASTCN;
		cnp->cn_flags |= ISLASTCN; /* tell lookup(9) dvp will be locked */
	} else {
		wantrsrc = 0;
		forknamelen = 0;
	}

	/*
	 * If deleting, and at end of pathname, return
	 * parameters which can be used to remove file.
	 */
	if (nameiop == DELETE && (flags & ISLASTCN)) {
		/*
		 * Write access to directory required to delete files.
		 */
		if ((retval = VOP_ACCESS(dvp, VWRITE, cred, p)))
			goto exit;

		if (isDot) { /* Want to return ourselves */
			VREF(dvp);
			*vpp = dvp;
			goto exit;
		} else if (flags & ISDOTDOT) {
			retval = hfs_getcnode(hfsmp, dcp->c_parentcnid, NULL, 0, NULL, NULL, &tvp);
			if (retval)
				goto exit;
		} else {
			retval = hfs_getcnode(hfsmp, attr.ca_fileid, &desc, wantrsrc, &attr, &fork, &tvp);
			if (retval)
				goto exit;
		}

		/*
		 * If directory is "sticky", then user must own
		 * the directory, or the file in it, else she
		 * may not delete it (unless she's root). This
		 * implements append-only directories.
		 */
		if ((dcp->c_mode & S_ISTXT) && (cred->cr_uid != 0) && (cred->cr_uid != dcp->c_uid) && (tvp->v_type != VLNK) &&
		    (hfs_owner_rights(hfsmp, VTOC(tvp)->c_uid, cred, false))) {
			vput(tvp);
			retval = EPERM;
			goto exit;
		}

		/*
		 * If this is a link node then we need to save the name
		 * (of the link) so we can delete it from the catalog b-tree.
		 * In this case, hfs_remove will then free the component name.
		 *
		 * DJB - IS THIS STILL NEEDED????
		 */
		if (tvp && (VTOC(tvp)->c_flag & C_HARDLINK))
			cnp->cn_flags |= RENAME;

		*vpp = tvp;
		goto exit;
	}

	/*
	 * If renaming, return the cnode and save the current name.
	 */
	if (nameiop == RENAME && wantparent && (flags & ISLASTCN)) {
		if ((retval = VOP_ACCESS(dvp, VWRITE, cred, p)) != 0)
			goto exit;
		/*
		 * Careful about locking second cnode.
		 */
		if (isDot) {
			retval = EISDIR;
			goto exit;
		} else if (flags & ISDOTDOT) {
			retval = hfs_getcnode(hfsmp, dcp->c_parentcnid, NULL, 0, NULL, NULL, &tvp);
			if (retval)
				goto exit;
		} else {
			retval = hfs_getcnode(hfsmp, attr.ca_fileid, &desc, wantrsrc, &attr, &fork, &tvp);
			if (retval)
				goto exit;
		}
		cnp->cn_flags |= RENAME;
		*vpp = tvp;
		goto exit;
	}

	/*
	 * We must get the target cnode before unlocking
	 * the directory to insure that the cnode will not be removed
	 * before we get it.  We prevent deadlock by always fetching
	 * cnodes from the root, moving down the directory tree. Thus
	 * when following backward pointers ".." we must unlock the
	 * parent directory before getting the requested directory.
	 * There is a potential race condition here if both the current
	 * and parent directories are removed before the VFS_VGET for the
	 * cnode associated with ".." returns.  We hope that this occurs
	 * infrequently since we cannot avoid this race condition without
	 * implementing a sophisticated deadlock detection algorithm.
	 */
	if (flags & ISDOTDOT) {
		//	VOP_UNLOCK(dvp);	/* race to get the cnode */
		retval = hfs_getcnode(hfsmp, dcp->c_parentcnid, NULL, 0, NULL, NULL, &tvp);
		if (retval) {
			//		vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY);
			goto exit;
		}
		if ((flags & LOCKPARENT) && (flags & ISLASTCN) && (dvp != tvp) /*&& 
		    (retval = vn_lock(dvp, LK_EXCLUSIVE))*/) {
			vput(tvp);
			goto exit;
		}
		*vpp = tvp;
	} else if (isDot) {
		vref(dvp); /* we want ourself, ie "." */
		*vpp = dvp;
	} else {
		int type = (attr.ca_mode & S_IFMT);

		if (!(flags & ISLASTCN) && type != S_IFDIR && type != S_IFLNK) {
			retval = ENOTDIR;
			goto exit;
		}

		retval = hfs_getcnode(hfsmp, attr.ca_fileid, &desc, wantrsrc, &attr, &fork, &tvp);
		if (retval)
			goto exit;

		*vpp = tvp;
	}

	/*
	 * Insert name in cache if appropriate.
	 *  - "." and ".." are not cached.
	 *  - Resource fork names are not cached.
	 *  - Names with composed chars are not cached.
	 */
	if ((cnp->cn_flags & MAKEENTRY) && !isDot && !(flags & ISDOTDOT) && !wantrsrc && (cnp->cn_namelen == VTOC(*vpp)->c_desc.cd_namelen)) {
		cache_enter(dvp, *vpp, cnp);
	}

exit:
	cat_releasedesc(&desc);
	return (retval);
}

/*
 * Based on vn_cache_lookup (which is vfs_cache_lookup in FreeBSD 3.1)
 *
 * Name caching works as follows:
 *
 * Names found by directory scans are retained in a cache
 * for future reference.  It is managed LRU, so frequently
 * used names will hang around.	 Cache is indexed by hash value
 * obtained from (vp, name) where vp refers to the directory
 * containing name.
 *
 * If it is a "negative" entry, (i.e. for a name that is known NOT to
 * exist) the vnode pointer will be NULL.
 *
 * Upon reaching the last segment of a path, if the reference
 * is for DELETE, or NOCACHE is set (rewrite), and the
 * name is located in the cache, it will be dropped.
 *
 */

int
hfs_cachedlookup(struct vop_cachedlookup_args *ap)
{
	/* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
	} */
	printf("[enter] hfs_cachedlookup\n");
	return (45);
}

/*
 * forkcomponent - look for a fork suffix in the component name
 *
 */
static int
forkcomponent(struct componentname *cnp, int *rsrcfork)
{
	char *suffix = cnp->cn_nameptr + cnp->cn_namelen;
	int consume = 0;

	*rsrcfork = 0;
	if (*suffix == '\0')
		return (0);
	/*
	 * There are only 3 valid fork suffixes:
	 *	"/..namedfork/rsrc"
	 *	"/..namedfork/data"
	 *	"/rsrc"  (legacy)
	 */
	if (bcmp(suffix, _PATH_RSRCFORKSPEC, sizeof(_PATH_RSRCFORKSPEC)) == 0) {
		consume = sizeof(_PATH_RSRCFORKSPEC) - 1;
		*rsrcfork = 1;
	} else if (bcmp(suffix, _PATH_DATAFORKSPEC, sizeof(_PATH_DATAFORKSPEC)) == 0) {
		consume = sizeof(_PATH_DATAFORKSPEC) - 1;
	}
#ifdef LEGACY_FORK_NAMES
	else if (bcmp(suffix, LEGACY_RSRCFORKSPEC, sizeof(LEGACY_RSRCFORKSPEC)) == 0) {
		consume = sizeof(LEGACY_RSRCFORKSPEC) - 1;
		*rsrcfork = 1;
	}
#endif
	return (consume);
}
