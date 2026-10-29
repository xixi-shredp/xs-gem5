#!/usr/bin/env python3
"""Generate a tiny RV64 ELF and run real O3 JALR recognition on ps1/ps2.

Generation and execution must occur on the audited remote host. The program
executes one non-return JALR to its own fallthrough four times, with 64 NOPs
between visits so the first recovery can train the unchanged baseline BTBs.
The first cold visit must recover once; subsequent recognized predictions
append ITH once each without a repeated identification recovery. Values from
production debug observers are checked against a 128-bit history oracle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import time


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def create_elf(path, kind="indirect"):
    base = 0x10000
    probe = base + 0x100
    words = []
    def addi(rd, rs, imm):
        return ((imm & 0xfff) << 20) | (rs << 15) | (rd << 7) | 0x13
    def jal(rd, offset):
        value = offset & ((1 << 21) - 1)
        return (((value >> 20) & 1) << 31) | (((value >> 1) & 0x3ff) << 21) | \
               (((value >> 11) & 1) << 20) | (((value >> 12) & 0xff) << 12) | (rd << 7) | 0x6f
    def bne(rs1, rs2, offset):
        value = offset & 0x1fff
        return (((value >> 12) & 1) << 31) | (((value >> 5) & 0x3f) << 25) | \
               (rs2 << 20) | (rs1 << 15) | (1 << 12) | (((value >> 1) & 0xf) << 8) | \
               (((value >> 11) & 1) << 7) | 0x63
    words += [((base >> 12) << 12) | (2 << 7) | 0x37, addi(2, 2, 0x104),
              addi(6, 0, 4), jal(0, probe - (base + 12))]
    while len(words) * 4 < 0x100:
        words.append(addi(0, 0, 0))
    # jalr x0,x2,0: x2 is neither link register x1 nor x5, so not a return.
    words.append((2 << 15) | 0x67 if kind == "indirect" else jal(1, 4))
    words.extend([addi(0, 0, 0)] * 64)
    words.append(addi(6, 6, -1))
    loop_branch = base + len(words) * 4
    words.append(bne(6, 0, probe - loop_branch))
    words += [addi(10, 0, 0), addi(17, 0, 93), 0x00000073]
    code = struct.pack("<" + "I" * len(words), *words)
    code += struct.pack("<I", addi(0, 0, 0)) * ((4096 - len(code)) // 4)
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 243, 1, base, 64, 0, 0,
                         64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 4096, base, base, len(code), len(code), 4096)
    path.write_bytes(header + phdr + bytes(4096 - len(header) - len(phdr)) + code)
    path.chmod(0o755)
    return {"entry": base, "probe_pc": probe, "actual_target": probe + 4,
            "executions": 4, "nop_delay": 64, "loop_branch": loop_branch,
            "kind": kind,
            "encoding": "jalr x0,x2,0; x2=P+4" if kind == "indirect" else "jal x1,+4",
            "instruction_size": 4, "path_coordinate": probe + 2}


def parse_events(text):
    events = []
    for line in text.splitlines():
        if "HINT_ITH event=" not in line:
            continue
        row = {}
        for key, value in re.findall(r"(\w+)=([^\s]+)", line):
            row[key] = value if key == "event" else int(value, 0)
        row["line"] = line
        events.append(row)
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--remote-alias", choices=("ps1", "ps2"), required=True)
    parser.add_argument("--kind", choices=("indirect", "direct-call"), default="indirect")
    args = parser.parse_args()
    args.gem5 = args.gem5.resolve()
    args.outdir = args.outdir.resolve()
    here = Path(__file__).resolve().parent
    root = here.parents[4]
    mem = next(int(line.split()[1]) * 1024 for line in Path("/proc/meminfo").read_text().splitlines()
               if line.startswith("MemAvailable:"))
    resources = {"affinity_cpus": len(os.sched_getaffinity(0)), "loadavg": os.getloadavg(),
                 "mem_available_bytes": mem}
    if resources["loadavg"][0] >= resources["affinity_cpus"] or mem < 1024 ** 3:
        raise SystemExit(f"Insufficient resources for one O3 probe: {resources}")
    args.outdir.mkdir(parents=True, exist_ok=False)
    elf, image = args.outdir / "jalr-fallthrough.elf", args.outdir / "empty-load.bin"
    program = create_elf(elf, args.kind)
    image.write_bytes(b"H2PHINTL" + bytes(16784 - 8))
    simulation = args.outdir / "gem5"
    command = [args.gem5, "--outdir", simulation, "--debug-flags=DecoupleBP,DecoupleBPHist",
               "--debug-file=history.log", here / "hint_jalr_microprobe_config.py",
               "--elf", elf, "--metadata", image]
    started = time.monotonic()
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    (args.outdir / "stdout").write_text(result.stdout)
    (args.outdir / "stderr").write_text(result.stderr)
    report = {"schema": "hint-real-o3-jalr-microprobe", "version": 1,
              "hostname": socket.gethostname(), "ssh_alias": args.remote_alias,
              "resources_before": resources, "gem5_sha256": sha(args.gem5),
              "elf_sha256": sha(elf), "metadata_sha256": sha(image), "program": program,
              "source_sha256": {str(path.relative_to(root)): sha(path) for path in
                                 (Path(__file__), here / "hint_jalr_microprobe_config.py",
                                  root / "src/cpu/o3/dyn_inst.hh", root / "src/cpu/o3/iew.cc",
                                  root / "src/cpu/pred/btb/decoupled_bpred.cc")},
              "command": list(map(str, command)), "returncode": result.returncode,
              "seconds": round(time.monotonic() - started, 3), "checks": {},
              "scope": "real O3 SE pipeline microprobe; separate from checkpoint/Difftest smoke",
              "young_history_rollback": "covered separately by production helper; natural probe rollback reported below",
              "passed": False}
    output = args.outdir / "validation.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    if result.returncode != 0:
        raise SystemExit(f"gem5 microprobe failed; see {args.outdir / 'stderr'}")
    text = (simulation / "history.log").read_text()
    events = parse_events(text)
    probe = program["probe_pc"]
    target = program["actual_target"]
    marker = "CFI identification recovery" if args.kind == "indirect" else "CALL identification recovery"
    recovery_lines = [line for line in text.splitlines() if marker in line
                      and re.search(rf"pc=0x{probe:x}\b", line)]
    redirects = [row for row in events if row["event"] == "redirect_after" and row["branchpc"] == probe]
    checks = report["checks"]
    branch_class = "indirect_jump" if args.kind == "indirect" else "direct_call"
    stats_text = (simulation / "stats.txt").read_text()
    committed = re.findall(rf"\.branchClassCounts::{branch_class}\s+(\d+)", stats_text)
    report["committed_executions_from_public_stats"] = int(committed[-1]) if committed else None
    checks["public_committed_execution_count"] = bool(committed) and int(committed[-1]) == program["executions"]
    checks["unknown_identification_recovery_exactly_once"] = len(recovery_lines) == 1
    checks["one_actual_taken_recovery"] = len(redirects) == 1 and redirects[0]["taken"] == 1 \
        and redirects[0]["indirect"] == (1 if args.kind == "indirect" else 0) \
        and redirects[0]["ret"] == 0 and redirects[0]["target"] == target
    prefix = "" if args.kind == "indirect" else "call_"
    def state_value(row):
        return row[prefix + "low0"] | (row[prefix + "low1"] << 64)
    def expected_value(old, next_target):
        if args.kind == "indirect":
            return ((old << 2) | ((next_target >> 1) & 3)) & ((1 << 128) - 1)
        word, folded = program["path_coordinate"] >> 1, 0
        while word:
            folded ^= word & 0x3fff
            word >>= 14
        return ((old << 2) ^ folded) & ((1 << 128) - 1)
    transitions = []
    restored_rows = [row for row in events if row["event"] == "redirect_restored" and row["branchpc"] == probe]
    for after in redirects:
        matches = [row for row in restored_rows if row["ftq"] == after["ftq"]]
        if not matches:
            continue
        before = matches[-1]
        old, new = state_value(before), state_value(after)
        expected = expected_value(old, target)
        transitions.append({"ftq": after["ftq"], "writes_before": before[prefix + "writes"],
                            "writes_after": after[prefix + "writes"], "head_before": before[prefix + "head"],
                            "head_after": after[prefix + "head"], "old": hex(old), "actual": hex(new),
                            "expected": hex(expected), "matched": after[prefix + "writes"] == before[prefix + "writes"] + 1
                            and after[prefix + "head"] == (before[prefix + "head"] + 1) % 128 and new == expected})
    checks["unknown_actual_context_appended_once"] = len(transitions) == 1 and transitions[0]["matched"]
    prediction_pairs = []
    pending = {}
    for row in events:
        if row["event"] == "prediction_before":
            pending[row["ftq"]] = row
        elif row["event"] == "prediction_after" and row["ftq"] in pending:
            before = pending.pop(row["ftq"])
            if row["branchpc"] not in (probe, probe + 2) or not row["taken"] or row["ret"] \
                    or row["indirect"] != (1 if args.kind == "indirect" else 0):
                continue
            old, new = state_value(before), state_value(row)
            expected = expected_value(old, row["target"])
            prediction_pairs.append({"ftq": row["ftq"], "target": row["target"],
                                     "writes_before": before[prefix + "writes"], "writes_after": row[prefix + "writes"],
                                     "old": hex(old), "actual": hex(new), "expected": hex(expected),
                                     "matched": row[prefix + "writes"] == before[prefix + "writes"] + 1
                                     and row[prefix + "head"] == (before[prefix + "head"] + 1) % 128 and new == expected})
    checks["known_predictions_append_once"] = bool(prediction_pairs) and all(row["matched"] for row in prediction_pairs)
    checks["no_repeat_identification_for_known_visits"] = len(recovery_lines) == 1 and len(prediction_pairs) >= program["executions"] - 1
    report["recovery_lines"] = recovery_lines
    report["actual_recovery_transitions"] = transitions
    report["known_prediction_transitions"] = prediction_pairs
    report["speculative_prediction_updates_observed"] = len(prediction_pairs)
    report["counting_note"] = "committed branchClassCounts and speculative prediction updates are distinct; no final-ring-writes==loop-count assertion"
    report["natural_ith_rewinds"] = []
    for after in restored_rows:
        before = [row for row in events if row["event"] == "redirect_before" and row["ftq"] == after["ftq"]]
        if before and before[-1]["writes"] > after["writes"]:
            report["natural_ith_rewinds"].append({"ftq": after["ftq"], "before": before[-1]["writes"],
                                                   "restored": after["writes"]})
    report["passed"] = all(checks.values())
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "hostname": report["hostname"],
                      "checks": checks, "report": str(output)}))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
