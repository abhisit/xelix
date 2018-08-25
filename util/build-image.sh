#!/bin/env bash
set -e

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <destname>"
    return 1
fi

dest=$1

sudo modprobe nbd max_part=8

if [ -f "$dest" ]; then
	sudo qemu-nbd --connect=/dev/nbd0 "$dest"
else
	qemu-img create -f qcow2 "$dest" 100M
	sudo qemu-nbd --connect=/dev/nbd0 "$dest"
	sudo mkfs.ext2 /dev/nbd0
fi

sudo mount /dev/nbd0 mnt

for i in land/*; do
	echo "Building $i"
	make -C "$i"
	sudo make -C "$i" install
done

sudo umount mnt
sudo qemu-nbd --disconnect /dev/nbd0
