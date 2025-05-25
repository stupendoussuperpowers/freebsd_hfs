#!/bin/sh

if [ -n "$1" ]; then
  BSD_RAW="$1"
else
  BSD_RAW='FreeBSD-14.2-RELEASE-arm64-aarch64.raw'
fi

qemu-system-aarch64 \
  -M virt \
  -accel hvf \
  -cpu host \
  -smp 4 \
  -m 4096 \
  -bios QEMU_EFI.fd \
  -device virtio-gpu-pci \
  -display default,show-cursor=on \
  -device qemu-xhci \
  -device usb-kbd \
  -device usb-tablet \
  -device intel-hda \
  -device hda-duplex \
  -drive file="$BSD_RAW",format=raw,if=virtio,cache=writethrough \
  -nic vmnet-shared \
  -nographic \
  -serial mon:stdio
