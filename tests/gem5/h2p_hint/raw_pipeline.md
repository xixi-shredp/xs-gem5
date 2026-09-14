# Raw CPU pipeline gate

`raw_pipeline_driver.py` runs the final `gem5.opt` with the real `kmhv3.py`
configuration in `off`, `collect`, and empty-table `replay` modes. This test is
separate from the component driver: it exercises retirement, FTQ attribution,
trap redirection, predictor history recovery, and RAS activity in the CPU.

Run on ICT186 after `gem5-scons` has exited successfully. Use the SHA of that
completed binary and a fresh output directory; the driver refuses to reuse an
existing directory. The three cases consume three of the shared 25 compute
slots. It neither builds gem5 nor modifies an existing experiment.

```sh
python3 tests/gem5/h2p_hint/raw_pipeline_driver.py \
  --gem5 build/RISCV/gem5.opt \
  --binary-sha256 <FINAL_BINARY_SHA256> \
  --output-root <FRESH_GATE_DIRECTORY> \
  --jobs 3 --warmup-insts 500 --measure-insts 1500 --interval 500
```

If required by the build environment, add `--library-path <LD_LIBRARY_PATH>`.
The default test is longer: 2,000 warm-up and 10,000 measure instructions, with
1,000-instruction windows. `--audit-only --output-root <EXISTING_DIRECTORY>`
rechecks saved results without starting simulations. The driver disables ASLR
with `setarch -R` consistently in all cases and records the exact command.

## Program and expected evidence

`raw_pipeline_program.py` generates the raw image without a cross compiler,
plus readable assembly, labels, candidate/empty-table JSON and a manifest. Its
small RV64 interpreter checks this specific guest program before simulation;
it is not a simulator or predictor reference model.

The body begins at `0x80000100`. The first indirect call at `0x8000010c` and
indirect jump at `0x80000118` both take their fall-through addresses. They are
followed by a not-taken conditional, an ordinary ALU instruction at
`0x80000120`, and `ecall` at `0x80000124`. The M-mode handler is far away at
`0x80002000`; it checks `mcause`, advances `mepc` and returns with `mret`.
The resumed body checks that the handler ran once, performs a direct call and
ordinary return, then executes a RISC-V call/return pop-and-push instruction.
Failure paths enter a distinct sentinel loop. No stack, UART, or external
runtime is required.

The audit requires at least eight actual traps and a cold FTQ containing at
least three retired controls. Every recorded trap recovery must use the
ordinary ALU's PC as its inclusive retired boundary. Required guest labels
must retire, the faulting `ecall` must not retire, and no failure path may
retire. `CommitTrace`, `RAS`, and `DecoupleBPHist` provide the evidence. The
driver compares the complete `(tick, sequence number, PC)` retirement trace
across the three modes, as well as all non-host numeric statistics excluding
Hint's own counters and all H2P aggregate/window contents.

It also checks raw phase configuration (no GCPT restore wait), exact warm-up
and measure counts, normal instruction-limit exit, window/target count
conservation, return exclusion, collect profile coverage, and zero overrides
or confidence updates for collect and the empty replay image.

## Artifacts and interpretation

`experiment.json` fixes binary, config, generator, driver and image identities.
Each case preserves command/process records, stdout/stderr, debug trace,
gem5 statistics and Hint outputs. `raw_pipeline_evidence.json` contains trap
tuples, label retirement counts and a retirement-trace digest;
`raw_pipeline_status.json` and the root `audit.json` record audit results.
Failed runs remain available for inspection and are never silently rerun.

The generated image SHA256 is
`1dcf6f222e76862464263713a017a349397046432a290cc8942483d9c1be02b0`.
The local architectural check completed 20,000 instructions, including 769
traps, with a zero failure sentinel. This is preparation evidence only;
actual CPU pipeline acceptance requires `audit.json` from the final ICT186
binary.

This gate does not claim to validate every memory/control squash, populated
table override, or confidence queue condition. Those have separate component
and workload gates. Legacy non-speculative RAS training remains organized by
the FTQ's final branch; the new test records real RAS behavior without claiming
to expand that training algorithm.
