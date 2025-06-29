#!/bin/sh

hdiutil create -size 500m -fs HFS+ -layout None -volname vol_hfs cleanhfs

# hdiutil attach cleanhfs.dmg

MOUNT_POINT=$(hdiutil attach cleanhfs.dmg | grep "vol_hfs" | awk '{print $2}')

mkdir "$MOUNT_POINT/folder1"
mkdir "$MOUNT_POINT/folder2"
touch "$MOUNT_POINT/folder1/file1.txt"
touch "$MOUNT_POINT/fileout.txt"

echo "multi\nline\nfile here\non the outside" > "$MOUNT_POINT/fileout.txt"

echo "hello world" > "$MOUNT_POINT/folder2/file2.txt"

ln -s "$MOUNT_POINT/folder1/file1.txt" "$MOUNT_POINT/symlink_to_file"

ln -s "$MOUNT_POINT/folder2" "$MOUNT_POINT/symlink_to_folder"

ls -l "$MOUNT_POINT"

hdiutil detach "$MOUNT_POINT"
