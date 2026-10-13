#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$script_dir"

cpto . ../qemu.build
ninja -C ../qemu.build/build qemu-system-x86_64 qemu-img
sudo install -m 0755 ../qemu.build/build/qemu-system-x86_64 \
    /usr/local/bin/qemu-system-x86_64
sudo install -m 0755 ../qemu.build/build/qemu-img \
    /usr/local/bin/qemu-img
sudo install -D -m 0644 ../qemu.build/pc-bios/efi-virtio.rom \
    /usr/local/share/qemu/efi-virtio.rom
sudo install -D -m 0644 ../qemu.build/pc-bios/pxe-virtio.rom \
    /usr/local/share/qemu/pxe-virtio.rom
sudo install -D -m 0644 ../qemu.build/pc-bios/vgabios-virtio.bin \
    /usr/local/share/qemu/vgabios-virtio.bin
