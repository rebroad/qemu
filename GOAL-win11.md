ok, so now we need to make the Windows 11 VM faster and more responsive. Can we minimize the amount of emulation it does? Also, can we set up with COW thing where it gives us the option to save the changes after powering down the VM? Is this feature GUI enabled via virt-manager? If not, we should make it so.

The COW mode must be guarded: for this VM, the configured writable GPT partitions are 1, 3, and 7. The implementation must read the allowlist and disk identity from the VM configuration rather than hard-coding this example. Writes to every other partition and to disk areas outside the configured partitions must be rejected while running in the overlay, and changes must remain subject to the same allowlist when saved back to the physical disk.

### Layered COW snapshots and commit semantics

The desired user experience is a configurable write-back cache: guest writes
are immediately visible to the guest, remain isolated until explicitly saved,
and can later be applied to the physical disk while newer guest writes continue
to accumulate. A qcow2 COW overlay can provide this behavior, but it is not
itself a FIFO cache. It records changed blocks relative to a backing image;
commit, discard, snapshot, and branch operations must preserve those block
dependencies and the guest's current view. The physical disk cannot simply be
updated underneath an overlay while QEMU continues using it as an unchanged
backing image: doing so can make reads combine new base blocks with stale
overlay assumptions, lose writes, or make rollback ambiguous.

Treat the physical disk as the current ground state and each COW image as a
versioned layer of block changes over its parent. Automatically seal the
current layer as a snapshot only after a clean guest shutdown has completed and
libvirt confirms the VM is fully shut off; the next boot must continue in a new
writable child layer based on the snapshot from which that boot started. A
crash, forced stop, or incomplete shutdown must not be reported as a
clean-shutdown snapshot. Snapshot creation must be automatic and idempotent for
each completed clean shutdown.

Register each sealed disk layer as a managed libvirt domain snapshot, so
Virt-manager displays the history through the same snapshot mechanism and UI
used for other libvirt snapshots. Generate a unique, useful name automatically
when sealing a layer, and let the user edit that display name in Virt-manager.
Keep a stable internal identity and parent relationship independent of the
editable name. The display name is snapshot metadata managed through libvirt.
These layered COW snapshots are external snapshots: their disk data lives in
separate qcow2 images linked by backing relationships. They do not automatically
have matching entries in a qcow2 internal-snapshot name table, and the
Virt-manager snapshot tree must not depend on that table. Do not create
redundant internal snapshots merely to mirror the editable display names.

### External overlay files versus internal snapshots

The current design uses one external qcow2 overlay file per sealed or active
COW layer, with each overlay backed by its parent layer. This makes each
delta's dependency explicit and lets libvirt manage a tree of disk snapshots.
The costs are more files and metadata to manage, backing-chain depth on reads,
and the need to preserve or deliberately reconnect every descendant when the
physical ground advances.

The alternative is one qcow2 image containing multiple internal snapshots.
QEMU stores a snapshot table and a separate L1 mapping for each internal
snapshot. “Shared clusters” means that if a data cluster is already allocated
in the qcow2 image, several snapshot mappings can refer to that one stored
cluster until a write needs a different version. If a sector is unallocated
and reads from the backing device, it has no data cluster in the qcow2 file to
share; the snapshot still needs mapping and snapshot-table metadata. The
external-overlay design behaves similarly for unchanged data by leaving it
unallocated in a child and reading it from its parent. Neither design normally
copies unchanged guest data into every snapshot. Internal snapshots therefore
do not inherently use more or less payload space; actual use depends on guest
writes, qcow2 cluster allocation, backing data, and metadata overhead, and must
be measured for the workload.

Internal snapshots avoid an external file per point and a chain of overlay
files, but all history is coupled to that image. QEMU's internal snapshot table
does not itself encode the libvirt parent/branch tree, and selecting an older
internal point as the new physical ground while preserving newer snapshots and
active writes still requires a carefully coordinated export/commit/reconnect
operation. It does not remove the guarded-commit or freshness requirements.

Keep the per-layer external-overlay design as the default for the guarded
physical-disk workflow because its backing relationships make the proposed
lineage and descendant-preservation rules explicit. Evaluate the single-file
internal-snapshot design as an alternative, but do not switch to it solely to
reduce file count; validate snapshot branching, disk-full behavior, recovery,
and committing a selected historical point before changing the design.

Add a lossless rename operation to QEMU's internal-snapshot interface so
libvirt can keep an internal snapshot's QEMU name synchronized when its
managed snapshot is renamed. Expose it over QMP for live libvirt use and via
`qemu-img` for offline maintenance. Identify the entry by stable snapshot ID,
not only its mutable name; check name length and collisions; preserve the ID,
disk mapping, VM-state data, and snapshot contents; and update the qcow2
snapshot table safely. Add tests for successful rename, collision and invalid
name failures, persistence after reopen, and unchanged restore/data behavior.
Libvirt must coordinate the QEMU rename with its own snapshot metadata update
and report or recover cleanly if either side fails. Never implement rename by
deleting and recreating the snapshot, since that would capture a different
state. External overlay layers do not get duplicate QEMU-internal entries just
to mirror their libvirt display names.

A later snapshot or unsealed COW layer must retain the exact parent snapshot it
was created from. For example, if A is sealed at shutdown, B is created as the
next boot's writable child of A, and C is later created as a child of B, their
lineage is ground → A → B → C. Committing A to the physical disk advances the
ground to A's cumulative state; it must not alter B's or C's stored deltas or
guest-visible contents. Update only the backing references and snapshot
metadata needed to reconnect B (and its descendants) to the new ground. Keep
divergent branches distinct and apply the existing explicit review/discard
rules to any branch invalidated by advancing the ground.

This ancestry model follows the normal external-snapshot/backing-chain model:
each layer records changes relative to its parent, while libvirt records the
managed snapshot tree. The custom requirements are automatic clean-shutdown
sealing, the guarded physical-partition backing device, and safely advancing
the physical ground while retaining valid descendants. They are not properties
provided automatically by an ordinary snapshot UI.

A sealed layer can be selected as the save boundary while the VM runs only if
QEMU atomically redirects subsequent guest writes to a descendant and the
commit updates/reconnects the block graph so that descendant reads use the new
ground plus its retained ancestors. The selected layer and its required
ancestors are then applied in order; writes after the boundary stay in the
descendant and are not part of that commit. This is a coordinated block-graph
operation, not generic cache draining. If the guest is stopped, the same
selection can use a validated offline commit.

### Snapshot deletion and chain reduction

Keep the existing **Delete snapshot** action; do not rename it to “merge with
previous” or “merge with next.” A snapshot is a named point in time, so Delete
clearly describes removing that point from the history. Libvirt's existing
delete behavior can merge the selected snapshot's changes into its children
when it has children, which can also shorten their backing chains.

Reuse the upstream deletion paths instead of adding a second generic delete or
merge implementation. QEMU already exposes live deletion of a qcow2 internal
snapshot through `blockdev-snapshot-delete-internal-sync`; this removes that
internal snapshot entry and releases clusters no longer referenced, but does
not merge an external overlay into another image. For external snapshots,
libvirt's QEMU snapshot-delete path performs the required block-job/metadata
work and merges the selected snapshot's changes into its children where
supported. The guarded physical-disk save remains separate: it applies a
reviewed snapshot boundary to the physical base under the partition guard and
freshness checks, rather than merely deleting a snapshot from the history.

Before confirmation, identify the exact snapshot by display name and stable
identity, show which children and backing layers are affected, and explain
which point-in-time state will no longer be available. State explicitly when
the operation will merge the selected snapshot's changes into its children,
what resulting disk states remain, and whether any branch or overlay data will
be removed. Keep unrelated branches intact. The deletion implementation must
retain its existing semantics unless a separately justified requirement calls
for changing them; do not add a separate direction-ambiguous merge action just
to expose the current delete behavior under another name.

The design must make the chosen cache boundary and resulting history explicit:
which sealed layers will become part of the physical ground, which active or
later descendants will remain pending, and which divergent branches become
invalid. Until a commit has completed and the graph or offline chain has been
verified, retain all source overlays and keep host access isolated. A failed or
interrupted commit may have partially changed allowed physical extents; it is
not automatically rollbackable like discarding an overlay. Recovery must
reconcile the actual base and overlay state before retrying, and must never
silently discard the only copy of a write.

Committing a selected snapshot means making its cumulative state from the
physical-disk root through that snapshot the new physical-disk ground state.
Descendant layers must remain usable by being correctly reconnected to the new
ground. Any sibling branch that diverged before the selected snapshot, and any
other history made invalid by changing the ground, must be identified in the
review and discarded as part of the confirmed operation. Do not silently
discard any layer. For example, committing A while later work is in a child
layer must preserve that child on the new ground; a sibling C branched from an
older point is discarded.

Prefer QEMU's live block-commit/block-job mechanism when the VM is running. It
must commit only the selected sealed layer and its required ancestors into the
guarded physical base, while redirecting ongoing guest writes to a descendant
layer that remains based on the resulting ground. This is a coordinated block
operation, not FIFO cache draining or an offline `qemu-img commit` against an
image that QEMU has open. Committing an older sealed point while a newer layer
is active is supported only if the live block job preserves that newer layer's
view and routes concurrent writes to it throughout the operation. If the
selected point is the active writable layer, first establish an atomic snapshot
boundary so subsequent guest writes land in a new child. If live commit through
the custom partition guard cannot be proven safe, require a clean guest
shutdown and use the validated offline path; do not simulate live cache
draining by copying blocks behind QEMU's back.

### Safe COW save review and commit

Do not enable or use a physical-disk COW save until the guarded commit path has
passed the disposable-disk validation below. A failed command is not proof that
no writes occurred: a commit can write some or all allowed extents and still
report an error later. Never infer physical-disk safety from the command's exit
status alone.

Virt-manager must provide a two-step save flow:

1. **Review COW save** performs a dry-run/preflight and makes no writes to the
   physical disk. It verifies the selected snapshot and full backing chain,
   disk identity, configured GPT allowlist, host mount/swap isolation, VM/QEMU
   state, and whether the selected point can use live commit or requires a
   clean shutdown. It must state clearly that it did not write to the physical
   disk and does not by itself prove a real commit will succeed. Before enabling
   the separate commit confirmation, ask the user to confirm that they reviewed
   the file-and-size differences for every affected partition, that the
   read-only base and overlay comparisons completed without mount, scan, or
   read errors, and that Windows shut down cleanly and this is the intended
   overlay state. These acknowledgements supplement, and do not replace, the
   machine-checked safety gates or disposable-disk validation.
2. **Commit COW changes** is a separate, explicit action enabled only when the
   review passes. The review ID is a record of what the user approved, not a
   lock on the VM or disk. At commit time, perform one fresh exact guarded
   write-plan traversal and compare disk identity, overlay, GPT layout, host
   state, root baseline, planned extents, and planned-range content with the
   reviewed report. Then repeat the cheap VM/configuration/mount/device checks
   at the write boundary. Avoid repeating the full virtual-disk traversal and
   full content fingerprints twice during one commit; that duplicates long
   reads without closing the final race after the last check. These checks do
   not lock out an arbitrary privileged process that writes directly to the raw
   disk; the configured partitions must stay unmounted and unused as swap, and
   the commit must not overlap another block-device writer. For a live commit,
   use QEMU's block job and verify its completion and graph transition; for the
   offline fallback, require a fully shut-off VM with no managed save. Show the
   reviewed target and affected branches, then require a separate confirmation
   before the first physical write.

The review must include a verbose, inspectable report with the backing disk's
stable identity and size, overlay path and health, the configured writable
partition numbers and GPT identities, current host mount/swap state, guard
configuration, and planned commit extents. Do not present a printed command or
qcow2 allocation map alone as proof of what will be written. Planned extents
must come from the same commit traversal used by the real operation, include
any cluster-aligned/padded writes, and be checked against the partition guard.
Any planned extent outside the configured partitions must fail the review and
block commit. Implement the human-readable sector report as
`scripts/qemu-cow-written-partitions DOMAIN`; it is a read-only planning tool,
not a substitute for the approved save-review action.

Show exact planned sector ranges for every affected GPT partition, both as
partition-relative sectors and absolute disk LBAs. Merge only adjacent ranges
with the same operation, report the logical sector size, operation, extent
count, and total planned sectors, and identify unaffected partitions as having
no planned writes. The report tool must run the guarded QEMU commit dry-run and
must not infer commit targets from qcow2 allocation maps or from sectors whose
current contents differ from the base. Recompute the plan for each review; a
previous report is not valid after the overlay or backing disk changes. This
plan identifies future commit targets, not the historical sequence or source
of guest writes.

The guarded block graph must select QEMU's block-device backend for a physical
whole-disk device and retain the regular-file backend for disposable image
fixtures. The dry-run must deny write permission to the backing node while
collecting the exact operations and ranges that the commit traversal issues.

The approved review path is incomplete until it also validates the overlay's
owner and restrictive mode, confirms that the installed guarded helper matches
the reviewed source, and has the required stopped-VM isolation/lifecycle state.
For the current helper policy, the overlay must be owned by the configured
libvirt QEMU user and have mode `0600`; a mismatch must fail review with a clear
diagnostic. Fix and verify these review prerequisites before considering any
real physical save. Keep physical commit disabled until the exact installed
QEMU and helper have passed disposable-disk success, failure, and retry
validation.

The command-line guarded commit must support an opt-in `--progress` flag. It
should show phase messages while revalidating the reviewed plan, QEMU's
percentage through the virtual-disk traversal while building each dry-run
write plan, byte-count percentages for fingerprint scans with a known byte
total, QEMU's live physical-merge percentage, and a completion message only
after the replacement overlay and host-state restoration succeed. Label the
dry-run percentage as traversal progress: it measures scanning the virtual
disk, not bytes planned or physically written. The actual merge percentage is
also traversal progress through the virtual disk, not the fraction of planned
extents physically written. Any long phase without a reliable denominator must
be named as in progress without inventing a percentage. The default CLI and
Virt-manager flows remain quiet/machine-readable as they are today.

### Interrupted physical commits and power loss

A physical commit is not an atomic disk transaction. If QEMU is terminated or
the host loses power after writes begin, the raw backing disk may contain only
part of the reviewed changes. The guarded path must retain the qcow2 overlay
until QEMU reports success; QEMU's `-d` option skips emptying the overlay, and
the helper replaces it only after the merge returns successfully. Keep the
overlay and its baseline intact after any interruption. Do not boot or mount
the raw partitions read/write, discard the overlay, or start Windows natively
until recovery has reconciled the partial merge.

On the same boot, the durable overlay remains the authoritative guest view and
the save-pending record blocks ordinary restore/start paths. After an
interruption, first confirm QEMU is no longer using either image, preserve the
overlay, keep host access to the affected partitions read-only or unmounted,
and use an explicit recovery/review path to compare the partially updated base
with the retained overlay before retrying. Never assume a killed process wrote
nothing, and never treat a successful exit alone as proof of power-loss
durability. A retry is safe only after the idempotent merge and its flush/error
handling have been validated against disposable images.

QEMU's `qemu-img commit` must explicitly flush the backing target after the
merge and return failure if that flush fails. If the source image is emptied,
its metadata must also be flushed before success is reported. The helper must
keep the overlay and isolation state when either operation fails. This asks
the storage stack to make completed writes stable; it still cannot make the
multi-partition update atomic or protect against hardware that lies about
flush completion.

The current isolation/review records live under `/run` and therefore do not
survive a reboot. This is not power-loss recovery: after a crash the physical
disk may be partially updated while the host no longer has the save-pending
record that prevents mounts or identifies a safe retry. Before treating the
workflow as power-cut-safe, persist a durable, fsynced commit journal containing
the physical disk identity, overlay and baseline identities, reviewed plan
fingerprint, and commit phase. Add an early-boot fail-closed guard that prevents
the affected partitions from being mounted writable or used for native boot
until that journal is reconciled. Make resume/retry idempotent and retain the
overlay until the new base and replacement overlay are verified. Clear the
journal only after durable completion and safe mount-state handling.
Reduce exposure during real commits by requiring a clean guest shutdown,
avoiding other host I/O to the target disk, using reliable power protection
(UPS or a laptop battery), and retaining an independent backup of data that
must not be lost. These measures reduce likelihood or impact; they do not make
an in-place multi-partition commit atomic.

Validate interruption recovery on disposable disks by terminating the commit
at several points, including during writes and between merge completion and
overlay replacement, then simulate reboot with volatile `/run` state removed.
Verify that the overlay remains readable, the base is recognized as partially
updated, startup blocks unsafe access, and recovery either safely resumes or
requires explicit manual reconciliation. Include abrupt power-loss simulation
where the test environment supports it. A successful ordinary failure/retry
test does not establish power-loss safety.

### Backing-disk freshness and conflict checks

Capture a backing-device baseline when each new COW root is created, then check
it again during review and immediately before the first physical write. Compare
the stable device identity (including major/minor, serial/WWN where available),
capacity, logical and physical sector sizes, GPT disk and partition identities,
partition boundaries, and configured partition identities. Record device and
filesystem metadata such as timestamps and size as useful diagnostic signals,
but treat any change as a reason to stop and investigate rather than as a
complete freshness test. The review must show the baseline and current values;
any identity, geometry, or layout mismatch blocks commit and requires fresh
reconciliation.

Also detect writes to base content relevant to the selected save since the COW
root was created, and detect conflicts since review. Record content fingerprints
at COW-root creation, at review, and immediately before commit, covering the
planned destination sectors plus GPT metadata and every other base sector on
which the selected chain depends. A mismatch at either comparison blocks the
commit and invalidates the review. Reads from the physical disk do not make the
base stale, though they may be reported as diagnostics.

Do not treat a block-device timestamp, inode timestamp, device name, or one
process's byte count as proof that the disk is unchanged. Timestamp checks
cannot prove that no raw write occurred; reads generally do
not update timestamps. Kernel block-device read/write counters can be useful
diagnostics, but are system-wide, resettable, and do not identify which sectors
changed, so they are not conflict protection either. Compare content, not just
metadata. If complete unchanged-base detection is required for correctness,
use a reliable whole-relevant-range fingerprint or write-tracking mechanism;
do not claim that checking only planned destination extents proves the entire
base is unchanged. Report the coverage and limitations of the selected check.

The overlay geometry and merge algorithm must not expand a changed range across
GPT partition boundaries. Derive the overlay cluster size from the backing
device's logical sector size, or use a sector-granular merge that preserves the
actual changed-sector map; do not hard-code a 512-byte sector assumption. Test
512-byte and 4-KiB logical-sector fixtures, including writes at both ends of
each allowed partition. Measure the overlay performance and space cost of the
selected geometry under the Windows workload before choosing it for normal VM
use.

Before enabling the real commit action, validate the exact installed QEMU and
helper versions end-to-end on disposable GPT disk images. Exercise a layered
chain with a selected intermediate snapshot, a preserved descendant, and a
divergent sibling. Validate live commit while guest writes continue in the
descendant, verify the resulting block graph and data, and verify that the
sibling is reported for discard. Also exercise the clean-shutdown fallback.
Verify that changes in allowed partitions reach only those partitions,
deliberately place a change in an off-limits region and prove the guard rejects
it without changing that region, compare the full base image before/after,
check every retained/discarded overlay, and confirm that success/failure status
accurately reflects completed writes and close/flush results. Do not write to
the physical disk during this validation. QEMU's general support for live
block-commit is not evidence that this custom guarded chain is safe; both the
live job and its graph transition must pass these tests before enabling it.

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

Evaluate a second Windows 11 installation using the reported low-memory method
for Windows 11 26H2. Do not assume Rufus alone creates a stripped-down Windows
image: Rufus 4.14's documented Quality of Life option disables selected bundled
Microsoft apps, while Tiny11 Builder creates a customized smaller Windows
image. The reported low-memory result is anecdotal and does not establish which
change caused it.

Use a controlled comparison with the same VM hardware profile, VirtIO drivers,
Windows update level, startup state, and workload. Compare a stock Microsoft
26H2 installation with a Tiny11 Builder **Tiny** image made from official
Microsoft media; record the input ISO hash, builder version/configuration, and
removed components. Evaluate Rufus's Quality of Life option separately if it
is part of the reported recipe. Do not use Tiny11 Core for this comparison
because its documented removal of servicing support would prevent a fair,
maintainable Windows Update comparison. Measure idle and workload memory,
boot time, and responsiveness more than once after the same settling period.

The referenced reporting is:

- https://www.neowin.net/news/you-can-soon-install-tiny-windows-11-26h2-isos-too-on-almost-all-unsupported-pcs/
- https://www.neowin.net/news/windows-11-26h2-reportedly-uses-shockingly-low-ram-with-a-rufus-installation-trick/

Install it to a virtual disk whose GPT partition layout and disk geometry mimic the physical Windows disk closely enough for a fair VM comparison. Both Windows installations will be on the same physical host, but the new VM installation must remain separate from the existing `p3` Windows installation. Do not write the new installation to physical partitions.

The new installation must be activated and properly licensed before it is considered usable or complete. Verify that the existing Windows 11 Pro license allows the VM installation to be used while retaining the `p3` installation, and confirm that Windows reports the VM as activated. Do not assume activation alone proves that the license covers both installations.

If the existing license cannot cover the VM while preserving the existing `p3` installation and its license state, stop before treating the new installation as complete and identify the valid licensing options. Do not remove, overwrite, or invalidate the existing `p3` installation or its license state.

Compare the lightweight installation's boot time, responsiveness, and memory use with the existing Windows 11 VM under the same KVM-backed configuration.

## Host runtime files for the guarded-COW VM

### Libvirt domain XML permissions

Persistent domain XML files, including `/etc/libvirt/qemu/win11-phys.xml`,
must remain owned by `root:libvirt` with mode `0640` after every libvirt or
Virt-manager rewrite. Do not assume the containing directory's SGID bit or
libvirt's umask controls this: libvirt may atomically replace the XML using
explicit ownership and mode settings. Determine and implement the narrowest
supported way to preserve the required owner, group, and mode, and verify it
after both `virsh` and Virt-manager configuration updates. Any post-write
permission repair must be scoped to the intended domain XML files and must not
weaken access to other libvirt configuration.

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
