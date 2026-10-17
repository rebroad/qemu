# Goal: commit-time estimates and review-ID override

Improve the guarded COW save command so users can understand how long review and commit phases may take, and can explicitly choose to commit without a review ID. This is a separate usability goal from the core guarded-save and interrupted-save-recovery requirements in [GOAL-win11-guarded-save.md](GOAL-win11-guarded-save.md) and [GOAL-win11-save-recovery.md](GOAL-win11-save-recovery.md).

Do not start the VM or write to the physical disk while implementing or validating this goal. Validate commit behavior only with disposable disk images and the existing guarded test infrastructure.

## Learned time estimates

Keep the existing opt-in `--progress` behavior. QEMU's percentage is traversal progress through the virtual disk; it is not a percentage of planned extents or physical bytes written. Preserve that label and do not present it as a direct measure of completed physical writes.

Show elapsed time and an estimated remaining time for the major phases, distinguishing review from commit. Track at least:

- review dry-run write-plan generation;
- review base fingerprint scans;
- commit safety validation;
- physical merge traversal;
- committed-overlay replacement and verification;
- host-state and firmware restoration, where applicable.

Use successful completed reviews and saves to build per-phase timing history. Associate samples with relevant workload and host factors, such as a hash of the target disk's stable identity, disk size, overlay size, planned extent count and bytes, and storage characteristics when available. Do not persist raw serial numbers or other unnecessary identifying data. Do not use wall-clock time, extent count, or total planned bytes alone to claim exact progress. Keep the history bounded and protected from unprivileged modification, in storage separate from the transaction-journal entries; malformed or missing timing history must not block a save or change any safety decision.

Offer a historical estimate before a phase starts when comparable samples exist, then refine it from observed elapsed time and progress once the phase is underway. Identify estimates as estimates and show elapsed time. If there is not enough comparable history or a phase has no reliable progress measure, say that an ETA is unavailable or low confidence instead of inventing one. A first successful review may establish review-phase history once its report is safely persisted. Commit-phase history is complete only after the full save, overlay replacement, verification, and required host-state restoration complete. Do not treat interrupted or failed runs as completed-duration samples.

The output should make the current phase clear and distinguish measured progress from predicted time. For example, QEMU's traversal percentage may be shown alongside a time estimate, but it must remain labeled as traversal progress. Estimates are informational only and must never alter, postpone, or waive any guard, validation, flush, journal, or recovery requirement.

## Explicit review-ID override

The ordinary commit flow continues to require a valid review ID by default. Add `--override-review-id` so the user can explicitly proceed without an ID or accept an expired ID. For example, `qemu-guarded-cow commit DOMAIN --override-review-id --progress` uses no supplied ID, while adding `REVIEW_ID` before the options selects that exact stored review. Clearly report whether the commit uses a current review, an expired review by explicit override, or no review ID.

The override is the user's decision to accept the review freshness and review-plan limitations. It must not silently extend or rewrite a review's recorded expiry, fabricate review evidence, or claim that the old review proves the backing disk has not changed. Before the first physical write, durably record the selected override mode, the review ID and original timestamps if supplied, and the user's explicit authorization. The progress output and final report must preserve that distinction.

Overriding the review-ID requirement does not override the mechanical safety gates. The VM must still be fully shut off with no managed-save state; the configured disk, GPT layout, and writable-partition allowlist must match; only the configured partitions may receive writes; those host partitions must be isolated from writable mounts, swap, and other block-device users; and the installed helper, QEMU graph, overlay, recovery guard, and durable transaction state must pass their applicable checks. Keep the guarded QEMU write path and its fail-closed behavior mandatory.

When no review ID is supplied, the explicit override path must perform a fresh full guarded review and write-plan internally before the first physical write; it must clearly say that the user did not separately inspect or authorize a review ID. With an expired ID, the override may use that exact stored review and plan only after clearly reporting its original creation and expiry times; it must not alter those timestamps. In both modes, do not imply that the review detects arbitrary raw writes made after review. Durably journal whether authorization used a current ID, an explicitly accepted expired ID, or no ID, and preserve the existing interruptible, resumable transaction behavior: retry must continue from that authorization without requiring another ID.

Provide the same explicit distinction in Virt-manager if its guarded-save workflow can initiate the affected path. Its confirmation must state the missing or expired review evidence and retain all existing user acknowledgements and machine-checked safety gates.

## Validation

Use disposable disk images to verify:

- ETA output with no timing history, one successful sample, and multiple comparable samples;
- estimates refine as progress arrives and remain clearly labeled as estimates;
- a failed or interrupted run does not become a completed-run duration sample;
- corrupt or unavailable timing history degrades to “ETA unavailable” without affecting commit safety;
- normal valid-review commits still work unchanged;
- missing and expired review IDs fail by default; no-ID override performs a fresh full review, while expired-ID override preserves and reports the original review timestamps;
- override authorization and mode are durably journaled before any physical write;
- all non-review safety checks, partition allowlisting, interruption handling, and resumable recovery still pass when an override is used;
- progress and completion are not reported until merge, overlay replacement, verification, and host-state restoration have succeeded.

No validation in this goal authorizes a physical-disk commit. Do not boot Windows to perform it.
