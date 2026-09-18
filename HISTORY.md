# QEMU/SunOS idle history

This is the historical engineering record for the idle-guest work. The active
acceptance requirements remain in `GOAL.md`; current implementation status is
in `ROADMAP.md`.

## 2026-09-17

- Rebased the downstream `exp` work into a flattened net patch directly after
  upstream `staging-11.1` (`93483720ca`).
- Added built-in SunOS PROM idle signatures and gated learned OS signatures so
  stale learning data cannot throttle boot-time polling or device probes.
- Removed the generic repeated-PC fallback as an idle signal; repeated PCs are
  common in legitimate boot and filesystem wait loops.
- Moved adaptive icount diagnostics to the dedicated `icount-debug.log` file.
- Tried direct vCPU halt/longjmp paths. They either failed to reduce the actual
  post-boot CPU sample or triggered a `current_cpu` assertion during boot, so
  they were removed.
- Tried accounting host idle sleeps directly into the icount bias. This made
  guest wall-clock behavior worse and was reverted.
- The retained integration arms QEMU's normal icount warp timer before a
  trusted vCPU-side idle wait.
- Adaptive icount was observed to over-correct around idle warp events. A
  one-second drift deadband and two-second minimum dwell reduced adjustment
  chatter, but adaptive mode remains diagnostic/experimental. The launcher’s
  tested default is therefore `shift=off` host-clock timing.
- The console helper originally printed the complete accumulated serial reply
  on every poll, adding timing noise. It now prints only newly received bytes
  while retaining the complete reply for prompt detection.

## 2026-09-18

- Cross-built the SunOS `sun4m` control kernel and verified the PROM accepts
  its static a.out packaging. The kernel currently fails before the SunOS
  banner: early diagnostics report `romp=ffd0c7f8 magic=0 version=0` and
  `No handler for PROM?`. This is an unresolved PROM-vector handoff issue;
  no idle-CPU result from that kernel is valid yet.

## Measurements

- Historical pre-fix PROM-idle sampling was approximately 100% QEMU CPU over
  15s. The fixed shell-idle and PROM-idle samples were 0% over 10s.
- Final boot-marker pair: 59.71s without `--cpuidle`, 59.01s with it.
- Final guest `sleep 10` and direct PROM-idle samples both measured 0%
  instantaneous QEMU CPU.
- Final guest clock sample advanced 11 displayed seconds during 11.605s host
  time.
- Three final guest `sleep 1` runs each advanced one guest second. Raw command
  times were 1.351--1.388s; the `true` command transport baseline was
  0.298--0.333s.
