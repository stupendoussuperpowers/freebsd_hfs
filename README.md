## Porting HFS+ to FreeBSD.

Status: Ported APIs to modern FreeBSD. Getting Mounting to work

XNU/HFS code ported from - [](https://github.com/apple-oss-distributions/hfs)

Generates a dynamic kernel mod which registers the filesystem.

To run, load, or test mount -

`./build`

`./load`

`./testmount` -- Requires an HFS/HFS+ image to be present at /dev/md10

## Structure

Structure of the port is inspired by yars' port that was tried on FreeBSD 5. 

HFS+ related code: `hfsplus/`

Original XNU source code: `xnu/`

Stubs for macOSisms: `sys/` & `hfsplus/hfs_macos_defs.h`, `vfs/`

Binaries: `disk_bin/`

## Changelog

[Changes made to port to FreeBSD 14](./updatelog)
