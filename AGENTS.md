# QEMU/SunOS measurement guidance

For VM CPU measurements, use an elevated host shell and sample the live QEMU
PID over at least 10 seconds using `/proc/$pid/stat` or an equivalent
instantaneous measurement. Do not treat sandbox-limited readings, stale PIDs,
or lifetime-average `ps %CPU` values as valid evidence.
