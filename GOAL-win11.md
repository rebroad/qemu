# Windows 11 VM: performance, snapshots, and guarded physical-disk saves

## Goals and safety boundary

Make the Windows 11 VM responsive by using KVM and hardware acceleration, minimizing device emulation, and installing useful guest drivers. Provide a copy-on-write (COW) workflow that lets the user keep or discard guest changes after shutdown and manage multiple saved states through Virt-manager.

The physical-disk VM uses a guarded COW overlay. Its configured writable GPT partitions are currently 1, 3, and 7, but code and tools must read the disk identity and writable-partition allowlist from that VM's configuration rather than hard-code these values. Guest writes must be rejected outside the configured partitions both in the overlay and when changes are applied to the physical disk.

## COW layers and snapshot behavior

Treat the physical disk as the current ground state and each external qcow2 overlay as a versioned layer of changed blocks over its parent. This is a write-back design, not a FIFO cache: the guest sees overlay writes immediately, while saving, discarding, snapshotting, and branching must preserve the guest's view and each layer's parent relationship. Do not update a physical backing disk under a live overlay unless QEMU atomically redirects writes and updates the block graph so reads cannot combine new base data with stale overlay assumptions.

After a clean guest shutdown has completed and libvirt confirms that the VM is fully shut off, automatically and idempotently seal the current layer as a snapshot. The next boot starts a new writable child of the snapshot from which that boot began. A crash, forced stop, or incomplete shutdown must not be recorded as a clean-shutdown snapshot.

Register sealed external layers as managed libvirt domain snapshots so Virt-manager shows them through its normal snapshot UI. Assign each a useful unique name automatically and allow the user to edit its display name. Keep a stable internal identity and parent relationship independent of that name. The display name is libvirt snapshot metadata; external overlay layers do not need duplicate entries in qcow2's internal snapshot-name table, and the snapshot tree must not depend on that table.

For example, if A is sealed at shutdown, B is created as its child, and C is later created as B's child, the lineage is ground → A → B → C. Committing A makes A's cumulative state the new physical ground without changing B's or C's stored deltas or guest-visible contents. Reconnect valid descendants to the new ground and retain their lineage. Keep divergent branches distinct; identify any branch invalidated by advancing the ground and require explicit review and confirmation before discarding it.

A live save of a sealed layer is supported only if QEMU atomically redirects subsequent writes to a descendant and updates the block graph so the descendant reads from the new ground plus its retained ancestors. Apply the selected layer and required ancestors in order; later writes remain in the descendant. If the selected point is the active writable layer, establish an atomic snapshot boundary first. If this operation through the custom partition guard and the resulting graph transition cannot be proven safe, require a clean shutdown and use the validated offline path. Never run offline `qemu-img commit` against an image QEMU has open, and do not simulate live cache draining by writing behind QEMU's back.

### External overlays and internal snapshots

Keep separate external qcow2 files per layer as the default. They make each delta's dependency explicit and let libvirt manage the snapshot tree, at the cost of more files and metadata, longer read chains, and careful descendant reconnection when the physical ground advances.

An alternative is one qcow2 image with multiple internal snapshots. QEMU stores a snapshot table and a separate L1 mapping for each snapshot. Several mappings can refer to one already-allocated data cluster until a later write needs another version. Unallocated sectors continue to read from the backing device and have no payload cluster to share; snapshots still require mapping and table metadata. External overlays similarly leave unchanged data unallocated in the child and read it from the parent. Neither design normally copies unchanged guest data into every snapshot. Space use depends on guest writes, cluster allocation, backing data, and metadata; measure it for the workload rather than assuming one design uses less space.

Internal snapshots avoid one external file per point and external backing-chain depth, but couple all history to one image. Their table does not encode libvirt's parent/branch tree, and selecting an old point as physical ground while retaining newer snapshots and active writes still requires coordinated export, commit, and reconnection. Do not switch from external layers solely to reduce file count; first validate branching, disk-full behavior, recovery, and committing a selected historical point.

Add a lossless QEMU internal-snapshot rename operation for libvirt to synchronize a managed snapshot rename where an internal snapshot exists. Expose it over QMP for live use and through `qemu-img` for offline maintenance. Identify snapshots by stable ID; check name length and collisions; preserve the ID, disk mapping, VM-state data, and contents; and update the qcow2 table safely. Test rename success, invalid names, collisions, persistence after reopen, and unchanged restore/data behavior. Libvirt must coordinate its metadata update with QEMU's rename and recover or report cleanly if either side fails. Never implement rename by deleting and recreating a snapshot. This operation does not create internal entries for external layers merely to mirror their libvirt display names.

## Snapshot deletion and chain reduction

Keep the existing **Delete snapshot** action. A snapshot names a point in time, so “delete” describes removing that point without introducing ambiguity about merge direction. Reuse upstream deletion behavior rather than adding a second generic delete/merge implementation.

QEMU's `blockdev-snapshot-delete-internal-sync` removes an internal snapshot entry and releases clusters no longer referenced; it does not merge an external overlay into another image. For external snapshots, libvirt's QEMU deletion path performs its supported block-job and metadata work and may merge the selected snapshot's changes into its children, shortening their backing chains. This history operation is separate from saving a reviewed snapshot boundary to the guarded physical disk.

Before confirmation, identify the selected snapshot by display name and stable identity; show affected children and backing layers, the point-in-time state that will disappear, whether changes will be merged into children, the resulting states, and any branches or overlay data to be removed. Keep unrelated branches intact and preserve current deletion semantics unless a separate requirement justifies changing them.

## Review and commit of physical-disk changes

Do not enable physical COW saves until the guarded path has passed the disposable-disk validation described below. A failed command may already have written some or all allowed extents; exit status alone does not prove whether the physical disk changed.

Virt-manager must provide two separate actions:

1. **Review COW save** performs a dry-run and makes no physical-disk writes. It checks the selected snapshot and backing chain, stable disk identity and geometry, configured GPT allowlist, host mount/swap isolation, VM/QEMU state, overlay health and permissions, installed helper identity, stopped-VM lifecycle state, and whether the selected point supports live commit or requires clean shutdown. It reports clearly that the review wrote nothing and does not guarantee that a later commit will succeed.
2. **Commit COW changes** requires explicit confirmation after review. The review ID identifies the reviewed report and its one-hour validity window. It is not a disk lock and cannot prove that nothing changed afterward. At commit time, validate that the ID is valid, unexpired, and belongs to this VM. Perform only the essential cheap checks: confirm the offline VM is fully shut off with no managed save, the configured disk and guard allowlist still match the report, the target device identity is unchanged, the configured writable partitions are unmounted and not used as swap, and no other block-device writer is active. Do not repeat the full virtual-disk dry-run traversal or content fingerprint scans at commit. Show the review time and expiry so the user can decide whether the reviewed state is still recent enough to authorize the write. The partition guard remains mandatory and continues to reject writes outside the configured allowlist.

Before enabling commit confirmation, ask the user to confirm that they reviewed the file-and-size differences for every affected partition, that read-only base/overlay comparisons completed without mount, scan, or read errors, and that the reviewed overlay state and shutdown/live-commit choice are intended. These acknowledgements supplement, but do not replace, machine-checked safety gates and disposable-disk validation.

The review report must be verbose and inspectable. Include stable backing-disk identity and size, overlay path and health, configured writable partition numbers and GPT identities, host mount/swap state, guard configuration, review creation and expiry times, and the exact planned write extents. Do not treat a printed command or qcow2 allocation map as proof of commit targets. Generate the plan from the same guarded QEMU commit traversal used by the real operation, including cluster-aligned or padded writes, and check every extent against the configured partitions. Any extent outside the allowlist fails review and blocks commit.

Implement `scripts/qemu-cow-written-partitions DOMAIN` as a read-only planning/report tool. It must run the guarded QEMU commit dry-run, not infer targets from qcow2 allocation maps or sectors whose current contents differ from the base. For each affected GPT partition, show exact planned ranges as partition-relative sectors and absolute disk LBAs; merge only adjacent ranges with the same operation. Report logical sector size, operation, extent count, and total planned sectors, and identify unaffected partitions as having no planned writes. A report is specific to the overlay and backing disk state reviewed at that time.

The guarded block graph must use QEMU's block-device backend for a physical whole-disk device and retain the regular-file backend for disposable fixtures. While collecting dry-run operations, deny write permission to the backing node. Preserve the exact installed-helper/source match, overlay owner and restrictive mode (current policy: configured libvirt QEMU user, mode `0600`), and required stopped-VM isolation/lifecycle state. Report clear diagnostics for any mismatch. Keep physical commit disabled until the exact installed QEMU and helper pass disposable success, failure, and retry validation.

### Backing-disk baseline and freshness information

Capture a baseline when each new COW root is created. At review, compare and report stable device identity (including major/minor and serial/WWN where available), capacity, logical and physical sector sizes, GPT disk and partition identities, partition boundaries, configured partition identities, and relevant base content. Any identity, geometry, layout, or baseline mismatch blocks review and requires reconciliation. Device and filesystem timestamps and sizes are useful diagnostics, but a metadata change is a reason to stop and investigate, not a complete freshness test.

Content fingerprints at COW-root creation and review must cover planned destination sectors, GPT metadata, and every other base sector on which the selected chain depends. Report coverage and limitations. Reads do not make the base stale, though they may be reported. Do not use a block-device timestamp, inode timestamp, device name, or process byte count as proof that the disk is unchanged. Kernel block-device read/write counters are system-wide, resettable, and do not identify sectors, so they are diagnostics, not conflict protection. If complete unchanged-base detection is required, use a reliable whole-relevant-range fingerprint or write-tracking mechanism; do not claim that hashing only planned destination extents proves the entire base is unchanged.

The one-hour review window is the deliberate tradeoff: review checks the baseline and content, while commit does not repeat full scans. It cannot detect arbitrary raw writes made after review. Make this limitation and the review timestamp/expiry visible so the user can decide whether to proceed. Do not silently turn this into a long second review at commit.

### Optional commit progress

The command-line guarded commit must support opt-in `--progress`. Show phase messages during review-ID and safety validation, QEMU's percentage through virtual-disk traversal while creating the review dry-run plan, byte-count percentages for fingerprint scans with a known total, QEMU's live physical-merge traversal percentage, and completion only after overlay replacement and host-state restoration succeed. Label both QEMU percentages as traversal progress: they are not the fraction of planned extents or physical bytes processed. For a long phase without a reliable denominator, say it is in progress without inventing a percentage. The default CLI and Virt-manager flows remain quiet and machine-readable.

## Interrupted commits and power loss

A physical merge is not an atomic disk transaction. If QEMU is terminated or power is lost after writes begin, the raw backing disk may contain only part of the reviewed changes. Retain the qcow2 overlay until QEMU reports success; QEMU's `-d` option skips emptying the source overlay, and the helper must replace it only after a successful merge. Keep the overlay and baseline after interruption. Do not boot Windows natively, mount affected raw partitions read/write, discard the overlay, or restore host write access until recovery reconciles the base and overlay.

On the same boot, the retained overlay is the authoritative guest view and the save-pending state blocks ordinary restore/start paths. For recovery, first confirm QEMU uses neither image, preserve the overlay, keep host access read-only or unmounted, then use an explicit recovery/review path to inspect the partially updated base and safely retry. Never assume a killed process wrote nothing. A retry is safe only after idempotent merge and flush/error handling have been validated on disposable images.

QEMU `qemu-img commit` must flush the backing target after the merge and fail if that flush fails. If the source image is emptied, flush its metadata before reporting success. On either failure, retain the overlay and isolation state. Flushes ask the storage stack to make completed writes stable; they cannot make the multi-partition operation atomic or protect against hardware that falsely reports flush completion.

Current isolation/review records under `/run` do not survive reboot. Thus power-loss recovery is incomplete: after a crash, the disk may be partly updated without the pending record that prevents mounts or identifies a retry. Before calling the workflow power-cut-safe, add a durable, fsynced journal with disk, overlay, baseline, reviewed-plan identity, and commit phase; add an early-boot fail-closed guard against writable mounts and native boot; and make resume/retry idempotent. Retain the overlay until the new base and replacement overlay are verified, and clear the journal only after durable completion and safe mount-state handling.

Reduce risk during real saves by requiring clean guest shutdown for the offline path, avoiding other host I/O to the target disk, using reliable power protection such as a UPS or laptop battery, and keeping an independent backup of irreplaceable data. These measures reduce likelihood or impact but do not make an in-place multi-partition commit atomic.

On failure or interruption, fail closed: keep affected host partitions unmounted or read-only, retain isolation, preserve the overlay, and refuse read/write restoration until the merge and fresh-overlay replacement are both complete and verified. After success, verify the clean replacement overlay before restoring host mount modes.

## Validation gates before physical use

Validate the exact installed QEMU and helper end to end on disposable GPT images; never use the physical disk for these tests. Cover:

- Allowed writes reach only configured partitions; a deliberate off-limits write is rejected without changing that region. Compare the complete base image before and after, and inspect every retained or discarded overlay.
- 512-byte and 4-KiB logical-sector fixtures, with writes at both ends of each allowed partition. Overlay geometry and merge behavior must not expand writes across GPT boundaries. Derive geometry from the backing logical-sector size or preserve a sector-granular changed-sector map; do not assume 512-byte sectors. Measure performance and space cost under the Windows workload.
- A layered chain with an intermediate selected snapshot, retained descendant, and divergent sibling. Verify the live block job preserves guest writes in the descendant, graph transition/data are correct, and invalid siblings are identified for discard. Also validate the clean-shutdown offline fallback.
- Success, failure, retry, and termination during writes and between merge completion and overlay replacement. Verify overlay readability, partial-base recognition, safe startup blocking, and retry or explicit manual reconciliation. Simulate reboot with volatile `/run` state removed; add abrupt power-loss simulation where supported. Ordinary failure/retry success does not prove power-loss safety.
- Accurate completion/error reporting, including close and flush failures, plus preservation of the overlay and host isolation on every incomplete path.

QEMU's general live block-commit support does not prove that this custom guarded graph is safe. Do not enable live or physical commit until both the block job and graph transition pass these tests. Exercise success, failure, and retry before installing the helper or enabling the Virt-manager commit action.

## Separate low-memory Windows 11 installation

Evaluate a second Windows 11 installation using the reported low-memory method for Windows 11 26H2. Do not assume Rufus alone creates a stripped-down image: Rufus 4.14's documented Quality of Life option disables selected bundled Microsoft apps, while Tiny11 Builder creates a customized smaller Windows image. The reported memory result is anecdotal and does not establish which change caused it.

Use the same VM hardware profile, VirtIO drivers, Windows update level, startup state, and workload to compare stock Microsoft 26H2 with a Tiny11 Builder **Tiny** image created from official Microsoft media. Record the input ISO hash, builder version/configuration, and removed components. Evaluate Rufus's Quality of Life option separately if testing the reported recipe. Do not use Tiny11 Core: its documented servicing removal would prevent a fair, maintainable Windows Update comparison. Measure idle and workload memory, boot time, and responsiveness multiple times after the same settling period.

The referenced reporting is:

- https://www.neowin.net/news/you-can-soon-install-tiny-windows-11-26h2-isos-too-on-almost-all-unsupported-pcs/
- https://www.neowin.net/news/windows-11-26h2-reportedly-uses-shockingly-low-ram-with-a-rufus-installation-trick/

Install the comparison system on a virtual disk with GPT layout and geometry close enough to the physical Windows disk for a fair VM comparison. Keep it separate from the existing `p3` Windows installation and never write the new installation to physical partitions.

The new installation must be activated and properly licensed before it is considered usable or complete. Verify that the existing Windows 11 Pro license permits using the VM installation while retaining the `p3` installation, and confirm Windows reports the VM as activated. Activation alone does not prove that the license covers both installations. If the existing license cannot cover the VM while preserving the existing installation and license state, stop and identify valid licensing options. Do not remove, overwrite, or invalidate the existing `p3` installation or its license state.

Compare the low-memory installation's boot time, responsiveness, and memory use with the existing Windows VM under the same KVM-backed configuration.

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
