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
manifest_tmp=$(mktemp /var/tmp/qemu-guarded-cow-source.XXXXXX)
trap 'rm -f -- "$manifest_tmp"' EXIT
python3 - "$script_dir/qemu-guarded-cow" >"$manifest_tmp" <<'PY'
import hashlib
import json
import os
import sys

source = os.path.realpath(sys.argv[1])
with open(source, "rb") as stream:
    digest = hashlib.sha256(stream.read()).hexdigest()
print(json.dumps({"schema": "qemu-guarded-cow-source-v1",
                  "source": source, "sha256": digest},
                 sort_keys=True, separators=(",", ":")))
PY
sudo install -D -o root -g root -m 0644 "$manifest_tmp" \
    /usr/local/libexec/qemu-guarded-cow.source.json
sudo install -D -m 0755 "$script_dir/qemu-cow-manager" \
    /usr/local/libexec/qemu-cow-manager
sudo install -D -m 0755 "$script_dir/qemu-guarded-cow-libvirt-hook" \
    /etc/libvirt/hooks/qemu
sudo install -D -m 0644 "$script_dir/qemu-guarded-cow-recovery-check.service" \
    /etc/systemd/system/qemu-guarded-cow-recovery-check.service
sudo systemctl daemon-reload
sudo systemctl enable qemu-guarded-cow-recovery-check.service
sudo virsh define --validate "$domain_xml"
sudo /usr/local/libexec/qemu-guarded-cow initialize "$domain"
