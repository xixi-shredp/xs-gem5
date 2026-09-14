# ITTAGE H2P validation

The gem5 test registrations in `test_h2p_stat.py` exercise the generated
`DecoupledBPUWithBTB.h2pStat` parameter, its default and CLI enablement, and
the single-CPU/predictor restrictions shared with the Xiangshan configuration.

Validate an output file after a simulation with:

```sh
python3 tests/gem5/h2p_stat/check_h2p_output.py \
    m5out/h2p-indirect.txt
```

The checker accepts an empty file and otherwise requires positive miss counts,
strictly increasing PCs, at least one target per record, and unique increasing
targets. Its focused Python tests can be run without a gem5 binary:

```sh
python3 tests/gem5/h2p_stat/check_h2p_output_test.py
```

Interval extension (run with the final completed gem5.opt build):

```sh
build/RISCV/gem5.opt -d /tmp/h2p-selftest-new tests/gem5/h2p_stat/stats_selftest.py
build/RISCV/gem5.opt tests/gem5/h2p_stat/config_smoke.py --h2p-stat --h2p-stat-interval=1000
build/RISCV/gem5.opt tests/gem5/h2p_stat/config_smoke.py --h2p-stat-interval=1000 --expect-rejection
build/RISCV/gem5.opt tests/gem5/h2p_stat/config_smoke.py --h2p-stat-interval=-1 --expect-rejection
python3 tests/gem5/h2p_stat/interval_tools_test.py
```

The TEST_ONLY SimObject invokes the production helper with controlled prediction
metadata and retired records. It is a white-box helper test, not evidence of CPU
restore detection. Use the same GCPT slice with kmhv3, `--disable-difftest`,
`--mem-type=DDR4_2400_8x8`, `--warmup-insts-no-switch=10000`,
`--maxinsts=12500` for CPU lifecycle checks, in fresh output directories:
H2P off; H2P on; H2P on plus `--h2p-stat-interval=1000`. No `--raw-cpt`.
Check three H2P_PHASE markers, exact 10000/2500 phase deltas, one measure stats
section and identical key simulated stats across modes. Windows must be 0.txt,
1000.txt and 2000.txt, with final tail length 500. Repeat with maxinsts=12000 to
check exactly two windows and no empty tail. A malformed GCPT restorer must fail
before warm-up. Production batch uses 100M + 50M, 1M windows and 24 workers.

`run-cpython-h2p.py` validates per-target miss sums and summed windows against the
full output. Zero-count targets can be absent from the window union because
miss-free PCs are omitted in individual windows; positive counts must match.
