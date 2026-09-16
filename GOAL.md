# Goal: QEMU `exp` and an idle SunOS guest

Keep the `exp` branch rebased onto the latest upstream QEMU 11.1.1 release
candidate, and make an idle SunOS 4.1.4 guest relinquish host CPU time while
remaining a usable, time-correct VM.

## Scope

- Rebase `exp` onto the latest upstream release-candidate commit for v11.1.1
  (normally `upstream/staging-11.1`, confirmed against the upstream release
  candidate/tag before the final rebase).
- Diagnose and fix the QEMU/SunOS idle path. This includes changing and
  rebuilding `~/src/SunOS-4.1.4` if the guest kernel or drivers prevent an
  idle instruction, timer behavior, or interrupt-driven wakeup.
- Keep source edits in the source repositories and perform QEMU builds in the
  corresponding external `.build` tree under `/mnt/kingston/builds/`.
- Preserve unrelated existing work in the checkout.
- Keep adaptive icount diagnostics out of normal stderr output when requested
  by writing them to a dedicated `-icount debug-file=...` logfile, and make
  shift changes stable by enforcing a drift deadband and minimum dwell time.

## Definition of done

The goal is complete only when all of the following are demonstrated on the
same tested QEMU/SunOS build:

1. `exp` is rebased onto the latest upstream v11.1.1 release candidate, with
   the downstream work represented as a reviewed net patch rather than a
   replay of obsolete experiment commits; record the resulting history and
   working tree.
2. With the SunOS guest booted and deliberately idle at a stable prompt,
   repeated host measurements show that the QEMU process uses drastically less
   CPU than the pre-fix baseline. Record the measurement method, host CPU
   model, QEMU command line, sample duration, baseline, fixed result, and the
   reduction. A useful pass criterion is at least a 90% reduction in QEMU CPU
   usage, unless the measured host baseline makes a more meaningful equivalent
   criterion necessary; any exception must be justified with measurements.
3. Guest activity still wakes the VM and the guest remains responsive after the
   idle test.
4. While the guest is idle, compare a guest-readable wall clock with the host
   clock before and after a sustained observation interval. The offset and
   accumulated drift must remain within one guest clock tick or one second,
   whichever is larger, and the test must record the commands and timestamps.
5. Run `sleep 1` inside SunOS repeatedly with host-side timing. Each run must
   take approximately one second (target range 0.9--1.1 seconds, allowing a
   clearly documented platform timing granularity), rather than returning
   immediately or taking materially longer.

## Evidence to retain

Record the upstream ref/tag and final commit, relevant QEMU and SunOS changes,
build commands, VM command line, CPU measurements before and after the fix,
clock samples, `sleep 1` timings, and the focused/full validation commands and
results. Do not mark this goal complete from compilation alone.

## Current validation status

- The local `exp` commit is `d745a25ac5`, based directly on
  `upstream/staging-11.1` at `93483720ca`; the rewritten branch is published
  as `origin/exp`.
- Matching headless 15-second PROM-idle samples measured approximately 100%
  QEMU CPU with idle handling disabled, versus approximately 0.5% with the
  SunOS idle halt path enabled and 0.2% with cooperative PROM sleeping. The
  adaptive icount diagnostics were written to a dedicated logfile rather than
  stderr.
- Guest boot-to-prompt responsiveness, guest/host clock offset, and repeated
  real-time `sleep 1` measurements remain outstanding; the goal is therefore
  intentionally still open.
