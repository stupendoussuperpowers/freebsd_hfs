## Porting HFS+ to FreeBSD.

Glue code to get a filesystem working. Doesn't yet port features from XNU/HFS.

XNU/HFS code ported from - [](https://github.com/apple-oss-distributions/hfs)

Generates a dynamic kernel mod which registers the filesystem.

To run, load, or test mount -

`./runtest <build | load | test_mount>`

`./runtest` will run build, load, and then test_mount

Doesn't yet completely support mounts, however it will not kernel panic anymore :)

## Structure

Structure of the port is inspired by yars' port that was tried on FreeBSD 5. 

HFS+ related code: `hfsplus/`

Original XNU source code: `xnu/`

Stubs for macOSisms: `sys/` & `hfsplus/hfs_macos_defs.h`

## Changelog

[Changes made to port to FreeBSD 14](./updatelog)
