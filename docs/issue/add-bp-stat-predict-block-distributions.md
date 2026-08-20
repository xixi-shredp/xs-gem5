# bpu: Add opt-in committed predict/fetch-block distribution statistics

branch: xs-dev
commit: 80035e4f5d6754f50aa9bf2ddffd216216bf997a


On `xs-dev` (`80035e4f5d`), `configs/common/Options.py` provides
`--bp-db-switches`, but no `--bp-stat`; `DecoupledBPUWithBTB` has aggregate
gem5 counters and DB traces only. It neither tracks committed branch sequences
per dynamic FTQ entry nor writes `bp-stat.txt`. Thus branch-count, direction,
distance, and FTQ-occupancy *distributions* cannot be reconstructed after a
run; averages and peaks are insufficient.

## Code

1. Add `--bp-stat` as an opt-in `store_true` option and add
   `bpStat = Param.Bool(False, ...)` to `DecoupledBPUWithBTB`. Pass the value
   in `xiangshan.py` and both Decoupled-BPU creation paths in `se.py`.
2. Allocate sparse `std::map<uint64_t,uint64_t>` profiling state only when
   `p.bpStat` is true. In `notifyInstCommit()`, use
   `!inst->isNonSpeculative() && inst->isControl()` to identify branches and
   `PCState::branching() || inst->isUncondCtrl()` for direction.
3. Accumulate global state and per-`FetchTargetId` state. The ID must be the
   key: different dynamic loop iterations at the same static PC must not be
   merged into one predict block. Within each prediction window, additionally
   key fetch-block state by the instruction PC rounded down to the BTB's 32B
   FBlock boundary. At `commit()`, finalize the front FTQ ID before
   `ftq.commitTarget(tid)`.
4. Record predict/fetch-block branch, taken, and not-taken count histograms;
   all/taken/not-taken branch distances; not-taken branches between taken
   branches; and `ftq.size(tid)` once per BPU tick.
5. On exit, flush unfinished IDs and write `bp-stat.txt` with `key count`
   sections named `global_*`, `predict_block_*`, `fetch_block_*`, and
   `ftq_occupancy_entries_tid*`. Do not print average or peak fields.

The current implementation in `decoupled_bpred.{cc,hh}` and
`decoupled_bpred_stats.cc` is the reference patch for these steps.

My Solution:

- `configs/common/Options.py`, `configs/common/xiangshan.py`, `configs/example/se.py`
- `src/cpu/pred/BranchPredictor.py`
- `src/cpu/pred/btb/decoupled_bpred.cc`, `decoupled_bpred.hh`, `decoupled_bpred_stats.cc`

My Solution (specific xs-dev locations):

All line numbers below refer to the unmodified `xs-dev` base `80035e4f5d`.

| Baseline location | Current xs-dev code | Required edit |
|---|---|---|
| `configs/common/Options.py:282-285` | Defines `--enable-bp-db`; line 286 is the insertion point. | Add `--bp-stat` at line 286. |
| `configs/common/xiangshan.py:489-491` | Builds the BPU with only `bpDBSwitches`. | Add `bpStat=args.bp_stat` in this constructor call. |
| `configs/example/se.py:276-279`, `291-295` | Both SE BPU construction paths call `bpClass()` with no parameter. | Pass `bpStat=args.bp_stat` only when constructing `DecoupledBPUWithBTB`. |
| `src/cpu/pred/BranchPredictor.py:1217-1218` | Defines `bpDBSwitches` followed by `resolveBlockThreshold`. | Insert the Bool SimObject parameter between them. |
| `decoupled_bpred.cc:62-70` | Constructor initializes FTQ and existing stats. | Initialize `bpStatEnabled(p.bpStat)` and an optional `BpStatData` immediately after FTQ setup. |
| `decoupled_bpred.cc:144-146` | Exit callback only calls `dumpStats()`. | Conditionally call `dumpBpStat()` after `dumpStats()`. |
| `decoupled_bpred.cc:278-337` | `tick()` completes prediction and has no FTQ sampling. | Call `sampleBpStatFtqOccupancy()` once per active tick, after the active-thread check and before the squash early return. |
| `decoupled_bpred.cc:735-760` | `commit()` updates components then calls `ftq.commitTarget(tid)`. | Finalize predict/fetch state for `ftq.frontId(tid)` between those operations. |
| `decoupled_bpred_stats.cc:962-972` | `notifyInstCommit()` only updates the committed count. | Record the instruction before the existing FTQ commit-count update; use `inst->ftqId` plus the 32B-aligned instruction PC for the dynamic fetch-block key. |
| `decoupled_bpred.hh:129-131` | `FetchTargetQueue ftq` is followed by existing predictor state. | Add the profiling structs, optional state, and helper declarations at this point. |

## Validation

Run with `--bp-stat` and verify a non-empty `bp-stat.txt` with all required
sections and no average/peak rows. All 78 agent-bench train slices completed
with those conditions and without difftest failures, kernel panics, or fatals.
