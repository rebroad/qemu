# Goal: maintain host integration for the guarded Windows VM

## Host runtime files and libvirt configuration

Persistent libvirt domain XML files, including `/etc/libvirt/qemu/win11-phys.xml`, must remain owned by `root:libvirt` with mode `0640` after every libvirt or Virt-manager rewrite. Do not assume the containing directory's SGID bit or libvirt umask controls this: libvirt may atomically replace XML using explicit ownership and mode. Find the narrowest supported way to preserve the required owner, group, and mode, then verify after both `virsh` and Virt-manager updates. Any post-write repair must be limited to the intended domain XML and must not weaken access to other libvirt configuration.

Keep a separate progress record for dated host audits, build/test results, installation state, and other transient findings. Before removing any legacy helper, search live system configuration and user scripts for references; do not remove shared package or application helpers based only on this VM's inventory.

| Installed path | Purpose | Source or caller |
| --- | --- | --- |
| `/usr/local/libexec/qemu-cow-manager` | Privileged entry point for Virt-manager COW save/discard controls. | `scripts/qemu-cow-manager`; called by Virt-manager with `pkexec`. |
| `/usr/local/libexec/qemu-guarded-cow` | Validates configured partitions, isolates host mounts, and initializes, saves, or discards the guarded overlay. | `scripts/qemu-guarded-cow`; called by the libvirt hook and `qemu-cow-manager`. |
| `/etc/libvirt/hooks/qemu` | Performs guarded-COW preparation, verification, and host-state restoration on domain lifecycle events. | Installed from `scripts/qemu-guarded-cow-libvirt-hook`; not installed under `/usr/local/libexec`. |
| `/usr/local/bin/qemu-system-x86_64` | Custom QEMU executable selected by the `win11-phys` domain XML, including `partition-write-guard`. | Built from this repository and installed by `scripts/build-install-qemu.sh`. |
| `/usr/local/bin/qemu-img` | Companion image utility used to initialize and merge overlays through the partition guard. | Built from this repository and installed by `scripts/build-install-qemu.sh`. |

`/usr/local/libexec/qemu-bridge-helper` is a separate symlink to the QEMU 9.2.50 bridge helper. The current VM uses a libvirt-managed tap on `br-winproxy`, so this integration does not call it; other local QEMU domains may. Do not remove it based only on this VM-specific inventory.
