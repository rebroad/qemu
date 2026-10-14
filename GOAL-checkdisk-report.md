# Physical p3 vs guarded COW p3 report

Inspection date: 2026-10-14. This is a read-only comparison of the physical
Windows partition and the same partition through the configured qcow2 overlay.
No changes were written to the physical disk or the qcow2 image, and no merge
was performed.

## Identity and safe access

- `win11-phys` was shut off in `virsh`; no matching guest QEMU process remained
  during inspection.
- Physical disk: Samsung `MZVLQ1T0HALB-000H1`, serial
  `S4U7NF0R101890`, WWN `eui.002538a111c08f03`, size
  `1,024,209,543,168` bytes.
- GPT disk ID: `7FCD1006-6F91-4A9A-82F5-44C674A2DB4A`.
- p3: start LBA `567296`, length `439191552` sectors of 512 bytes
  (`224,866,074,624` bytes); PARTUUID
  `3dd1edd6-cd1b-415e-9692-065ac8ea678d`; NTFS UUID `34783B02783AC302`.
- The qcow2 reports a `954 GiB` virtual disk over raw `/dev/nvme0n1`;
  qcow2 file length is `50,038,374,400` bytes and `qemu-img info` reported
  `corrupt: false`. The VM guard configuration allows partitions 1, 3, and 7.
  The overlay GPT exposed the same p3 PARTUUID, type, start LBA, and sector
  count as the physical GPT.
  The inspection graph kept that guard and set the host-device, guard, qcow2
  file, qcow2 node, NBD export, and NBD client read-only. The NBD device
  reported read-only (`blockdev --getro` returned `1`).
- On revalidation for this report p1, p3, and p7 were not mounted. p4 and p6
  were active swap (about 5.1 GiB used on each); p5 was mounted as the host's
  Btrfs filesystem. No host filesystem mount was changed. Both p3 views were mounted using NTFS-3G with
  `ro,norecover`; `findmnt` confirmed `ro` on both.

The graph used the custom QEMU storage daemon's `host_device` node for the
physical disk, the VM's `partition-write-guard` node with partitions 1, 3, and
7, a read-only `file` node for the qcow2, a read-only `qcow2` node backed by
the guard, and a local NBD export with `writable=off`. The kernel NBD client
was also attached read-only. This exposed the composed image without bypassing
the configured guard.

## NTFS usage comparison

Both views reported NTFS 3.1, the same volume UUID, a 4096-byte cluster size,
`54,898,943` total clusters, and zero volume flags. Capacity from cluster count
is `224,866,070,528` bytes for each (the GPT partition is 4096 bytes larger).
Used clusters below are calculated as total clusters minus free clusters;
`ntfsinfo -m`'s “allocated clusters” field describes MFT allocation and is not
the whole-volume used-space count. `df -B1` independently returned the same
used and available byte counts.

| Measurement | Physical p3 | Overlay p3 | Overlay minus base |
|---|---:|---:|---:|
| Total clusters | 54,898,943 | 54,898,943 | 0 |
| Used clusters | 42,354,751 | 45,104,432 | +2,749,681 |
| Used bytes | 173,485,060,096 | 184,747,753,472 | +11,262,693,376 (+10.49 GiB) |
| Free clusters | 12,544,192 | 9,794,511 | -2,749,681 |
| Free bytes | 51,381,010,432 | 40,118,317,056 | -11,262,693,376 (-10.49 GiB) |
| Used | 77.15% | 82.16% | +5.01 percentage points |

## File and directory inventory

The same read-only Python `os.walk`/`lstat` scan was used on both mounts.
Names were keyed case-insensitively to match NTFS path comparison. The scanner
reported no errors.

| Entry count / path-size total | Physical p3 | Overlay p3 | Difference |
|---|---:|---:|---:|
| Regular files | 648,932 | 689,831 | +40,899 |
| Directories | 181,279 | 189,986 | +8,707 |
| Symlinks | 565 | 569 | +4 |
| Other entries | 78 | 78 | 0 |
| Sum of regular-file `st_size` by path | 194,456,222,543 bytes | 209,950,427,256 bytes | +15,494,204,713 bytes (+14.43 GiB) |

The file-size sum is logical size per enumerated path; NTFS hard links can make
it count the same underlying file data more than once. It is not a measure of
allocated disk space. The complete path-and-size difference is in
[`/var/tmp/win11-checkdisk-20261014-p3-file-diff.tsv`](/var/tmp/win11-checkdisk-20261014-p3-file-diff.tsv): 40,583 base-only entries, 90,193 overlay-only entries, and 6,146 same-path size changes (136,922 rows total, excluding unchanged paths). No entry changed type. The TSV columns are status, base size in bytes, overlay size in bytes, and path. Directory and symlink `st_size` values are included where present.

This describes how the two file trees differ; it is not a history of guest file
operations. qcow2 stores changed disk blocks, not a chronological filesystem
operation log.

## Checks and limits

- Mountability/readability probe: NTFS-3G `2022.10.3`, mounted each view with
  `ntfs-3g -o ro,norecover DEVICE MOUNTPOINT`. Both mounts succeeded and were
  confirmed read-only. `norecover` disables journal recovery/replay.
- Limited non-writing check: `ntfsfix v2022.10.3`, run as
  `ntfsfix -n /dev/nvme0n1p3` and `ntfsfix -n /dev/nbd0p3`; both exited 0.
  Output reported MFT/MFTMirr processing completed, the alternate boot sector
  was OK, NTFS version 3.1, and each partition was processed successfully.
  `-n` prevented changes. This is a limited check, not a full NTFS consistency
  proof or a replacement for Windows `chkdsk`.
- No full non-repairing NTFS consistency checker was available on the host.
  A full check should be performed on a disposable copy of the relevant disk
  view in Windows, so that any Windows online repair or requested repair mode
  cannot affect the physical p3 or the only qcow2 overlay. Microsoft documents
  `/scan` as an online NTFS scan and distinguishes it from repair switches;
  even so, use a disposable copy for this follow-up.

The checks found no errors within their limited scope. They do not establish
that every NTFS structure is consistent, that the overlay is safe to merge, or
that committing it to the physical disk is safe. This report does not authorize
or perform a commit.

## Reproduction tools

- Custom QEMU build: `qemu-img version 11.1.2`
  (`v11.1.2-66-gdd26c358280-dirty`); storage daemon from the same build.
- `ntfs-3g`, `ntfsinfo`, and `ntfsfix`: `2022.10.3`.
- Key measurement commands: `qemu-img info --backing-chain`,
  `ntfsinfo -m DEVICE`, `ntfsfix -n DEVICE`, `df -B1 MOUNTPOINT`, and the
  same Python path/size inventory over each read-only mount.
- The NBD export and mounts were disconnected after collection; the guest
  remained shut off.

Microsoft reference: [CHKDSK command reference](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk).
