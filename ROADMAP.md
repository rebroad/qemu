# QEMU/SunOS idle roadmap

`GOAL.md` is the acceptance contract. This file records implementation notes
and the latest validation evidence without changing the goal itself.

## Implemented

- `exp` is based directly on `upstream/staging-11.1` at `93483720ca`; the
  downstream work is flattened and published through `59b1342497`.
- QEMU idle waits arm the normal icount warp timer before sleeping. Adaptive
  icount uses a one-second drift deadband and a two-second minimum dwell.
- The normal SunOS launcher path uses `shift=off` host-clock timing. Explicit
  `--shift auto` remains available for diagnostics, with output in
  `icount-debug.log`.
- The serial command helper detects the `%` prompt while printing only newly
  received bytes. The launcher supports temporary autologin and throwaway
  disks.

## Latest evidence

Test build: external `qemu-system-sparc` build from
`configure --target-list=sparc-softmmu --enable-debug --disable-strip`.

- Boot marker: 59.71s without `--cpuidle`, 59.01s with it.
- Guest `sleep 10`: 0% instantaneous QEMU CPU over 10s.
- Direct idle PROM (`--noboot --cpuidle`): 0% instantaneous QEMU CPU over 10s.
- Guest `date; sleep 10; date`: 11 displayed seconds over 11.605s host time.
- Three guest `sleep 1` runs: one displayed guest second each; raw command
  times 1.351--1.388s, with 0.298--0.333s command transport overhead.
- A responsive shell command completed after the idle tests.

The pre-fix PROM-idle baseline was approximately 100% QEMU CPU in the matched
15s measurement; the fixed path measured 0% in the final shell-idle and PROM
samples.
