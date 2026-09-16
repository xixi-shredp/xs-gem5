# Unlimited Hint implementation contract

This contract fixes interfaces before the three implementation tasks begin.
The default finite model and all version 2 inputs remain compatible.

## Configuration and history

* Python/BPU parameter: `idealHintIBP = False`; CLI: `--ideal-hint-ibp`.
* C++ `HintIndirect::Config::ideal = false`.
* `HintHistory::configure(pcs, bool ideal = false)`. Finite snapshots retain
  their current array. Ideal snapshots share an immutable persistent local
  history root, with O(log P) lookup and path-copy update.
* `HintIndirect::isIdeal()` exposes the flag to integration/tests.
* Keep one query/cycle, existing complete()/commit()/tick() order, stage 2
  override, measure-only scoring and the bounded confidence FIFO unchanged.
* Occupancy boundaries must use CPU cycle identities. Extend measurement
  notification with the current cycle, retaining an overload/default for old
  production tests if needed. Count elapsed measure cycles, including idle
  cycles; reset retains tables/confidence/FIFO/history.

## Versioned interchange

Finite metadata/candidates/profile use version 2 exactly as before. Ideal
metadata/profile use version 3, `storage: "unlimited"`, `hash_version: 2`,
the unchanged `ghr_bits`, `mask_order` and `history` fields, plus:

```json
{
  "layout": {"conf_queue_depth": 8},
  "finite_reference": {
    "cam_entries": 16, "context_entries": 4096, "ways": 2, "slot_entries": 32
  }
}
```

Metadata format remains `h2p-hint`. CAM rows and target strings retain their
v2 encoding. Ideal Context rows are `{ "valid": true, "key": 123,
"slot": 0, "conf": 3 }`; key is a nonnegative 28-bit JSON integer. Keys
are unique even for invalid rows. Valid entries start at confidence 3. Same
key across PCs must share one mapping/confidence. No full-context/PC tie-break.
Slot values remain deduplicated canonical even addresses. Raw baseline target
observations remain unrestricted uint64, including noncanonical wrong values.

Ideal candidates: `format: "h2p-hint-candidates"`, `version: 3`,
`storage: "unlimited"`, `pcs` and `source`; unlimited unique PCs. A mode/input
version mismatch is an error. Existing 96-byte decoded profile rows need not
change; cache identity includes the complete header and input SHA.

Ideal metadata entry array order defines stable resident entry identity.
HintQuery gains `entryId` (size_t, invalid sentinel for no hit); `slot` is a
size_t. Queued updates preserve this identity and the prediction-time target.
Finite lookup also records its vector index. The public set/way fields remain
for finite diagnostics; ideal diagnostics additionally use key/entry_id.

## Occupancy output

`hint-stats.json`: retain existing fields; finite version 2, ideal version 3;
add `storage: "finite"|"unlimited"` and `occupancy` keyed by `cam`, `context`,
`slots`. Each has `resident_entries`, `avg_entries`, `peak_entries`,
`sample_cycles`, `finite_capacity`, `resident_multiple`, `avg_multiple`,
`peak_multiple`. Capacities are 16/4096/32. Counts are resident valid rows;
zero-confidence entries still occupy space. Collect marks this comparison
inapplicable and reports zero hardware occupancy. Frozen tables produce equal
resident/average/peak when sample_cycles > 0; with zero cycles avg/peak are 0.

stats.txt fields are `h2pHint.<table>_<field>` for the same table/field names.
No percentage of infinity. Existing finite non-Hint performance statistics
must remain identical. Global reset restarts sampling without changing state.

## Ownership

A owns hint_indirect.hh/.cc and production test driver/config/input-test files.
B owns hint_history.hh/.cc, hint_types.hh if required, decoupled_bpred integration,
BranchPredictor.py, configs/common/H2PHint.py and a separate history test include.
C owns util/h2p_hint, util/run-h2p-hint-ideal.py, Python tool tests and experiment
documentation. Root owns this contract, plan.md, remote preparation/integration,
independent validation and final delivery. Coordinate before crossing ownership.
