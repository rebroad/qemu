# QEMU/SunOS measurement guidance

For VM CPU measurements, use an elevated host shell and sample the live QEMU
PID over at least 10 seconds using `/proc/$pid/stat` or an equivalent
instantaneous measurement. Do not treat sandbox-limited readings, stale PIDs,
or lifetime-average `ps %CPU` values as valid evidence.

Never wrap a run using a persistent qcow2 disk in a hard timeout. A timeout may
terminate QEMU while the guest is writing and risk filesystem corruption. Use
`--throwaway` for forcibly time-limited exploratory runs; for persistent-disk
runs, wait for a guest-controlled clean shutdown or stop QEMU through its
monitor and verify the disk state before proceeding.
