## Porting HFS+ to FreeBSD.

### Status

#### Features
- [x] Mounting (RO)
- [x] Unmounting
- [ ] Read support for directories, files
- [ ] Read support for attributes
- [ ] Write support for directories, files
- [ ] Write support for attributes
- [ ] Journalling support
#### Internal
- [x] Port to modern FreeBSD VFS APIs (vop/vfs vectors, VOP_* functions)
- [ ] Build/port tests
- [ ] Native implementation/port of `hfscommon/` code
- [ ] Remove dependence on macOS stubs and type aliases
#### Userland Binaries (Rust)
- [x] mount_hfs
- [ ] newfs_hfs
- [ ] fsck_hfs

### Structure
```
freebsd_hfs/
├── MAKEFFILE               # Build rules for kernel module or tools
├── hfsplus/                # HFS+ filesystem logic
│   └── hfscommon/          # Util layer for BTree support
├── xnu/                    # Apple source code for HFS
├── vfs/                    # utf8 support for xnu/hfs
├── sys/                    # Compatibility support
├── utils/                  # Misc. util scripts
├── disk_bin/               # Rust binaries for mount_hfs, newfs_fs, fsck_hfs
│
├── build                   # Trigger fresh build 
├── load                    # kldload
├── testmount               # mount /dev/md10 to /hello as HFS
├── unload                  # kldunload
└── diving                  # kgdb into /var/crash/vmcore.last
```

XNU/HFS code ported from - [hfs](https://github.com/apple-oss-distributions/hfs)

The `MAKEFILE` is configured to generate a kernel module which registers the HFS+ filesystem.

This port builds on the work previously done on FreeBSD 5 by yars which was a direct port of Apple's implementation. As such, there are plenty of compatibility layers built into the code, such as aliased types and shims headers in `sys/` and `vfs/`. These features need to natively ported to remove dependence on such compatibility layers.

Most of the logic for managing BTrees is borrowed unchanged from the original implementation, the code for which is located at `hfsplus/hfscommon/` 

Core logic for HFS+ implementation is located at `hfsplus/`. The folder structure is in line with that of existing filesystems on FreeBSD. 

This repository also has several QOL scripts for frequently used actions such as building and loading the kernel module, mounting the filesystem, and using kgdb to inspect crash dumps.


### Changelog

[Changes made to port to FreeBSD 14](./updatelog)

### Contact

ss19723 [at] nyu [dot] edu on FreeBSD mailing lists.
