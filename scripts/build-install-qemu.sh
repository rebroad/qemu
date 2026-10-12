#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$script_dir"

cpto . ../qemu.build
ninja -C ../qemu.build/build qemu-system-x86_64
sudo install -m 0755 ../qemu.build/build/qemu-system-x86_64 \
    /usr/local/bin/qemu-system-x86_64
