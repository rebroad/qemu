# Goal: recover safely from interrupted COW saves

## Interrupted commits and power loss

A physical merge is not an atomic disk transaction. If QEMU is terminated or power is lost after writes begin, the raw backing disk may contain only part of the reviewed changes. Retain the qcow2 overlay until QEMU reports success; QEMU's `-d` option skips emptying the source overlay, and the helper must replace it only after a successful merge. Keep the overlay and baseline after interruption. Do not boot Windows natively, mount affected raw partitions read/write, discard the overlay, or restore host write access until recovery reconciles the base and overlay.

On the same boot, the retained overlay is the authoritative guest view and the save-pending state blocks ordinary restore/start paths. For recovery, first confirm QEMU uses neither image, preserve the overlay, keep host access read-only or unmounted, then use an explicit recovery/review path to inspect the partially updated base and safely retry. Never assume a killed process wrote nothing. A retry is safe only after idempotent merge and flush/error handling have been validated on disposable images.

QEMU `qemu-img commit` must flush the backing target after the merge and fail if that flush fails. If the source image is emptied, flush its metadata before reporting success. On either failure, retain the overlay and isolation state. Flushes ask the storage stack to make completed writes stable; they cannot make the multi-partition operation atomic or protect against hardware that falsely reports flush completion.

Current isolation/review records under `/run` do not survive reboot. Thus power-loss recovery is incomplete: after a crash, the disk may be partly updated without the pending record that prevents mounts or identifies a retry. Before calling the workflow power-cut-safe, add a durable, fsynced journal with disk, overlay, baseline, reviewed-plan identity, and commit phase; add an early-boot fail-closed guard against writable mounts and native boot; and make resume/retry idempotent. Retain the overlay until the new base and replacement overlay are verified, and clear the journal only after durable completion and safe mount-state handling.

Reduce risk during real saves by requiring clean guest shutdown for the offline path, avoiding other host I/O to the target disk, using reliable power protection such as a UPS or laptop battery, and keeping an independent backup of irreplaceable data. These measures reduce likelihood or impact but do not make an in-place multi-partition commit atomic.

On failure or interruption, fail closed: keep affected host partitions unmounted or read-only, retain isolation, preserve the overlay, and refuse read/write restoration until the merge and fresh-overlay replacement are both complete and verified. After success, verify the clean replacement overlay before restoring host mount modes.

### Required interruption tests

On disposable disks, terminate the commit during writes and between merge completion and overlay replacement. Verify the overlay remains readable, the base is recognized as partially updated, startup blocks unsafe access, and recovery either safely resumes or requires explicit manual reconciliation. Simulate reboot with volatile `/run` state removed, and abrupt power loss where the test environment supports it. An ordinary failure/retry test does not establish power-loss safety.
