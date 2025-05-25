The following general API changes have been accomodated:

VOP_LOCK    -   No thread arg 
VOP_UNLOCK  -   No thread arg, no flags
MALLOC (a, b, c, d, e) -> a = (b) MALLOC(c, d, e)
FREE -> free
VOP_FSYNC

stuct buf's b_ops changes to buf->b_bufobj->buf_ops

__private_extern__ need to be 'static'

function definitions for vfs_ops functions, (e.g. hfs_mount(struct mount*) )
