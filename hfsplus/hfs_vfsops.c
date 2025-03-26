#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/malloc.h>


static MALLOC_DEFINE(M_HFSMNT, "HFS mount", "HFS mount data");

static int hfs_mount(struct mount *mp) {
    printf("---hfs_mount---\n");

    if(mp->mnt_data == NULL) {
        mp->mnt_data = malloc(100, M_TEMP, M_WAITOK);
    }

    vfs_getnewfsid(mp);
    return (0);
}

static int hfs_root(struct mount *mp, int flags, struct vnode**vpp) {
    printf("---hfs_root---\n");

    struct vnode *nvp;
    int retval;


    if((retval = VFS_VGET(mp, rootObjId, LK_EXCLUSIVE, &nvp))) {
        printf("VFS_VGET failed: %d\n", retval);
        return (retval);
    }

    printf("setting vpp to nvp\n");
    
    *vpp = nvp;

    printf("---");

    return (0);
}

static int hfs_statfs(struct mount *mp, struct statfs *sbp) {
    printf("---hfs_statfs---\n");
    sbp->f_bsize = 4096;
    sbp->f_blocks = 1000;
    sbp->f_bfree = 500;
    sbp->f_bavail = 500;
    sbp->f_files = 100;
    sbp->f_ffree = 50;

    return (0);
}

static int hfs_sync(struct mount *mp, int waitfor) {
    printf("---hfs_sync---\n");

    return (0);
}

static int hfs_unmount(struct mount *mp, int mntflags) {
    printf("---hfs_unmount---\n");

    free(mp->mnt_data, M_HFSMNT);
    mp->mnt_data = NULL;

    return (0);
}

static int hfs_vget(struct mount *mp, ino_t inode, int flags, struct vnode **vpp) {
    printf("---hfs_vget---\n");
    return 45;
}

static struct vfsops hfs_vfsops = {
	.vfs_mount =		hfs_mount,
    
    .vfs_root  =    hfs_root, 
    .vfs_statfs = hfs_statfs, 
    .vfs_sync = hfs_sync, 
    .vfs_unmount = hfs_unmount, 
    .vfs_vget = hfs_vget,
};

VFS_SET(hfs_vfsops, hfs, 0);