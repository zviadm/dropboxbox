#!/bin/bash

set -e

DBBOX_IMG=/tmp/dbbox_img
DBBOX=/tmp/dbbox

sudo umount -f $DBBOX || true
sudo umount -f $DBBOX_IMG || true

sudo mkdir -p $DBBOX
sudo mkdir -p $DBBOX_IMG

sudo ./dbbox $DBBOX_IMG -d &
FUSE_PID=$!

sleep 5
sudo mount $DBBOX_IMG/dbbox.img $DBBOX -t vfat -o loop,ro,noexec || sudo kill $FUSE_PID
wait $FUSE_PID
