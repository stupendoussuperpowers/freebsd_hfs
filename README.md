## Porting HFS+ to FreeBSD.

Glue code to get a filesystem working. Doesn't yet port features from XNU/HFS.

Generates a dynamic kernel mod which registers the filesystem.

To run, load, or test mount -

`./runtest <build | load | test_mount>`

`./runtest` will run build, load, and then test_mount

Doesn't yet completely support mounts, however it will not kernel panic anymore :)
