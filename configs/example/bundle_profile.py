"""Collect guest-started Bundle profiling through its real final WriteResp.

Uses the ordinary BackendV2 machine with an independent profiling driver
until guest profiling Started/Busy clear. Its statistics are profiling-only;
this entry must not be used to report the 1M/1M performance experiment.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import sys

import m5
from m5.util import fatal


if __name__ == "__m5_main__":
    extra = argparse.ArgumentParser(add_help=False)
    extra.add_argument(
        "--bundle-profile-kind", choices=("discovery", "detail"), required=True
    )
    extra.add_argument("--bundle-profile-tail-ticks", type=int, default=100_000_000_000)
    extra.add_argument("--bundle-profile-quantum-ticks", type=int, default=100_000_000)
    knobs, remaining = extra.parse_known_args(sys.argv[1:])
    if (
        knobs.bundle_profile_tail_ticks <= 0
        or not 0 < knobs.bundle_profile_quantum_ticks <= knobs.bundle_profile_tail_ticks
    ):
        fatal("Bundle profile tail and polling quantum must be positive and bounded")
    sys.argv = [sys.argv[0]] + remaining
    state = runpy.run_path(
        str(Path(__file__).with_name("backend_v2_norvv_nol3.py")),
        run_name="bundle_backend_definitions",
    )
    m5.ticks.setGlobalFrequency(state["TICK_FREQUENCY_HZ"])
    args = state["xiangshan_system_init"]()
    state["lock_arguments"](args)
    if not args.h2p_hint_bundle:
        fatal("Bundle profiling requires one guest-controlled Bundle CPU")
    system = state["build_xiangshan_system"](args)
    state["set_backend_v2_params"](args, system)
    cpu = system.cpu[0]
    # A performance limit permanently closes the Core retirement window.
    # Profiling instead stops at the guest's real Started/Busy completion;
    # its retired budget is controlled exclusively by the guest CSR.
    requested_instruction_limit = args.maxinsts
    args.maxinsts = None
    cpu.max_insts_any_thread = 0
    cpu.measureInstCount = 0
    root = state["Root"](full_system=True, system=system)
    state["Simulation"].run_vanilla(args, root, system, None, initialize_only=True)
    if not cpu.instructionPhaseControl:
        fatal("Bundle profile requires verified restore/instruction phase accounting")
    begin = m5.curTick()
    limit = begin + knobs.bundle_profile_tail_ticks
    if args.abs_max_tick:
        limit = min(limit, args.abs_max_tick)
    if args.maxtime:
        limit = min(limit, m5.ticks.fromSeconds(args.maxtime))
    status = cpu.readHintBundleCsr(0x5C0)
    started_busy = (7 << 59) | (1 << 50)
    profiling_seen = False
    events = []
    while not profiling_seen or status & started_busy:
        if m5.curTick() >= limit:
            fatal("Bundle profiling timed out before real final DMA drain")
        event = m5.simulate(
            min(knobs.bundle_profile_quantum_ticks, limit - m5.curTick())
        )
        cause, code = event.getCause(), event.getCode()
        status = cpu.readHintBundleCsr(0x5C0)
        if status & (1 << 62):
            fatal("Bundle profile entry cannot collect replay/Enabled mode")
        profiling_seen |= bool(status & (3 << 60)) or cpu.hintBundleTraceWritten() > 0
        if cause != "simulate() limit reached" or code:
            events.append({"tick": m5.curTick(), "cause": cause, "code": code})
            if (
                code
                or cause != "m5_exit instruction encountered"
                or status & started_busy
            ):
                fatal(
                    "Unexpected guest exit while waiting for Bundle profiling drain: %s",
                    cause,
                )
    if not status & (1 << 63) or (status >> 52) & 7:
        fatal("Bundle profile finished with invalid status or load error")
    profile_error = (status >> 56) & 7
    if profile_error >= 3:
        fatal("Bundle profile finished with fatal profiling error %d", profile_error)
    records = cpu.hintBundleTraceWritten()
    if not records:
        fatal("Bundle profile produced no completed Trace records")
    base = cpu.readHintBundleCsr(0x5C1) << 20
    path = Path(m5.options.outdir) / "bundle-trace.bin"
    digest = hashlib.sha256()
    with path.open("xb") as stream:
        for offset in range(0, records * 22, 1 << 20):
            data = bytes(
                cpu.readHintBundleTrace(
                    base + offset, min(1 << 20, records * 22 - offset)
                )
            )
            stream.write(data)
            digest.update(data)
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "util/h2p_hint"))
    import bundle_tools

    rows = bundle_tools.decode_trace(
        path, knobs.bundle_profile_kind, profile_error, ((status >> 15) & 4095) << 20
    )
    counts = {"boundary": 0, "sample": 0}
    for row in rows:
        counts["boundary" if row["is_boundary"] else "sample"] += 1
    report = {
        "schema": "bundle-profile-memory-export/v1",
        "status": hex(status),
        "profiling_error": profile_error,
        "load_error": 0,
        "profile_driver_start_tick": begin,
        "dma_drained_tick": m5.curTick(),
        "elapsed_sim_ticks": m5.curTick() - begin,
        "completed_records": records,
        "trace": {
            "path": str(path.resolve()),
            "bytes": records * 22,
            "sha256": digest.hexdigest(),
        },
        "events": events,
        "guest_csr_budget_million": status & 4095,
        "kind": knobs.bundle_profile_kind,
        "decoded_rows": counts,
        "performance_instruction_limit_applied": False,
        "performance_entry_limit_argument_ignored": requested_instruction_limit,
        "trace_final_or_full_capacity_validated": True,
        "scope": "Independent guest-started profiling including startup prefix. Host reads completed NC memory only; no table loading or DMA completion bypass. Not performance IPC.",
    }
    (Path(m5.options.outdir) / "bundle-profile.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print("BUNDLE_PROFILE_DRAIN_PASS " + json.dumps(report), flush=True)
