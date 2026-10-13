ok, so now we need to make the Windows 11 VM faster and more responsive. Can we minimize the amount of emulation it does? Also, can we set up with COW thing where it gives us the option to save the changes after powering down the VM? Is this feature GUI enabled via virt-manager? If not, we should make it so.

The COW mode must be guarded: the guest may write only to GPT partitions 1, 3, and 7. Writes to every other partition and to disk areas outside those partitions must be rejected while running in the overlay, and changes must remain subject to the same allowlist when saved back to the physical disk.

Change the "Save/Discard Cow changes" so that we can instead have snapshots of changes rather than just one single COW file. I.e. perhaps use the existing snapshot feature to allow for multiple layers of changes, which can be merged back to the physical disk.

Also install the drivers (if they exist) within Windows 11 to help optimize the use within the VM.

## Separate low-memory Windows 11 installation

Evaluate a second Windows 11 installation using the reported low-memory Rufus installation method for Windows 11 26H2:

- https://www.neowin.net/news/you-can-soon-install-tiny-windows-11-26h2-isos-too-on-almost-all-unsupported-pcs/
- https://www.neowin.net/news/windows-11-26h2-reportedly-uses-shockingly-low-ram-with-a-rufus-installation-trick/

Install it to a virtual disk whose GPT partition layout and disk geometry mimic the physical Windows disk closely enough for a fair VM comparison. Both Windows installations will be on the same physical host, but the new VM installation must remain separate from the existing `p3` Windows installation. Do not write the new installation to physical partitions.

The new installation must be activated and properly licensed before it is considered usable or complete. Verify that the existing Windows 11 Pro license allows the VM installation to be used while retaining the `p3` installation, and confirm that Windows reports the VM as activated. Do not assume activation alone proves that the license covers both installations.

If the existing license cannot cover the VM while preserving the existing `p3` installation and its license state, stop before treating the new installation as complete and identify the valid licensing options. Do not remove, overwrite, or invalidate the existing `p3` installation or its license state.

Compare the lightweight installation's boot time, responsiveness, and memory use with the existing Windows 11 VM under the same KVM-backed configuration.

## Host runtime files for the guarded-COW VM

On the current host, keep these files for the `win11-phys` guarded-COW setup:

| Installed path | Purpose | Source or caller |
| --- | --- | --- |
| `/usr/local/libexec/qemu-cow-manager` | Privileged entry point for the Virt-manager COW save/discard controls. | `scripts/qemu-cow-manager`; Virt-manager calls it with `pkexec`. |
| `/usr/local/libexec/qemu-guarded-cow` | Validates the VM's configured partition allowlist, isolates the host mounts, and initializes, saves, or discards the guarded overlay. | `scripts/qemu-guarded-cow`; called by the libvirt hook and `qemu-cow-manager`. |
| `/etc/libvirt/hooks/qemu` | Runs guarded-COW preparation, verification, and host-state restoration on libvirt domain lifecycle events. | Installed from `scripts/qemu-guarded-cow-libvirt-hook`. The hook is not installed under `/usr/local/libexec`. |
| `/usr/local/bin/qemu-system-x86_64` | Custom QEMU executable selected by the `win11-phys` domain XML. | Built from this repository and installed by `scripts/build-install-qemu.sh`; includes `partition-write-guard`. |
| `/usr/local/bin/qemu-img` | Companion custom image utility used by the guarded helper to initialize and merge overlays through the partition guard. | Built from this repository and installed by `scripts/build-install-qemu.sh`; includes `partition-write-guard`. |

The current installed copies of both libexec helpers and the libvirt hook match
their source files. The two installed QEMU binaries match the corresponding
outputs in the external `qemu.build` tree. The live `win11-phys` process uses
the custom QEMU executable and its guarded block node.

`/usr/local/libexec/win11-phys-host-guard` is a legacy helper from the earlier
physical-disk design. It hard-codes `/dev/nvme0n1` and the old partition IDs,
and grants `libvirt-qemu` read/write access to the raw disk. The active libvirt
hook, installer, current VM XML, and COW manager do not call it; no external
references were found in the host configuration or user VM scripts during the
2026-10-13 audit. It is not required by the current guarded-COW setup and is a
candidate for removal after any private/manual invocations have been ruled out.

`/usr/local/libexec/qemu-bridge-helper` is a separate symlink to the
QEMU 9.2.50 bridge helper. The current VM uses a libvirt-managed tap on
`br-winproxy`, so this integration does not call that helper; other local QEMU
domains may. Do not remove package/application helpers from `/usr/local/libexec`
based only on this VM-specific inventory.
