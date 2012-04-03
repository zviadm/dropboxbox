#!/bin/bash

set -e

# Make sure only root can run our script
if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root" 1>&2
    exit 1
fi

DBBOX_IMG=/tmp/dbbox_img
DBBOX=/tmp/dbbox

umount -f $DBBOX || true
umount -f $DBBOX_IMG || true

mkdir -p $DBBOX
mkdir -p $DBBOX_IMG

./dbbox $DBBOX_IMG -d &
FUSE_PID=$!
echo "DBBOX FUSE running, pid: $FUSE_PID"
trap "kill $FUSE_PID" EXIT

sleep 5
mount $DBBOX_IMG/dbbox.img $DBBOX -t vfat -o loop,ro,noexec
wait $FUSE_PID
