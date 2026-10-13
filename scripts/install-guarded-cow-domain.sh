#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $# != 1 ]]; then
    echo "Usage: $0 DOMAIN.xml" >&2
    exit 2
fi
domain_xml=$1
domain=$(python3 - "$domain_xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

print(ET.parse(sys.argv[1]).getroot().findtext("name", ""))
PY
)
if [[ ! $domain =~ ^[A-Za-z0-9_.-]+$ ]]; then
    echo "Invalid or missing domain name in $domain_xml" >&2
    exit 2
fi

sudo install -D -m 0755 "$script_dir/qemu-guarded-cow" \
    /usr/local/libexec/qemu-guarded-cow
sudo install -D -m 0755 "$script_dir/qemu-cow-manager" \
    /usr/local/libexec/qemu-cow-manager
sudo install -D -m 0755 "$script_dir/qemu-guarded-cow-libvirt-hook" \
    /etc/libvirt/hooks/qemu
sudo virsh define --validate "$domain_xml"
sudo /usr/local/libexec/qemu-guarded-cow initialize "$domain"
