#!/usr/sbin/dtrace -s

#pragma D option quiet

fbt::hfs_*:entry
{
	printf("[HFS] %s\n", probefunc);
}

fbt::vfs_*:entry,
fbt::vop_*:entry
{
	printf("[VFS] %s\n", probefunc);
}

syscall::mount:entry
{
	printf("\n--- mount() called ---\n");
}
