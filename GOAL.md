Make an idle SunOS 4.1.4 guest relinquish host CPU time while remaining a usable, time-correct VM.

## Scope

- Diagnose and fix the QEMU/SunOS idle path. This includes changing and
  rebuilding `~/src/SunOS-4.1.4` if the guest kernel or drivers prevent an
  idle instruction, timer behavior, or interrupt-driven wakeup.
- Make the modified, rebuilt SunOS kernel a primary implementation path for
  guest idling. Do not treat increasingly long host-side polling sleeps as
  the final solution, and do not replace idle with a periodic one-second
  sleep; the guest idle path must remain promptly interruptible and
  responsive.
- Keep source edits in the source repositories and perform QEMU builds in the
  corresponding external `.build` tree under `/mnt/kingston/builds/`.
- Keep the SunOS source itself compilable by both the Debian host cross-build
  and a native SunOS build. Any difference required specifically by the host
  workflow or by QEMU testing must be generated and applied by the documented
  workflow, rather than maintained as a second source variant.
- Preserve unrelated existing work in the checkout.
- The normal VM network must attach to the host `spod` bridge. Networking is
  part of the acceptance path, not an optional fallback to user-mode or no
  networking.
- The guest must identify as `lily` and use the address expected by the host
  (`10.205.192.4`, currently recorded in `/etc/hosts` as `lily sunos`). If an
  existing snapshot already has that hostname and address, use that snapshot
  for testing rather than recreating the state or changing the persistent
  disk.
- Provide working telnet access to the SunOS guest and port/build an SSH
  server suitable for SunOS 4.1.4, then verify SSH access to the running VM.
  The guest is recorded in `/etc/hosts` as `lily` and that name should be used
  when verifying the services.
- If authentication is needed for testing, it is permitted to change the
  `rebroad` or `root` password through SunOS single-user mode.
- All QEMU CPU measurements must be taken from an elevated host shell against
  the live QEMU PID, using a real observation interval (at least 10 seconds);
  sandbox-limited or stale-PID readings are invalid evidence.
- The acceptance tests must be run in both graphical and `--nographic` modes;
  each mode must retain working prompt detection, idle CPU reduction, guest
  wakeup, clock, and `sleep 1` behavior.
- Keep adaptive icount diagnostics out of normal stderr output when requested
  by writing them to a dedicated `-icount debug-file=...` logfile, and make
  shift changes stable by enforcing a drift deadband and minimum dwell time.

## Definition of done

The goal is complete only when all of the following are demonstrated on the
same tested QEMU/SunOS build:

- With the SunOS guest booted and deliberately idle at a stable `login:`
  prompt or shell, repeated host measurements show that the QEMU process uses
  drastically less CPU than the pre-fix baseline. For SunOS kernel idle, the
  target is less than 3% QEMU CPU. Record the measurement method, host CPU
  model, QEMU command line, sample duration, baseline, fixed result, and the
  reduction. A useful pass criterion is at least a 90% reduction in QEMU CPU
  usage, unless the measured host baseline makes a more meaningful equivalent
  criterion necessary; any exception must be justified with measurements.
- Boot performance must not regress when idle detection is enabled. From the
  same launcher start to the same boot-complete marker, `--cpuidle` must take
  no more than 2% longer than the matching run without `--cpuidle` (at least
  98% of the non-idled speed), measured over repeated runs.
- When the SunOS guest is doing no work except a long sleep (for example, the
  logged-in shell is running `sleep 10`), the QEMU process must remain mostly
  idle, at less than 3% instantaneous host CPU during that interval.
- When the guest is stopped at an idle SunOS PROM prompt, the QEMU process
  must use less than 3% instantaneous host CPU, matching the SunOS kernel-idle
  target.
- Guest activity still wakes the VM and the guest remains responsive after the
  idle test.
- For both graphical and `--nographic` operation, the latency from a guest
  key press arriving at QEMU to the corresponding character being visible in
  the GUI or serial output must remain below 100 ms while the idle path is
  active.
- The modified SunOS kernel must be booted and tested as part of the final
  idle implementation, with its idle entry and interrupt wakeup behavior
  documented alongside the QEMU changes.
- Build the same modified kernel both inside SunOS and with the reproducible
  host-side cross-build, then compare the resulting boot images byte-for-byte
  (or record and justify any unavoidable toolchain metadata difference) before
  using either image as final idle-performance evidence.
  The source used by both builds must be the shared portable checkout; any
  QEMU-only instrumentation or instruction must come from the reproducible
  workflow patch applied to each build tree.
- While the guest is idle, compare a guest-readable wall clock with the host
  clock before and after a sustained observation interval. The offset and
  accumulated drift must remain within one guest clock tick or one second,
  whichever is larger, and the test must record the commands and timestamps.
- Run `sleep 1` inside SunOS repeatedly with host-side timing. Each run must
  take approximately one second (target range 0.9--1.1 seconds, allowing a
  clearly documented platform timing granularity), rather than returning
  immediately or taking materially longer.
- The normal launcher invocation `./run_Solaris112.sh` must work successfully
  without test-only options: it must start QEMU, complete its normal
  boot/monitoring path on the `spod` bridge, and shut down cleanly. In
  `--nographic` mode, startup must also avoid the current approximately
  30-second `No Keyboard Detected` delay (targeting no more than five
  seconds) without making the PROM hang or losing serial responsiveness.
- The running bridged SunOS guest must accept a verified telnet connection
  and a verified SSH connection as `lily`. Building/porting the SSH server
  and its required SunOS support is within scope.

## Evidence to retain

Record the upstream ref/tag and final commit, relevant QEMU and SunOS changes,
build commands, VM command line, CPU measurements before and after the fix,
clock samples, `sleep 1` timings, and the focused/full validation commands and
results. Do not mark this goal complete from compilation alone.
