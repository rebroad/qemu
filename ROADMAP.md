# QEMU/SunOS idle roadmap

`GOAL.md` is the acceptance contract. This file records implementation notes
and current validation evidence without changing the goal itself. Historical
experiments and discarded approaches are in `HISTORY.md`.

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

## Idle implementation direction

PC/NPC probing is diagnostic and fallback-only. It is not a reliable primary
guest/host power-management interface: the running SPARC state can differ from
what the monitor reports, and a repeated PC can also be a boot-time poll.

The preferred implementation is to compile the SunOS 4.1.4 kernel's real
`sun4m` scheduler idle path (`_idle`/`sw_goidle`) with the SPARC
`wrpowerdown` instruction after interrupts are enabled. QEMU already models
that instruction as a halted vCPU which resumes on an interrupt. The rebuilt
kernel image must be installed in the throwaway test disk and its instruction
bytes verified before any CPU result is accepted. PROM probing may remain as a
small, conservative fallback until an equivalent firmware idle instruction is
identified.

Current state: the reproducible cross-build now produces and packages a
static SunOS SPARC a.out kernel, and a throwaway transfer has been exercised.
The control kernel still fails during early startup because its PROM vector is
reported as `romp=ffd0c7f8 magic=0 version=0`; it has not reached the SunOS
scheduler. The modified kernel therefore remains unvalidated and must not yet
be used for CPU acceptance measurements.

## Latest evidence

Test build: external `qemu-system-sparc` build from
`configure --target-list=sparc-softmmu --enable-debug --disable-strip`.

- Boot marker: 59.71s without `--cpuidle`, 59.01s with it.
- Earlier low-CPU readings are not acceptance evidence: they measured the
  QEMU main thread rather than the TCG worker. A live login-prompt run was
  subsequently observed at approximately 100% in the TCG thread. All idle
  results must therefore be repeated with elevated, per-thread process
  measurements after the explicit guest idle instruction is booted.
- Guest `date; sleep 10; date`: 11 displayed seconds over 11.605s host time.
- Three guest `sleep 1` runs: one displayed guest second each; raw command
  times 1.351--1.388s, with 0.298--0.333s command transport overhead.
- A responsive shell command completed after the idle tests.
