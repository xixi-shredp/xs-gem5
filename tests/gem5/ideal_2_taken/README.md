# Ideal 2-taken validation

`config_smoke.py` is registered with the gem5 test framework by
`test_ideal_2_taken.py`. It checks both the default-disabled parameter value
and explicit `--ideal-2-taken` enablement using the generated
`DecoupledBPUWithBTB` SimObject.

After an ideal 2-taken simulation, validate the scheduling statistics with:

```sh
python3 tests/gem5/ideal_2_taken/check_stats.py \
    m5out/stats.txt --require-second-slot --require-two-enqueues
```

The checker rejects any nonzero distribution bucket above two and any
overflow of the query, enqueue, or taken-per-cycle distributions. It also
checks that second-slot queries do not exceed total queries or sampled cycles,
and that every two-query cycle can be accounted for by a second-slot query.
The two optional requirements make a branch-heavy validation workload prove
that the second lookup and a two-block enqueue cycle were exercised.

`run_cpython_ab.py` runs a resumable, paired baseline/ideal experiment over a
published CPython workload list. It records the binary, configuration,
workload-list, checkpoint metadata, Difftest reference, exact command, and
per-run status. `analyze_cpython_ab.py` requires every pair to pass, applies
the published SimPoint weights, and emits per-slice and per-benchmark CSV,
JSON, and Markdown summaries. `audit_cpython_ab.py` independently checks all
status identities and A/B pairs, the 50M instruction window and exit cause,
the generated SimObject flag, panic/oops absence, and ideal-mode scheduling
statistics for every slice.

On ICT186, `run_cpython_ab_ict186.sh` fixes the agreed 50M interval, eight-way
parallelism, paths, and consistent Difftest-disabled setting. It records batch
and analysis exit codes and invokes the analyzer only after every run passes.
