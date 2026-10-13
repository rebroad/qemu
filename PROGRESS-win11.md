# Windows 11 VM progress

This file records dated implementation and host-state findings for the
`win11-phys` goal. Durable requirements and design constraints belong in
`GOAL-win11.md`; replace or append progress entries here as work continues.

## 2026-10-13

### Host and VM state

- `win11-phys` is registered with system libvirt and is shut off; it has no
  managed save. No QEMU guest process was running at the time of inspection.
- The inactive domain configuration uses KVM and host CPU passthrough, with a
  guarded COW overlay backed by `/dev/nvme0n1` and an allowlist for GPT
  partitions 1, 3, and 7. Its current devices still include SATA storage,
  rtl8139 networking, and QXL graphics; no device changes were made because
  activation should be checked first.
- The host had `/dev/nvme0n1p1` mounted read-write at `/boot/efi`; p3 and p7
  were unmounted. p4 and p6 were active swap, and p5 was mounted as `/`,
  `/home`, and `/mnt/p8`.
- No physical-disk writes were performed during this inspection.
- The overlay could not be checked as an unprivileged user because access was
  denied; `sudo -n` required a password. Its health therefore remains
  unverified.

### Guarded commit implementation and validation

- `qemu-img commit --backing-image-opts OPTS` was added so commit can use the
  explicitly supplied guarded backing node. The earlier traversal helper
  skipped filter nodes, which could bypass the partition guard; the explicit
  node path keeps the guard in the commit write path.
- The guarded helper now retains the source overlay until commit succeeds and
  replaces it with a fresh overlay. Overlay cluster size is derived from the
  backing device logical sector size, and unsupported sizes fail closed.
- A disposable 1 MiB GPT image check found exactly one intended 512-byte
  allowed-sector change and no other changed bytes.
- The first 4 KiB fixture exposed that a qcow2 overlay can hide the physical
  base's logical-sector alignment. The guard now follows the backing chain to
  the base node when determining GPT sector geometry.
- Focused test `./run ./tests/qemu-iotests/check -raw partition-write-guard`
  passed all five cases, including 4 KiB-sector overlay writes at both ends of
  each allowed partition and a guarded commit through a 4 KiB-aligned backing
  node. The suite also checks denied direct writes, rejection of a forbidden
  extent, full-base comparison, and retaining the source overlay.
- `qemu-img` compiled in the external build tree. Helper, install-script,
  domain XML, and syntax checks passed. The guarded helper now checks qcow2
  consistency and validates a freshly-created temporary overlay before
  atomically replacing the existing overlay.
- Performance/space measurements, graphical review-map implementation, and
  end-to-end validation of the installed binaries remain outstanding.

### Installed files and remaining gates

- The installed `/usr/local/bin/qemu-system-x86_64` matched the external build
  output at inspection. Installed `/usr/local/bin/qemu-img` and
  `/usr/local/libexec/qemu-guarded-cow` differed from current source/build
  outputs. No install command was run.
- The existing host state has not been proven safe for VM startup or a physical
  save. In particular, active swap and EFI mount isolation still need review.
- Do not enable physical-disk commit or write to the physical disk until the
  outstanding dry-run, installed-binary, failure/retry, and isolation gates in
  `GOAL-win11.md` have been implemented and validated.

### Latest runtime and isolation check

- The user started `win11-phys` after the earlier shut-off snapshot. A fresh
  read-only `virsh` check confirmed it is running, with no managed save. The
  live QEMU process is `/usr/local/bin/qemu-system-x86_64`, using KVM and the
  guarded COW overlay. Its raw `/dev/nvme0n1` node is opened read-only, and the
  partition guard is configured for p1, p3, and p7. The VM still has the IDE
  disk frontend, rtl8139 NIC, and QXL display; SPICE audio is configured.
- A fresh mount-table check while the VM was running showed `/boot/efi` resolves
  to the host root filesystem, not p1; p1, p3, and p7 were unmounted. p4 and
  p6 are active swap, both outside the configured write allowlist.
- The helper now enumerates active swap using this host's supported
  `swapon --show=NAME` syntax and rejects swap whose backing device matches an
  allowlisted partition. A read-only check passed for the live p3/p4 layout
  and rejected p4 when deliberately included in the allowlist.
- No VM interaction, overlay check, physical-disk write, or device
  reconfiguration was performed while the VM was running.
