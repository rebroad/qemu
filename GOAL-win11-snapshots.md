# Goal: define layered COW snapshots for Windows 11

## COW layers and snapshot behavior

Provide a COW workflow that lets the user keep or discard guest changes after shutdown and manage multiple saved states through Virt-manager. If Virt-manager does not support the required save/discard controls or snapshot behavior, extend it.

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
