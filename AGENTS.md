# QEMU/SunOS measurement guidance

For VM CPU measurements, use an elevated host shell and sample the live QEMU PID over at least 10 seconds using `/proc/$pid/stat` or an equivalent instantaneous measurement. Do not treat sandbox-limited readings, stale PIDs, or lifetime-average `ps %CPU` values as valid evidence.

Never wrap a run using a persistent qcow2 disk in a hard timeout. A timeout may terminate QEMU while the guest is writing and risk filesystem corruption. Use `--throwaway` for forcibly time-limited exploratory runs; for persistent-disk runs, wait for a guest-controlled clean shutdown or stop QEMU through its monitor and verify the disk state before proceeding.

## Desktop and guest control

Avoid taking focus from or injecting keyboard and mouse events into the host desktop while the user may be working. Prefer control paths that target the guest directly, such as SSH, the QEMU guest agent, QMP/HMP monitor input, or a guest remote-desktop interface. Do not use host-level automation such as `xdotool` for guest interaction when a direct guest-control path is available.  If host input automation is unavoidable, first confirm the target window and current focus, keep the action narrow, and restore the user's prior desktop state.

Anything requiring network (e.g. the `virsh` command) will require an escalated shell to function.
