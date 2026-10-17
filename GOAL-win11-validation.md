# Goal: validate guarded COW commits on disposable disks

## Validation gates before physical use

Validate the exact installed QEMU and helper end to end on disposable GPT images; never use the physical disk for these tests. Cover:

- Allowed writes reach only configured partitions; a deliberate off-limits write is rejected without changing that region. Compare the complete base image before and after, and inspect every retained or discarded overlay.
- 512-byte and 4-KiB logical-sector fixtures, with writes at both ends of each allowed partition. Overlay geometry and merge behavior must not expand writes across GPT boundaries. Derive geometry from the backing logical-sector size or preserve a sector-granular changed-sector map; do not assume 512-byte sectors. Measure performance and space cost under the Windows workload.
- A layered chain with an intermediate selected snapshot, retained descendant, and divergent sibling. Verify the live block job preserves guest writes in the descendant, graph transition/data are correct, and invalid siblings are identified for discard. Also validate the clean-shutdown offline fallback.
- Accurate completion/error reporting, including close and flush failures, plus preservation of the overlay and host isolation on every incomplete path.

QEMU's general live block-commit support does not prove that this custom guarded graph is safe. Do not enable live or physical commit until both the block job and graph transition pass these tests. Exercise success, failure, and retry before installing the helper or enabling the Virt-manager commit action.
