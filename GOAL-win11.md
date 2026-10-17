# Windows 11 VM goals

This file is the index for the focused Windows 11 VM goals. Each linked document describes one work area and should be handled on its own. Keep transient host audits, command output, and dated progress in a separate progress record.

## Shared guarded-disk rule

The physical-disk VM uses a guarded COW overlay. For the current `win11-phys` configuration, the writable partitions are 1, 3, and 7. Treat that list as configuration data, not a code constant: tools must read the disk identity and writable-partition allowlist from the VM configuration and reject writes outside it both in the overlay and during a physical save.

## Focused goals

- [VM performance and guest drivers](GOAL-win11-performance.md): KVM/hardware acceleration, reduced emulation, and useful Windows drivers.
- [COW layers and snapshots](GOAL-win11-snapshots.md): external versus internal snapshot design, lineage, renaming, deletion, and branching.
- [Guarded COW save review and commit](GOAL-win11-guarded-save.md): review reports, review-ID lifetime, freshness policy, progress, and guarded write planning.
- [Interrupted-save recovery](GOAL-win11-save-recovery.md): partial writes, retry, flushes, reboot handling, and power-loss protection.
- [Disposable-disk validation](GOAL-win11-validation.md): the tests required before enabling physical or live commits.
- [Low-memory Windows comparison](GOAL-win11-low-memory.md): a separate licensed Windows installation and controlled performance comparison.
- [Host and libvirt integration](GOAL-win11-host-integration.md): domain XML permissions and the guarded VM runtime-file inventory.

## Suggested order

Establish the performance configuration, settle the snapshot and guarded-save designs, then complete disposable-disk validation before any physical commit. Treat interrupted-save recovery as a separate safety goal that must be completed before describing the workflow as power-cut-safe. The low-memory comparison and host integration can be progressed independently where their prerequisites permit.
