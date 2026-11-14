#!/bin/sh
set -e

IMG="$HOME/izhos-rework/Downloads/test.img"
ISO="$HOME/izhos-rework/Downloads/IzhOS.iso"

qemu-img create -f raw "$IMG" 1.5G

sgdisk -o "$IMG"
sgdisk -n 1:0:+100M -t 1:ef00 -c 1:"EFI System" "$IMG"
sgdisk -n 2:0:0 -t 2:0700 -c 2:"IZHOS" "$IMG"
sgdisk -p "$IMG"

qemu-system-x86_64 -m 256M \
  -bios /usr/share/edk2/ovmf/OVMF_CODE.fd \
  -cdrom "$ISO" -boot d \
  -usb -device usb-kbd \
  -drive file="$IMG",format=raw,if=virtio
