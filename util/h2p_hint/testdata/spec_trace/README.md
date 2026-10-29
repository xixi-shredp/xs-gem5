# Independent SPEC raw trace fixtures and offline CLI

Project execution constraint: smoke tests, production gem5 helper tests, final
integration verification, and performance experiments run only on ps1 and must
record the executing hostname. Existing local fixture/unit results are local
software checks, not ps1 experiments. The offline tool itself is host independent.

`../../spec_trace.py` uses Python's standard library and can be invoked by absolute
path from any current directory. It never imports gem5, the old JSONL observer,
or its builder. These small binary fixtures were packed with literal bit shifts,
independently of the production parser. Run `python3 ../../spec_trace_test.py`
from this directory, or invoke that test file by absolute path.

The Discovery fixture has valid-window DELTA values 0, 1, 15, 30, 31 and all
records recover startPC 0x1000. The separate encoding fixture contains signed
DELTA values -64, -1, 0, 31, 63 to exercise the complete encoding range; its
two invalid windows are reported and strictly rejected by CAM selection.
The boundary-discovery/detail fixtures exercise the SPEC legal DELTA=-1
32-bit crossing case, including Detail query_valid=0, and page clipping.
The Detail
fixture has three valid snapshots and one invalid snapshot. Its different ITH,
LHR, Call and Ret components expose mask-order mistakes. Both files end with a
payload-free final marker. The CAM fixture contains the frozen mapping.

## File contracts

Raw traces have no header. A required JSON sidecar declares:

```json
{
  "schema": "hint-spec-trace",
  "version": 1,
  "hash_version": 7,
  "mask_order": ["ITH", "Call", "Ret", "LHR"],
  "kind": "discovery",
  "record_bytes": 7,
  "termination": "final",
  "capacity_records": 6
}
```

Detail uses `kind=detail`, `record_bytes=22`, and requires `cam_sha256`, the SHA256
of the exact H2PHINTP CAM image used during collection. `data_sha256`, `p_error`
(SPEC encoding), `stop_reason`, and workload/checkpoint/slice provenance may be
included. A memory fault is rejected; overflow may lose samples and is reported.
The sidecar is a software file contract; it adds no hardware header fields.

The default `layout=compact` describes compact dumps of complete records. For `termination=final`, the final
record must be the last record, with every field except final zero. For
`termination=capacity`, no final record is allowed and the file must contain
exactly `capacity_records` records. To parse a larger hardware memory dump, first
extract its populated complete-record prefix including final, or its complete
record capacity if no marker fits. Alternatively use the buffer layout below.
The tool never guesses the valid length from zero bytes. Final records are not
branch samples. Zero records remain ordinary valid records unless final is set.

For a complete hardware memory dump, set `layout=buffer` and `buffer_bytes` to
the exact dump length. `capacity_records` must equal floor(buffer_bytes /
record_bytes). The file length must equal buffer_bytes. Parsing stops at a
payload-free final marker and ignores all subsequent padding or stale memory,
including whole stale records with arbitrary contents. If termination=capacity
there must be no final marker within the complete-record capacity; a remaining
partial row at the end is ignored. A 1 MiB Detail buffer therefore has 47662
complete 22-byte rows and 12 unused tail bytes. This requires no external
trimming or observer data. A sidecar that claims final termination without a
marker still fails. Compact layout continues to reject partial tails and all
records after final. The independent discovery-buffer/detail-buffer fixtures
contain nonzero stale whole rows and partial tails after final.

Bit offsets, all little endian:

| Kind | Fields |
| --- | --- |
| Discovery, 7 B | PC>>1 [46:0], signed DELTA [53:47], final [54], reserved [55] |
| Detail, 22 B | pc_id [3:0], target>>1 [50:4], ITH [78:51], LHR [106:79], Call [134:107], Ret [162:135], signed DELTA [169:163], baseline_miss [170], query_valid [171], final [172], reserved [175:173] |

DELTA is independently decoded per record; no predecessor relation is assumed.
Discovery reconstructs startPC = PC - 2 * DELTA; window reports and construction
check canonical sv48 addresses. Exporters must assert the original integer DELTA lies in -64..63
before packing; overflow cannot be detected after an exporter has truncated it.
No Discovery valid bit or implicit skip rule is added. Detail query_valid=0 is
retained in coverage statistics and excluded from mask selection and table
construction. The parser checks bit widths and reserved fields; it cannot prove
that a supplied folded context was sampled at prediction time.

Validation reports invalid_window separately from signed-field decoding and
query_valid. Producers know actual instruction length: the terminal halfword
coordinate C=P for RVC and C=P+2 for a 32-bit control must be in
[Q, min(alignDown(Q+64,32), next4KiBpage)). Raw sourcePC remains actual P.
Therefore DELTA=-1 is legal for the 32-bit crossing case C=Q. Raw has no length
field: the decoder accepts -1 under this producer guarantee and cannot prove
the actual length independently. Other negative deltas are rejected by CAM
selection/build, even for query_valid=0. Nonnegative offsets are accepted if
at least a 2-byte instruction could belong to the window; producers must
validate their known length. Window arithmetic must not wrap. The sidecar may
declare fetch_block_bytes, fetch_align_bytes and page_bytes (defaults64/32/4096).
Snapshot-invalid Detail retains its real DELTA, including -1, and remains
excluded from context training. Sidecars may preserve producer audit counters.

CAM mapping JSON has schema `hint-spec-cam`, version 1, hash_version 7, mask_order,
16 `entries` with explicit fixed id, valid, start_pc (hex address), mask, and
`image_sha256`. Selection ranks descending Discovery count, then ascending
startPC for deterministic ties. These initial ids are then frozen. Build keeps
all valid CAM entries, including entries with no useful Context rows, and never
sorts, compacts or deletes ids. Only the four-bit mask changes. Discovery and
Detail traces may be arbitrarily reordered without changing the built image.

The binary CAM image is 144 bytes, with magic H2PHINTP. The load image is 16784
bytes, magic H2PHINTL. Both have eight zero reserved header bytes. CAM starts at
0x10, Context at 0x90, Slot at 0x4090. Context is set-major, two ways per set.
Slot words have only target[47:1]; there is no legacy Slot valid bit. Image
version/hash contract and integrity checks live in JSON sidecars, never in the
reserved hardware bytes.

## CLI

```sh
python3 spec_trace.py validate --trace discovery.bin --meta discovery.json
python3 spec_trace.py select-cam --trace discovery.bin --meta discovery.json --image cam.bin --output cam.json
python3 spec_trace.py validate --trace detail.bin --meta detail.json --cam cam.json
python3 spec_trace.py rank-masks --trace detail.bin --meta detail.json --cam cam.json --output rank.json
python3 spec_trace.py build --trace detail.bin --meta detail.json --cam cam.json --image load.bin --output load.json
python3 spec_trace.py inspect --kind cam --image cam.bin --meta cam.json
python3 spec_trace.py inspect --kind load --image load.bin --meta load.json
```

`build --masks masks.json` accepts an object mapping every valid frozen CAM id to
a four-bit integer mask, for example `{"0":9}`. Without it, build uses the
per-entry static best mask. `rank.json` reports all 16 masks for every valid CAM
entry and at most three SHA-deduplicated candidate configurations: static-best,
uniform9 (ITH+LHR), uniform15. Each candidate's masks object can be written to an
independent masks.json and fed to build. The independent replay runner must run
these images and choose by measured IPC. Static estimates do not select the
IPC winner, exhaust all mask combinations, or model new Hint-on contexts.

Static scoring counts unordered (key,target,baseline_miss) observations. Gain
for replacing a key with target T is count(T) - baseline_correct_count, equal to
corrected baseline misses minus harmed baseline successes. A deterministic
greedy allocator admits positive-gain mappings with at most two keys per set
and 32 shared target slots. Ties use numeric key and target, so ordering cannot
affect layout. Confidence starts at 3, but no online confidence replay is
attempted. Cross-entry collisions are combined for full-image scoring.

Trace fields are ITH,LHR,Call,Ret; mask bits are ITH,Call,Ret,LHR. Folded 28-bit
components are XORed directly with the SPEC index/tag fold of startPC[47:1].
There is no old fold28 or rotation step.
