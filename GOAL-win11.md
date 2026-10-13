ok, so now we need to make the Windows 11 VM faster and more responsive. Can we minimize the amount of emulation it does? Also, can we set up with COW thing where it gives us the option to save the changes after powering down the VM? Is this feature GUI enabled via virt-manager? If not, we should make it so.

The COW mode must be guarded: for this VM, the configured writable GPT partitions are 1, 3, and 7. The implementation must read the allowlist and disk identity from the VM configuration rather than hard-coding this example. Writes to every other partition and to disk areas outside the configured partitions must be rejected while running in the overlay, and changes must remain subject to the same allowlist when saved back to the physical disk.

### Safe COW save review and commit

Do not enable or use a physical-disk COW save until the guarded commit path has
passed the disposable-disk validation below. A failed command is not proof that
no writes occurred: a commit can write some or all allowed extents and still
report an error later. Never infer physical-disk safety from the command's exit
status alone.

Virt-manager must provide a two-step save flow:

1. **Review COW save** performs a dry-run/preflight and makes no writes to the
   physical disk. It verifies that the VM is fully shut off with no managed
   save, the overlay is healthy and backed by the expected disk, the configured
   GPT allowlist is valid, and the host partitions and swap are safely isolated
   for a later commit. The review must state clearly that this step did not
   write to the physical disk and that it does not itself prove a real commit
   will succeed.
2. **Commit COW changes** is a separate, explicit action enabled only when the
   review passes. Recheck the VM, disk identity, overlay, GPT layout, and mount
   state immediately before committing. Show the user the reviewed target and
   require a separate confirmation before the first physical write.

The review must include a verbose, inspectable report with the backing disk's
stable identity and size, overlay path and health, the configured writable
partition numbers and GPT identities, current host mount/swap state, guard
configuration, and planned commit extents. Do not present a printed command or
qcow2 allocation map alone as proof of what will be written. Planned extents
must come from the same commit traversal used by the real operation, include
any cluster-aligned/padded writes, and be checked against the partition guard.
Any planned extent outside the configured partitions must fail the review and
block commit.

The overlay geometry and merge algorithm must not expand a changed range across
GPT partition boundaries. Derive the overlay cluster size from the backing
device's logical sector size, or use a sector-granular merge that preserves the
actual changed-sector map; do not hard-code a 512-byte sector assumption. Test
512-byte and 4-KiB logical-sector fixtures, including writes at both ends of
each allowed partition. Measure the overlay performance and space cost of the
selected geometry under the Windows workload before choosing it for normal VM
use.

Show a graphical disk map in the review: include every GPT partition, GPT and
other out-of-partition areas, label each partition and its size/LBA range, and
mark the configured writable partitions separately from off-limits regions.
Use a proportional disk/LBA scale, a clear legend for allowed, off-limits, and
planned-write markings, and visually distinguish GPT metadata and unallocated
or otherwise unpartitioned space. Overlay the exact planned write extents on
the map, with a readable text list of their LBA ranges for small or dense areas
that cannot be distinguished at the map's scale. Make it clear that configured
partitions are merely eligible for writes; only the highlighted extents are
planned to change in this save. The map and report must be generated from live
disk/overlay state, not hard-coded partition numbers or a prior review.

Before enabling the real commit action, validate the exact installed QEMU and
helper versions end-to-end on disposable GPT disk images. Verify that changes
in allowed partitions reach only those partitions, deliberately place a change
in an off-limits region and prove the guard rejects it without changing that
region, compare the full base image before/after, check the overlay, and confirm
that success/failure status accurately reflects the completed writes and
close/flush results. Do not write to the physical disk during this validation.

On any failed or interrupted physical save, fail closed: keep the affected host
partitions unmounted (or read-only), retain the isolation lock, preserve the
overlay, and refuse to restore any partition read-write until the merge and
fresh-overlay replacement have both completed and been verified. Support a
safe retry of an incomplete merge. After a successful merge, verify the new
clean overlay before restoring prior host mount modes. Exercise these success,
failure, and retry paths before installing the helper or enabling the Virt-manager
commit action.

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

Keep a separate progress record for dated host audits, build/test results,
installation state, and other transient findings. Before removing any legacy
helper, search current system configuration and user scripts for references;
do not remove shared package or application helpers based only on this VM's
runtime-file inventory.

`/usr/local/libexec/qemu-bridge-helper` is a separate symlink to the
QEMU 9.2.50 bridge helper. The current VM uses a libvirt-managed tap on
`br-winproxy`, so this integration does not call that helper; other local QEMU
domains may. Do not remove package/application helpers from `/usr/local/libexec`
based only on this VM-specific inventory.
