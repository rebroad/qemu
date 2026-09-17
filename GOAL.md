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
3. Boot performance must not regress when idle detection is enabled. From the
   same launcher start to the same boot-complete marker, `--cpuidle` must take
   no more than 2% longer than the matching run without `--cpuidle` (at least
   98% of the non-idled speed), measured over repeated runs.
4. When the guest is doing no work except a long sleep (for example, the
   logged-in shell is running `sleep 10`), the QEMU process must remain mostly
   idle, at no more than 5% instantaneous host CPU during that interval.
5. When the guest is stopped at an idle SunOS PROM prompt, the QEMU process
   must use less than 2% instantaneous host CPU.
6. Guest activity still wakes the VM and the guest remains responsive after the
   idle test.
7. While the guest is idle, compare a guest-readable wall clock with the host
   clock before and after a sustained observation interval. The offset and
   accumulated drift must remain within one guest clock tick or one second,
   whichever is larger, and the test must record the commands and timestamps.
8. Run `sleep 1` inside SunOS repeatedly with host-side timing. Each run must
   take approximately one second (target range 0.9--1.1 seconds, allowing a
   clearly documented platform timing granularity), rather than returning
   immediately or taking materially longer.

## Evidence to retain

Record the upstream ref/tag and final commit, relevant QEMU and SunOS changes,
build commands, VM command line, CPU measurements before and after the fix,
clock samples, `sleep 1` timings, and the focused/full validation commands and
results. Do not mark this goal complete from compilation alone.

## Current validation status

- `exp` is published at `d437a5099d`, with the downstream net patch following
  upstream `staging-11.1` at `93483720ca`. The branch is an ancestor of that
  upstream ref and the working tree has only unrelated generated directories.
- Build: configured in the external build tree with
  `configure --target-list=sparc-softmmu --enable-debug --disable-strip`, then
  built with `ninja -C /mnt/kingston/builds/rebroad/src/qemu.build/build
  qemu-system-sparc`.
- Final test command: `run_Solaris112.sh --nonet --nographic --autologin
  --throwaway --cpuidle`, using the committed QEMU binary, the throwaway disk,
  the serial socket, and `shift=off` host-clock timing by default.
- Boot-to-`Monitoring for system shutdown`: 59.71s without `--cpuidle` versus
  59.01s with it, so the idle path was 1.2% faster in the paired run.
- A guest shell running `sleep 10` measured 0% instantaneous QEMU CPU over a
  10s `/proc/$pid/stat` interval. The idle-at-shell samples also measured 0%.
- A direct `--noboot --cpuidle` PROM-idle sample also measured 0% instantaneous
  QEMU CPU over 10s.
- The final clock sample ran `date; sleep 10; date`: the guest advanced from
  07:37:25 to 07:37:36 EDT during 11.605s host time; the one-second display
  granularity keeps the measured offset within the required one-second bound.
- Repeated `date; sleep 1; date` runs produced one-second guest intervals.
  Raw host command times were 1.351--1.388s, with 0.298--0.333s `true`
  command transport overhead, giving corrected guest sleep times of about
  1.02--1.09s.
- `sunos-console-command` now prints only newly received serial bytes while
  still retaining the full reply for prompt detection. QEMU's adaptive icount
  diagnostics go to `icount-debug.log`; explicit adaptive runs now use a
  one-second drift deadband and two-second minimum dwell, while the tested
  default path uses host-clock timing because adaptive warp correction remains
  experimental.
- All required measurements are now present; the remaining task is to commit
  and push this evidence together with the scoped SunOS launcher changes.
