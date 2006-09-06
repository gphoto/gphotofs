#!/bin/sh

set -ex

mountpoint=${mountpoint-test-mountpoint}
testdir="${testdir-.}"
camera="Mass Storage Camera"

# FIXME: 
# If libgphoto2 is not installed yet,
# - IOLIBS must be pointed to where the "disk" iolib is.
# - CAMLIBS must be pointed to where the "directory" camlib is.

trap "umount ${mountpoint}; rm "$testdir"/*; rmdir ${mountpoint} $testdir" EXIT

if test -d "$mountpoint"; then rmdir "$mountpoint"; fi
if test -d "$testdir"; then rmdir "$testdir"; fi
mkdir "${mountpoint}" "$testdir"
cp "$srcdir/gphotofs.c" "$testdir/"

"./gphotofs" --port "disk:$(cd "$testdir" > /dev/null && pwd)" --camera "$camera" "${mountpoint}"

sleep 2
ls -al "${mountpoint}"

umount "${mountpoint}"

rm -f "$testdir"/*
rmdir "${mountpoint}" "$testdir"
