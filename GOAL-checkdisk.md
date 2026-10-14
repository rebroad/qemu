# Goal: compare physical p3 with the guarded COW view

Produce a read-only report comparing the Windows NTFS filesystem on physical
GPT partition 3 with the same partition as presented through
`/var/lib/libvirt/images/win11-phys-guarded-cow.qcow2`, and assess whether each
view passes available non-repairing filesystem checks.

## Safety requirements

- Do not write to the physical disk or the qcow2 image. Do not commit, rebase,
  compact, repair, or otherwise modify either image.
- First verify that `win11-phys` is fully shut off and that no QEMU process has
  the overlay open. If it is running or the image is otherwise in use, stop and
  report that a stable offline comparison cannot safely proceed; do not shut
  down the VM as part of this task.
- Identify the physical disk and p3 from the VM configuration and current GPT,
  checking stable disk identity, partition identity, start, and size. Do not
  assume a `/dev` name based only on the label “p3.” Confirm that the overlay
  presents the same GPT layout and partition boundaries.
- Inspect host mount and swap state before touching the physical partition.
  Never read the base through a host filesystem mount that is read/write. Do
  not silently unmount or remount a host filesystem. If a safe read-only view
  cannot be established without changing host state, stop and report the
  blocker.
- Expose the complete qcow2-plus-backing view through the configured QEMU
  block graph, with the export and underlying block device read-only. Preserve
  the VM's configured backing driver and disk identity; do not bypass the
  partition-write-guard configuration. Mount NTFS read-only with journal
  recovery/replay disabled where the selected driver supports that option.
  If read-only, no-replay access cannot be guaranteed, do not mount it.
- Never run a repair or write-capable operation. In particular, do not run
  normal `ntfsfix`, `chkdsk /f`, `chkdsk /r`, QEMU commit, or any repair mode.
  `ntfsfix -n` is non-writing but limited; it is not equivalent to Windows
  `chkdsk`. A successful read-only mount is not, by itself, a complete NTFS
  integrity check.

## Tooling changes are in scope

The work may modify QEMU and/or Virt-manager if needed to provide a reliable,
repeatable way to inspect the physical and composed p3 views through the
configured guarded block graph. Do not stop merely because an existing command
or UI does not expose the required view. Keep changes narrow, preserve the
partition guard and disk-identity checks, and enforce read-only access at the
overlay, backing-device, and exported-device layers. Never weaken or bypass the
write guard to make inspection easier. Validate any new path against a
disposable disk image before using it on the physical disk, and document the
source changes, tests, commands, and remaining limitations in the report.

## Measurements and checks

1. Record the physical disk identity, p3 GPT identity and boundaries, overlay
   metadata/backing chain, VM state, and relevant mount state.
2. Obtain the physical p3 and overlaid p3 views read-only. Validate their NTFS
   volume identities and sizes before comparing them.
3. Using the same tool and method for both views, report NTFS total capacity,
   allocated/used clusters and bytes, and free clusters and bytes. Also report
   file and directory counts and, if practical, summed logical file sizes;
   distinguish these figures because sparse and compressed files can make
   logical file sizes differ from allocated disk usage. State the units and
   method used, then calculate the differences.
4. Run only checks confirmed to be non-writing. Record the exact tool, version,
   command, exit status, and diagnostic output for each view. Distinguish a
   structural NTFS consistency check from a mountability/readability probe.
   If no full non-repairing NTFS checker is available on the host, say so
   explicitly; do not imply that `ntfsfix -n` or a successful mount proves the
   volume is fully consistent. Recommend a safe full-check method, such as
   checking a disposable copy with Windows, without running it against the
   physical p3 or the only qcow2 copy.
5. Compare directory/file metadata between the two views to identify notable
   guest-visible changes if this can be done read-only. Do not treat the
   comparison as a complete history of guest writes: qcow2 records block
   contents, not a chronological file-operation log.

## Deliverable

Write a concise report with side-by-side usage figures, the differences,
filesystem-check results and their limits, and the exact read-only procedures
used. Clearly state whether the overlay view was readable and whether any
check found errors. Keep all findings separate from the decision to commit the
COW to the physical disk; this task must not perform or authorize that commit.
