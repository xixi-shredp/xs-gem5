# Finite Hint replay adversarial review

This is an independent counterexample review for the finite replay scope.  It
uses the frozen simulator source and the ICT186 kmhv2 snapshots already listed
in `source-manifest.json` and `target-interface-ledger.md`.  The observations
below are policy evidence for the main review; they are not claims that every
new RTL implementation is impossible.  A new finite block can pass these
cases only if it exposes the stated state, ordering and storage mechanisms.

## CEX-01: launch confidence must survive a following writeback

At query launch, `HintIndirect::query()` stores the selected physical
`entryId`, `key`, `slot`, target and `conf` in the query object
(`src/cpu/pred/btb/hint_indirect.cc:816-821`).  A commit enqueues an update
with `readyCycle = commit cycle + 1`; the update is not visible to the query
executed later in that CPU cycle (`src/cpu/pred/btb/hint_indirect.cc:974-987`,
`src/cpu/o3/cpu.cc:581-590`).  At writeback, the saved identity is checked and
used; the current hash/history is never recomputed
(`src/cpu/pred/btb/hint_indirect.cc:1029-1041`).

A concrete trace is: entry E has confidence 1 at cycle C; query Q selects E at
C and saves confidence 1; an older commit in C has enqueued a wrong-target
decrement, still immature during Q.  Q therefore produces an enabled result
at the simulator completion observation in C.  The tick at C+1 changes E to
confidence 0, while Q's saved confidence remains 1 through its later commit.
A retimed implementation that performs CAM in S1 and reads
Context in S2 at C+1 sees confidence 0 and suppresses Q.  This is an observable
functional mismatch.  The minimum fix is a launch-time confidence/entry
snapshot, or an equivalent versioned/atomic lookup that guarantees the S2
read belongs to Q's C state.  `HintQuery` storage must also retain target and
slot identity through FTQ lifetime.

This CEX does not require a particular number of pipeline stages.  It rejects
only the naïve `CAM -> delayed SRAM read` mapping without versioning.

## CEX-02: FIFO overflow is drop-new and has no backpressure

`enqueue()` drops the new update when the depth-eight FIFO is full and returns
normally (`src/cpu/pred/btb/hint_indirect.cc:974-994`).  The commit loop can
present up to `commitWidth=8` controls in one CPU cycle
(`src/cpu/o3/commit.cc:1325-1340`, `1429-1451`; anchor config
`evidence/config/pickle_pure_python_test/18/config.ini:132`).  A legal bounded
trace therefore starts with a full FIFO, drains at most one mature entry, and
then presents several ordered commits: only the available slot is accepted;
newer updates are dropped without stalling commit.  The unit test also drives a
nine-update burst and records eight accepted plus the ninth dropped
(`src/cpu/pred/btb/test/hint_indirect_test.cc:1170-1207`).

The same test verifies that a full FIFO does not bypass a same-cycle table read
(`hint_indirect_test.cc:1209-1216`), that each accepted update writes its saved
entry in FIFO order (`:1226-1244`), and that a repeated tick in the same cycle
does not issue another write (`:1237-1244`).  A candidate that applies all
commit updates, backpressures the producer, drops the oldest item, or exposes a
same-cycle confidence write is functionally different.  The minimum hardware
mechanism is an eight-entry ordered FIFO, ready-cycle maturation, one
confidence write per cycle, explicit drop-new accounting, and a separate
read/write arbitration rule.

The nine-update unit-test burst exceeds one CPU commit-width and is therefore
supporting evidence for queue semantics.  The full-at-entry plus legal
commit-width trace is the deployment-relevant form of the counterexample.

## CEX-03: wrapped or empty queries consume the query opportunity

`query()` records `lastQueryCycle` before checking the block range and returns a
query token for a null snapshot or `block_end <= block_start`
(`src/cpu/pred/btb/hint_indirect.cc:720-743`).  The range is half-open; an
address wrap does not become a circular range.  The test launches a wrapped
range at cycle 1 and then attempts a normal range at the same cycle; the second
launch is rejected (`src/cpu/pred/btb/test/hint_indirect_test.cc:1345-1351`).

An RTL front end that releases the query port when the first range is empty, or
retries it in the same cycle, permits an extra lookup and shifts all later
tokens.  The minimum mechanism is a one-query-per-cycle token/arbiter whose
accept state is consumed even for empty, wrapped, confidence-zero and
no-candidate blocks while Hint is enabled.  Off/finished mode returns before
consuming a query opportunity.  The token must still be carried with the FTQ entry so
completion and squash can discard it exactly once.

## CEX-04: earliest-PC arbitration and full-PC aliases are intentional

For a non-empty replay query, CAM candidates in the half-open block are
counted and the lowest PC wins; then the context set is selected from the
28-bit hash and ways are scanned in way order
(`src/cpu/pred/btb/hint_indirect.cc:744-815`).  The finite loader rejects a
duplicate valid tag in one set but permits distinct full PCs that hash to the
same key (`:615-644`).  The production unit test confirms that
`0x20001002` aliases the entry selected by `0x1000`, and that a third key in the
same set has no implicit third way (`hint_indirect_test.cc:537-547`).

Replacing the CAM with an unordered “first hit,” using a different tie break,
comparing full PC in Context, or adding an implicit associative way changes
which target is selected.  The minimum mapping is a deterministic 16-entry
priority comparator followed by set/way-ordered tag comparison, with the
28-bit key as the physical identity.  The current image remains frozen after
loading; the simplest exact design forbids runtime replacement while any
query/update can refer to that image.  A proposed hot-load/replacement feature
would need an additional protocol before it could preserve in-flight identity.

## CEX-05: cancellation is ordered and query completion is single-use

`complete()` marks a query completed once and applies cancellation predicates in
this order: not-taken, return, non-indirect, selected-PC mismatch, then no BTB
identification (`src/cpu/pred/btb/hint_indirect.cc:830-885`).  Only an eligible
non-return indirect with a real tag hit and saved confidence greater than zero
can override.  The caller writes the override only to stages at or after
`HintS3Stage=2` and only when the selected S3 entry is the same indirect PC
(`src/cpu/pred/btb/decoupled_bpred.cc:580-607`).  The tests cover all five
cancellation outcomes (`hint_indirect_test.cc:548-564`) and verify that an
earlier taken branch cannot train a later candidate (`:1299-1313`).

An RTL block that lets a return or direct branch inherit the candidate, accepts
two completions after a retry, or overwrites an earlier stage changes both
function and training.  The minimum mechanism is a one-shot query token with
the ordered predicates and stage-qualified override metadata.

## CEX-06: squash and trap recovery replay an ordered retired prefix

Normal squash restores the block-entry Hint snapshot, replays only retired
controls with `pc < squash_pc`, and applies the corrected control once
(`src/cpu/pred/btb/hint_history.cc:187-206`).  Trap recovery restores the
snapshot, truncates speculative history, then replays each retired control in
order; the prefix is bounded by `maxInstsNum = predictWidth/2` and an overlong
prefix is fatal (`src/cpu/pred/btb/decoupled_bpred.cc:1675-1743`,
`src/cpu/pred/btb/common.hh:467-477`).  The history tests specifically cover
equal-PC exclusion, call/return classification, not-taken direction and
replaying multiple events in one stream (`src/cpu/pred/btb/test/hint_indirect_test.cc:197-307`).

The current kmhv2 FTQ/BPU snapshots expose existing history pointers and a
single block update, but no Hint query identity or Hint checkpoint
(`evidence/rtl/FrontendBundle.scala:667-676`,
`evidence/rtl/Bundle.scala:96-123`, `evidence/rtl/NewFtq.scala:631-650`).
Restoring only the 256-bit kmhv2 GHR or the last CFI therefore loses Hint's
GITHR/LITHR/call/return state and can duplicate or omit an equal-PC event.  The
minimum bridge is a bounded FTQ-indexed checkpoint plus an ordered retired
prefix/replay interface; the implementation must specify its replay bandwidth
and latency.  A software loop bounded by 32 controls is not evidence that one
RTL cycle can perform 32 state updates.

## CEX-07: resetStats and finish are control semantics, not architectural reset

`resetStats()` first ticks to the requested cycle, clears measurements, and
retains trained confidence, histories and pending FIFO entries
(`src/cpu/pred/btb/hint_indirect.cc:1116-1140`).  The test enqueues an update,
resets stats, and observes that the carried-in update still writes next cycle
(`hint_indirect_test.cc:597-615`).  `finish()` marks the helper finished,
writes accounting, and deliberately does not advance time or drain pending
updates (`src/cpu/pred/btb/hint_indirect.cc:1252-1272`; test `:586-595`).

Treating statistics reset as a cold table reset, flushing the FIFO at finish,
or applying updates after finish changes later prediction state and output
accounting.  The minimum hardware boundary is an explicit measurement/reset
control distinct from cold reset, plus a defined stop point that freezes the
pending FIFO.  Host profile files and confidence-zero logs can stay outside the
RTL boundary, but their control event must not silently alter predictor state.

`measurementActive` is also functional: `commit()` does not enqueue training
while inactive, and `tick()` does not apply queued writes while inactive
(`src/cpu/pred/btb/hint_indirect.cc:891-893`, `1008-1010`).  Prediction itself
can still use preloaded confidence during warm-up; the test's wrong warm-up
branch produces no queued update (`hint_indirect_test.cc:497-503`).  Replacing
this control with an always-on trainer changes confidence at the measurement
boundary.  `setMeasurementActive()` ticks under the old active state before
changing the enable (`hint_indirect.cc:1102-1112`), so enable transitions need
the same edge ordering as normal queries.

## CEX-08: sleeping CPU catch-up does not grant extra write ports

When calls to `tick()` skip elapsed CPU cycles, the helper drains only entries
whose mature write cycles have passed.  It increments each logical write cycle
by one and loops at most over the eight pending entries
(`src/cpu/pred/btb/hint_indirect.cc:1006-1066`).  In the test, writes scheduled
for cycles 12 and 13 are both processed by a call to `tick(13)` and retain those
distinct logical write cycles (`hint_indirect_test.cc:513-523`, `:591-595`).

A mapping that clocks the Hint state only when the CPU invokes an observation
but performs only one write at that observation leaves obsolete confidence;
one that claims both writes occurred in cycle 13 violates the declared service
rate.  A free-running Hint update clock that performs one write at each elapsed
logical cycle is an exact finite mechanism.  A clock-gated implementation
requires a bounded catch-up schedule before the next query can observe state,
and must prove that it introduces no additional visible query latency.

## Relation to existing kmhv2 ITTAGE

The existing kmhv2 ITTAGE is a three-stage, single-port folded SRAM predictor
(`evidence/rtl/ITTAGE.scala:266-287`, `530-609`).  It suppresses a tagged
response after a request/update conflict and exposes no Hint history or FTQ
metadata.  Reusing it unchanged fails CEX-01, CEX-02 and CEX-06.  These are
direct-reuse failures; they do not establish that a separately instantiated,
finite Hint block with the mechanisms above cannot be added.

## Review disposition

The counterexamples establish required finite mechanisms and identify direct
reuse mismatches.  They do not decide ASIC timing or area: process node,
period/frequency, SRAM/register/CAM/logic budgets and a complete candidate
pipeline remain required inputs.  Until those inputs and the candidate timing
schedule are supplied, the corresponding main-review gates remain
`NEEDS_INPUT`/`UNPROVEN`; no claim of physical signoff is made.

## Frozen Candidate A application

Canonical reviewer role: `counterexample_review`.
Reviewed candidate: `candidate-mapping.md`.
Candidate SHA256: `89966f0fb46f9fdea46bde8de47c553e24afbb9a316b07e32c0a7bc8e820daf3`.
Candidate source manifest:
`sha256:2cfd621a9329aece46e072654414fb14260e15beaa1b523d894be50dde009e25`.
This section reviews that exact partial inventory, not the previously rejected
late-Context-read example.  No numeric capability is inferred from primitive
latencies or from the prose below.

| Source case | Exact candidate treatment | Adversarial disposition |
|---|---|---|
| CEX-01, launch snapshot | Lines 26 and 30 require immutable launch confidence/identity; line 30 explicitly rejects delayed Context read without bounded capture/version reconstruction. Line 12 requires mature-write-before-launch visibility and writer forwarding. | The known mismatch is excluded from the accepted proposal. The capture epoch, stall bound and storage are still unallocated; `NEEDS_INPUT`. A write-first primitive by itself does not preserve a launch version across later stalls. |
| CEX-02, queue order/drop-new | Lines 15–16 specify eight retained payloads, a mature release before program-order accepted prefix, and drop-new suffix. Lines 26 and 35 preserve no backpressure and require an explicit retirement/RMW/query schedule. | The proposed categorical behavior matches the source. Read1/write8 and read2/write1 are proposed resources only; no maturity/service schedule is certified. `NEEDS_INPUT`, with no current counterexample to an accepted completed schedule because no such schedule is claimed. |
| CEX-03, empty/wrapped query | Line 11 preserves full unsigned range comparison. Lines 32 and 38 leave token lifetime and all source topics as explicit obligations; no capability grants extra acceptance on an empty result. | The inventory does not separately instantiate the consumed-empty-query token/arbiter yet. This is an unresolved capability requirement, not an accepted permission to reuse that cycle's port. Keep empty/wrapped/null-snapshot consumption explicit in the final query contract; `NEEDS_INPUT`. |
| CEX-04, arbitration/alias | Lines 11 and 24 specify unsigned full-Addr comparison and earliest-member selection; line 26 preserves 28-bit Context aliases; lines 11 and 13 freeze CAM/slot contents after start. Context's two packed ways keep both tags and slots. | No source mismatch in the declared finite representation. Canonical address reconstruction, final tag-hit selection and physical resource realization remain required before function/timing PASS. No extra full-PC Context discriminator or hidden third way is introduced. |
| CEX-05, cancellation/stages | Line 26 preserves one-shot completion, cancellation and no retry. Line 31 explicitly declines to equate four source slots with three target physical stages, and requires matching the source observation. | Ordered cancel predicates, S3 identity qualification and token storage remain design obligations. No accepted new completion latency or bypass of returns is present; `NEEDS_INPUT`. |
| CEX-06, squash/trap | Lines 26, 33–34 require block-entry checkpoint plus ordered retired-prefix replay and separately count a possible corrected control. The source's 970-bit GHR and checkpoint state are explicitly unresolved. | The candidate avoids the last-CFI-only recovery error, but has not allocated checkpoints/prefixes or recovery bandwidth. The corrected control must remain conditional on `control_squash`; memory/trap recovery does not append an extra correction. `NEEDS_INPUT`, not a proven one-cycle recovery. |
| CEX-07, measurement/reset/finish | Line 26 separates cold initialization, statistics reset and finish-without-drain; lines 35–36 require measurement transitions and finite controls. | No cold-reset-at-stat-reset or finish-drain behavior is accepted. Control edge ordering, inactive training suppression and initialization protocol remain unresolved; `NEEDS_INPUT`. |
| CEX-08, idle catch-up | Line 35 requires preservation of modeled write epochs and a shared explicit cycle schedule. Line 38 deliberately leaves capabilities empty. | No extra same-cycle writes or one-observation-only writer are claimed. Active versus clock-gated operation and its next-query visibility remain undecided; `NEEDS_INPUT`. |

No definite source-semantic violation was found in the **accepted partial
Candidate A specification**.  This is not a function PASS: the candidate
intentionally leaves the mechanisms that would discharge the strongest cases
unresolved.  In particular, empty-query token consumption and the
control-squash-only correction predicate must survive elaboration of the
categorical contract; simplifying either away would turn the corresponding
case into a concrete function failure.

The canonical `counterexample_review` conclusion is **NEEDS_INPUT**.  Timing,
service, queue maturity, FTQ ownership, launch version capture, recovery and
control obligations must be connected to finite capabilities before these
counterexamples can be declared passed.  The proposed new block is not rejected
solely because current kmhv2 lacks it.  Physical frequency closure and area
closure remain UNPROVEN, and signoff remains NOT_PERFORMED.
