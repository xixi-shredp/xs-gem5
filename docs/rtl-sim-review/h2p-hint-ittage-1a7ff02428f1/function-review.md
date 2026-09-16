# Function and ordering review: finite Hint replay on kmhv2

This is the completed independent function-gate review of frozen Candidate A.
The scope is a new, explicitly integrated Hint module in ICT186
`/home/xuejing/rtl/XiangShan-kmhv2`; it is not a claim that the existing kmhv2
ITTAGE can be reused as-is.  Evidence is the frozen simulator working tree and
the kmhv2 snapshot at RTL commit `4b9ddb8a6311bff6f5e29cb3722d1f4236d66292`.

Reviewed candidate: `candidate-mapping.md`, SHA256
`89966f0fb46f9fdea46bde8de47c553e24afbb9a316b07e32c0a7bc8e820daf3`.
Source manifest identity:
`sha256:2cfd621a9329aece46e072654414fb14260e15beaa1b523d894be50dde009e25`.
The sixteen simulator/RTL source files cited by this review were reread or
cross-checked against the source manifest hashes; every checked file matched.
No build, test execution, simulation, source edit, or RTL edit was performed.

## Frozen candidate assessment

**Function gate: NEEDS_INPUT.**  Candidate A's stated categorical behavior has
no discovered contradiction with the finite replay source.  However, it
explicitly leaves version capture, FTQ lifetime, training enable/control,
history checkpointing, recovery and the edge schedule unresolved.  These
affect source-visible behavior and prevent PASS.  The rejected direct-ITTAGE
reuse and late-read examples below are not accepted features of Candidate A;
they do not force this partial new-module candidate to FAIL.

The candidate's 220-bit copied update record is functionally sufficient for
the retained confidence update payload: physical entry identity, key and slot
preserve the immutable table identity check, saved target reconstructs its
canonical 64-bit value for comparison against the full actual target, and
readyCycle retains the eligibility epoch (`candidate-mapping.md:15`, `:32`,
`:35-36`; `src/cpu/pred/btb/hint_indirect.cc:987`, `:1029-1041`).  It can
outlive FTQ reuse without retaining a freed pointer.  Launch confidence need
not be stored in this FIFO: it belongs to query override/accounting; writeback
increments or decrements the current entry counter.  Host diagnostic sequence
and commit-cycle logging are outside this payload reduction.  This is a
payload sufficiency check, not a proof of the undeclared FIFO service schedule.

The proposal to retain only sixteen local histories is valid for finite
replay: `historyPcs` is populated from valid CAM entries, then sorted for local
indexing (`src/cpu/pred/btb/hint_indirect.cc:443-449`,
`src/cpu/pred/btb/hint_history.cc:120-145`).  The source array's other four
entries cannot receive a replay PC.  Candidate integration must preserve that
PC-to-local-index association, including partially populated images.

The collision rules demand mature writes before launch observation and ordered
writer forwarding (`candidate-mapping.md:12`); the behavioral obligations
preserve drop-new, aliasing, shadow training and committed FIFO survival on
squash (`:26`).  No unspecified memory collision has been accepted as harmless.
The missing schedule is still needed to show that a future primitive choice
actually realizes those rules, including same-address operations and stalls.

The function gate can close only after the seven unresolved obligations in
`candidate-mapping.md:30-36` are connected to bounded state and an executable
cycle/interface definition.  The highest-priority gaps are immutable launch
confidence across arbitrary allowed stalls, query/checkpoint ownership through
FTQ reuse, ordered recovery before the next query, and training enable/reset
epochs.  Missing process, timing profile, and area budgets do not by
themselves cause the function status; these semantic bridges do.

`frequency_closure: UNPROVEN`; `area_closure: UNPROVEN`;
`signoff_status: NOT_PERFORMED`.

## Verdict implication

The existing kmhv2 ITTAGE is functionally incompatible with Hint replay when
treated as a direct implementation.  This is a **FAIL for direct reuse**.  A
new module could still implement the finite Hint contracts, but the necessary
interfaces and recovery protocol are absent from the captured RTL.  Those
bridges are therefore **NEEDS_INPUT**, rather than evidence that every new
implementation is impossible.  No function-gate PASS is justified until the
bridges below are specified and checked against RTL timing.

The mismatch is structural: the captured target parameters provide a 256-bit
global history (`evidence/rtl/Parameters.scala:395`), whereas Hint requires a
970-bit directional GHR plus separate target/call/return contexts
(`src/cpu/pred/btb/hint_types.hh:16-28`).  The target ITTAGE table entry is
valid/tag/counter/target-offset/useful (`evidence/rtl/ITTAGE.scala:244-253`),
while Hint requires full-PC CAM selection, a 28-bit context identity, shared
slots, and confidence state.  These are separate state machines, not a change
of tag width or target encoding.

## Launch snapshot, identity, and query arbitration

* At prediction launch, the simulator captures `HintHistory` and the 970-bit
  global history into a `HintSnapshot`, then passes that immutable object to
  `query()` (`src/cpu/pred/btb/decoupled_bpred.cc:553-562`).  The query retains
  block range, query cycle, selected PC, mask, 28-bit key, set, way, slot,
  target, confidence, and physical `entryId` (`src/cpu/pred/btb/hint_indirect.cc:796-826`).
  Commit and writeback use this saved identity; they do not rehash current
  history (`src/cpu/pred/btb/hint_indirect.cc:903-939`, `1029-1035`).
* Only one ordered block query is accepted per CPU cycle.  A second query at the
  same or an earlier cycle is a fatal contract violation, while an empty or
  wrapped half-open block still consumes the query slot
  (`src/cpu/pred/btb/hint_indirect.cc:720-742`).  Candidate arbitration chooses
  the lowest PC in `[blockStart, blockEnd)`; the range is not circular
  (`src/cpu/pred/btb/hint_indirect.cc:737-785`).
* The frozen kmhv2 BPU has a three-stage ready/valid pipeline
  (`evidence/rtl/BPU.scala:231-239`, `:369-441`) and its ITTAGE request carries
  only PC plus folded histories (`evidence/rtl/ITTAGE.scala:70-73`).  There is
  no Hint query token, block range, launch snapshot, candidate mask, or saved
  physical entry in the target bundle.  A new finite module needs an explicit
  one-query-per-cycle arbiter and a per-FTQ-entry snapshot/token path.  The
  target's existing `s1_ready` backpressure may be reused only after the Hint
  query acceptance and cancel behavior are defined; it cannot be assumed to
  provide those semantics.

## Completion and cancellation ordering

`complete()` records the baseline selected PC/target and applies cancellation
in this exact order: not taken, return, not indirect, selected-PC mismatch, no
BTB identification (`src/cpu/pred/btb/hint_indirect.cc:830-873`).  Only an
eligible, tagged, confidence-nonzero entry can override, and the override is
applied only at stages at or after `HintS3Stage` while matching the selected
indirect entry (`src/cpu/pred/btb/decoupled_bpred.cc:580-607`).

Required adversarial traces:

1. A candidate is found, but the baseline block is not taken.  The result must
   be a cancellation and no target override, even if the table entry is valid.
2. A taken return shares the same PC as a candidate.  The implementation must
   preserve `cancelReturn` as the recorded reason; a return must never receive
   the Hint target.
3. A taken direct branch at the candidate PC must cancel as `not indirect`.
4. A different selected PC in the same block must cancel as PC mismatch; an
   unidentifiable s3 entry must cancel as no-BTB.  The predicate order is
   observable through the cancellation counters; the functional result remains
   ineligible on every cancellation path.
5. An eligible tag hit with confidence zero is a shadow prediction: it must not
   override, but it must still reach commit training (`commit()` and
   `enqueue()`, `src/cpu/pred/btb/hint_indirect.cc:874-877`, `:934-939`).

The existing kmhv2 ITTAGE selects the provider/alternate provider in s2 and
registers a target into s3 (`evidence/rtl/ITTAGE.scala:544-609`), with no Hint
cancel reason or baseline-vs-final target metadata.  Directly wiring Hint onto
the current ITTAGE response would therefore lose the required precedence and
is a function FAIL.

## Confidence update FIFO and launch-time versioning

At commit, each eligible tagged hit enqueues an update for `commit_cycle+1`.
The finite FIFO has depth eight; when full it drops the **newest** update with
no backpressure (`src/cpu/pred/btb/hint_indirect.cc:974-994`).  Writes are FIFO
ordered, one confidence write per observed cycle, saturate in `[0,3]`, and use
the saved target and physical identity (`src/cpu/pred/btb/hint_indirect.cc:1019-1058`).
Repeated ticks in one cycle are no-ops, and a later tick may catch up elapsed
idle cycles while preserving each update's ready cycle (`:997-1024`).

Concrete ordering counterexample: entry E has confidence 1 at query cycle C.
Query Q saves confidence 1 and target T.  An older wrong commit for E at C
queues a decrement, applied at C+1, changing E to confidence 0.  Q remains a
tag hit and must train using its saved `entryId` and target; its saved
confidence still governs launch-time override/accounting.  The actual
read-modify-write saturates the current entry confidence at writeback.  A
retimed implementation that reads E at C+1 and uses that value to suppress Q
changes the behavior.  The
RTL must therefore carry either a versioned launch record or an equivalent
per-entry pending-update/scoreboard mechanism.  A plain S1 CAM followed by an
S2 context read cannot preserve this behavior.

The target ITTAGE has a different update protocol: it computes update index/tag
from commit PC and a 256-bit `ghist`, writes the table entry directly, and
suppresses a tagged response on a read/write conflict
(`evidence/rtl/ITTAGE.scala:284-287`, `:292-327`, `:341-357`).  It has no
eight-entry drop-new FIFO, saved Hint entry identity, or one-write confidence
service contract.  Direct replacement is therefore a function FAIL; the
finite Hint FIFO and scoreboard are unresolved integration requirements.

The configured CPU can commit up to eight instructions per cycle
(`evidence/config/pickle_pure_python_test/18/config.ini:132`).  A full-FIFO
trace does not need the standalone helper's nine same-cycle commits: start
cycle C with two pending eligible updates.  The CPU's Hint tick releases the
oldest before `commit.tick()` (`src/cpu/o3/cpu.cc:581-590`), leaving one.
If eight eligible tagged indirect controls then commit at C, the first seven
new updates are retained and the eighth is dropped.  A query at C sees only
the old write applied by the initial tick; it sees none of these seven new
updates.  A queue implementation that admits against pre-release occupancy,
drops the oldest, drains again at fetch, or backpressures the eighth commit
has a concrete mismatch.

## History, squash, and trap recovery

Hint history updates only on taken controls.  Calls and returns update bounded
64-entry PC windows; returns are excluded from indirect-target history even if
the instruction is also marked call.  Indirect history shifts target bits into
the Hint 128-bit global target history and configured 8-bit local target
histories.  The separate 970-bit directional GHR is captured in the launch
snapshot for hashing
(`src/cpu/pred/btb/hint_history.cc:154-183`; `hint_history.hh:23-34`).

Squash recovery restores the block-entry snapshot, replays retired controls
strictly before the squash PC, and optionally applies the corrected control once
(`src/cpu/pred/btb/hint_history.cc:187-206`).  Trap recovery restores the same
snapshot, clears component state, then replays the ordered retired prefix; the
prefix is bounded by the prediction block's maximum instruction count
(`src/cpu/pred/btb/decoupled_bpred.cc:1675-1743`).  Equality at the squash PC
must not replay the squashed branch twice.

Adversarial traces:

* A call and return in one block: both PC windows change, but the return must
  not shift the indirect target history.
* A taken indirect branch before a squash and a corrected branch at the squash
  boundary: replay the former once and the latter once, with no equal-PC double
  update.
* A trap after multiple retired controls, including a self-targeting fallthrough
  branch: replay the entire ordered prefix, not only the last branch.
* A prefix longer than the configured prediction-block bound must raise a
  defined overflow condition; silently truncating it changes future hashes.

The kmhv2 FTQ currently stores speculative metadata and a single block update
path (`evidence/rtl/NewFtq.scala:631-650`, `:1471-1480`), while its frontend
bundles expose no Hint history snapshot, retired-control list, Hint query token,
or recovery state (`evidence/rtl/FrontendBundle.scala:667-676`).  The new module
must specify these structures and their flush/redirect ownership before this
contract can be considered mapped.  This is a NEEDS_INPUT bridge for new
integration, and a direct-ITTAGE function FAIL.

## Reset, measurement, finish, and alias behavior

`resetStats()` advances pending work to the requested cycle, clears only
measurement counters, and preserves trained confidence plus FIFO contents
(`src/cpu/pred/btb/hint_indirect.cc:1115-1140`).  `finish()` validates accounting
but deliberately does not drain pending updates (`:1252-1267`).  These are
functional observability contracts when measurement controls are enabled; an
RTL implementation may expose equivalent control/status signals, but dropping
or draining state at either boundary changes behavior.

Measurement enable is also functional: `commit()` ignores scores when disabled
and `tick()` advances its last-observed cycle without draining
(`src/cpu/pred/btb/hint_indirect.cc:891-892`, `:1002-1009`).  The toggle calls
`tick(cycle)` using the old enable before changing it (`:1102-1112`).  For
example, disable at C with an update ready at C+1, remain disabled through
C+10, and enable at C+10.  The pending update does not apply at enable; it can
apply at C+11, and a query at C+10 still sees the old confidence.  Resetting the
FIFO or retroactively draining the disabled interval would violate this trace.

Finite replay uses a 16-entry CAM keyed by full PC plus a 5-bit mask, then a
28-bit hash into 2048 sets × 2 ways and 32 shared target slots.  The context
`key` is the derived set/tag identity (`src/cpu/pred/btb/hint_indirect.cc:615-644`).
Different full PCs can intentionally alias the same 28-bit context key; full-PC
CAM arbitration must remain the first discriminator.  The self-test exercises
same-key full-PC aliases (`src/cpu/pred/btb/test/hint_indirect_test.cc:545-547`)
and wrapped/empty query consumption (`:1345-1351`).  Replacing the CAM with
only the target ITTAGE's short tags or region target encoding would create
false hits and lose the required alias behavior.

## Minimum RTL mechanisms and open function inputs

To make a new kmhv2 Hint module reviewable, the design input must define:

1. A finite launch snapshot record (970-bit history or an explicitly equivalent
   undo/replay representation), full-PC/mask CAM arbitration, and a stable
   physical context identity carried through commit.
2. A query/complete interface tied to the target's three-stage ready/valid
   pipeline, including one accepted query per cycle, override at simulator
   stage index two and later, cancellation
   precedence, and flush invalidation.
3. Context/slot storage with the 2-way alias rule, confidence shadow training,
   versioned writeback, FIFO depth eight, drop-new overflow, one-write-per-cycle
   service, and defined same-cycle read/write behavior.
4. FTQ ownership for query snapshots and retired-control prefixes, plus ordered
   squash and trap replay with a bounded prefix and call/return history updates.
5. Reset/measurement/finish controls that preserve or discard state exactly as
   specified, and a preload/initialization protocol for the frozen finite image.

Until these interfaces, capacities, and conflict policies are supplied and
implemented, the function gate remains NEEDS_INPUT for new integration.  The
ideal mode's unbounded vectors and persistent local-history tree are outside
this finite function review and cannot be used to discharge the finite RTL
requirements.
