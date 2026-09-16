#!/usr/bin/env python3
"""Audit finite/unlimited Hint behavior in the final gem5.opt CPU pipeline.

Run only after the final gem5-scons process has exited successfully. This
driver verifies ELF, execute permission, and a required final binary SHA. It
never builds, resumes, or reuses an output directory. Eight positive and four
negative cases run with bounded concurrency; all evidence is kept on failure.
"""

import argparse
from collections import Counter
import concurrent.futures
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import struct
import subprocess

from raw_pipeline_driver import (
    COMMIT,
    REPO,
    TRAP,
    require,
    save,
    sha,
    utc,
    verify_binary,
)
from raw_pipeline_program import (
    BASE,
    addi,
    architectural_check,
    generate,
    make_program,
)
from h2p_hint.audit import (
    compare_observer,
    legacy,
    stats_values,
    validate_hint_stats,
    validate_occupancy,
)


DEBUG_FLAGS = "CommitTrace,RAS,DecoupleBPHist,Override,DecoupleBP,BTB"
PREDICTION = re.compile(
    r"^\s*(\d+):.*Requesting new prediction for PC (0x[0-9a-fA-F]+)", re.M
)
STAGE_FIELDS = re.compile(
    r"(bbStart:.*|BTB entry:.*|condTakens:.*|indirectTarget of .*|returnTarget .*)"
)


def digest(value):
    return hashlib.sha256(
        json.dumps(value, sort_keys=True).encode()
    ).hexdigest()


def ideal_metadata(finite):
    """Translate the same fitting mapping, preserving hash aliases and slots."""
    result = json.loads(json.dumps(finite))
    result.update(version=3, storage="unlimited")
    result["finite_reference"] = {
        key: finite["layout"][key]
        for key in ("cam_entries", "context_entries", "ways", "slot_entries")
    }
    result["layout"] = {
        "conf_queue_depth": finite["layout"]["conf_queue_depth"]
    }
    result["entries"] = [
        {
            "valid": entry["valid"],
            "key": (entry["tag"] << 11) | entry["set"],
            "slot": entry["slot"],
            "conf": entry["conf"],
        }
        for entry in finite["entries"]
    ]
    return result


def prepare_inputs(output):
    assets = output / "inputs"
    program = generate(assets)
    labels = {name: int(pc, 16) for name, pc in program["labels"].items()}
    candidates = json.loads((assets / "candidates.json").read_text())
    # Unreachable PCs are far from every executed/speculatively fetched block.
    # They exercise >20 candidate loading without changing real arbitration.
    extra = [0x90000000 + index * 0x1000 for index in range(24)]
    ideal_candidates = {
        **candidates,
        "version": 3,
        "storage": "unlimited",
        "pcs": candidates["pcs"] + [hex(pc) for pc in extra],
    }
    save(assets / "ideal-candidates.json", ideal_candidates)
    empty = json.loads((assets / "empty-metadata.json").read_text())
    save(assets / "ideal-empty.json", ideal_metadata(empty))

    # A call to fallthrough need not get a taken BTB entry. As in the audited
    # prior stage gate, replace only the two a0 materialization instructions
    # so the existing call really targets the existing returning subroutine.
    materialize_pc = labels["body"] + 4
    difference = labels["subroutine"] - materialize_pc
    upper = (difference + 0x800) >> 12
    patches = {
        materialize_pc: ((upper & 0xFFFFF) << 12) | (10 << 7) | 0x17,
        materialize_pc + 4: addi(10, 10, difference - (upper << 12)),
    }
    image = bytearray((assets / "raw_pipeline.bin").read_bytes())
    for pc, word in patches.items():
        struct.pack_into("<I", image, pc - BASE, word)
    (assets / "taken-call.bin").write_bytes(image)
    taken_program = make_program()
    taken_program.instructions = [
        (pc, patches.get(pc, word), text)
        for pc, word, text in taken_program.instructions
    ]
    patch_record = {
        "original_image_sha256": program["image_sha256"],
        "taken_image_sha256": sha(assets / "taken-call.bin"),
        "patched_instruction_pcs": [hex(pc) for pc in patches],
        "actual_call_target": hex(labels["subroutine"]),
        "architectural_check": architectural_check(taken_program),
    }
    save(assets / "taken-program-change.json", patch_record)
    pc, wrong_target = labels["fallthrough_call"], labels["bad"]
    word = pc >> 1
    key = (word ^ (word >> 28) ^ (word >> 56)) & ((1 << 28) - 1)
    populated = {
        **empty,
        "cam": [{"pc": hex(pc), "mask": 0, "valid": True}],
        "slots": [hex(wrong_target)],
        "entries": [
            {
                "set": key & 2047,
                "way": 0,
                "tag": key >> 11,
                "valid": True,
                "slot": 0,
                "conf": 3,
            }
        ],
        "source": {
            "purpose": "same fitting mapping: S3 wrong target and recovery",
            "taken_image_sha256": patch_record["taken_image_sha256"],
        },
    }
    save(assets / "finite-populated.json", populated)
    save(assets / "ideal-populated.json", ideal_metadata(populated))
    return (
        program,
        patch_record,
        {path.name: sha(path) for path in sorted(assets.iterdir())},
    )


def cases():
    result = {}
    for name, mode, ideal, metadata in (
        ("off", "off", False, None),
        ("finite-collect", "collect", False, None),
        ("finite-empty", "replay", False, "empty-metadata.json"),
        ("ideal-collect", "collect", True, None),
        ("ideal-empty", "replay", True, "ideal-empty.json"),
        ("taken-off", "off", False, None),
        ("finite-populated", "replay", False, "finite-populated.json"),
        ("ideal-populated", "replay", True, "ideal-populated.json"),
    ):
        result[name] = {
            "mode": mode,
            "ideal": ideal,
            "metadata": metadata,
            "fixture": "taken"
            if "populated" in name or name == "taken-off"
            else "raw",
            "candidates": (
                "ideal-candidates.json"
                if ideal and "populated" not in name
                else "candidates.json"
            )
            if mode != "off"
            else None,
        }
    # The populated ideal case uses the same four candidates as its finite
    # peer, translated to v3, to compare all per-PC counters directly.
    result["ideal-populated"]["candidates"] = "ideal-fitting-candidates.json"
    result.update(
        {
            "reject-off": {
                "mode": "off",
                "ideal": True,
                "reject": "requires --h2p-hint-mode=collect or replay",
            },
            "reject-geometry": {
                "mode": "collect",
                "ideal": True,
                "candidates": "ideal-candidates.json",
                "extra": ["--h2p-hint-cam-entries=17"],
                "reject": "does not accept changing --h2p-hint-cam-entries",
            },
            "reject-finite-v3": {
                "mode": "replay",
                "ideal": False,
                "metadata": "ideal-empty.json",
                "reject": "Hint format/storage",
            },
            "reject-ideal-v2": {
                "mode": "replay",
                "ideal": True,
                "metadata": "empty-metadata.json",
                "reject": "Hint format/storage",
            },
        }
    )
    return result


def run_case(name, case, args, experiment):
    out = args.output_root / name
    out.mkdir(exist_ok=False)
    assets = args.output_root / "inputs"
    verify_binary(args.gem5, args.binary_sha256)
    require(
        sha(args.config) == experiment["config_sha256"],
        "configuration changed before run",
    )
    require(
        all(
            sha(assets / name) == value
            for name, value in experiment["input_shas"].items()
        ),
        "an input changed before run",
    )
    image = (
        "taken-call.bin"
        if case.get("fixture") == "taken"
        else "raw_pipeline.bin"
    )
    command = [
        "setarch",
        platform.machine(),
        "-R",
        str(args.gem5),
        "-d",
        str(out),
        f"--debug-flags={DEBUG_FLAGS}",
        "--debug-file=debug.log",
        str(args.config),
        "--disable-difftest",
        "--mem-type=DDR4_2400_8x8",
        "--raw-cpt",
        f"--generic-rv-cpt={assets / image}",
        f"--warmup-insts-no-switch={args.warmup_insts}",
        f"--maxinsts={args.warmup_insts + args.measure_insts}",
        "--h2p-stat",
        f"--h2p-stat-interval={args.interval}",
        f"--h2p-hint-mode={case['mode']}",
    ]
    if case["ideal"]:
        command.append("--ideal-hint-ibp")
    for option in ("candidates", "metadata"):
        if case.get(option):
            command.append(f"--h2p-hint-{option}={assets / case[option]}")
    command.extend(case.get("extra", []))
    (out / "cmd.txt").write_text(shlex.join(command) + "\n")
    save(out / "command.json", command)
    environment = os.environ.copy()
    if args.library_path:
        environment["LD_LIBRARY_PATH"] = args.library_path
    record = {
        "started_utc": utc(),
        "binary_sha256": args.binary_sha256,
        "case": case,
    }
    with (out / "stdout.log").open("w") as stdout, (out / "stderr.log").open(
        "w"
    ) as stderr:
        process = subprocess.Popen(
            command, cwd=REPO, env=environment, stdout=stdout, stderr=stderr
        )
        record["pid"] = process.pid
        save(out / "process.json", record)
        try:
            record["returncode"] = process.wait(timeout=args.timeout_seconds)
        except subprocess.TimeoutExpired:
            process.kill()
            record.update(returncode=process.wait(), timed_out=True)
    record.update(finished_utc=utc(), binary_sha256_after=sha(args.gem5))
    save(out / "process.json", record)
    return name


def prediction_records(debug, pc, target):
    records, observations = [], []
    starts = list(PREDICTION.finditer(debug))
    for index, start in enumerate(starts):
        segment = debug[
            start.end() : starts[index + 1].start()
            if index + 1 < len(starts)
            else None
        ]
        bubble = re.search(
            r"Prediction complete: override bubbles=(\d+)", segment
        )
        require(
            bubble is not None,
            "prediction trace is missing completion/bubble evidence",
        )
        dumps = segment[: bubble.start()].split("dumping FullBTBPrediction")[
            1:
        ]
        require(
            len(dumps) == 5,
            "expected four stage dumps and the selected final prediction",
        )
        stages = [
            [
                match[1]
                for line in stage.splitlines()
                if (match := STAGE_FIELDS.search(line))
            ]
            for stage in dumps
        ]
        require(
            all(
                any(line.startswith("bbStart:") for line in stage)
                and any(line.startswith("returnTarget ") for line in stage)
                for stage in stages
            ),
            "incomplete stage prediction data",
        )
        record = {
            "tick": int(start[1]),
            "block_pc": start[2],
            "stages": stages,
            "bubbles": int(bubble[1]),
        }
        records.append(record)
        needle = f"indirectTarget of {pc:#x}: {target:#x}"
        visible = [any(line == needle for line in stage) for stage in stages]
        if any(visible):
            require(
                visible == [False, False, True, True, True]
                and record["bubbles"] == 2,
                "wrong Hint target was not first visible at S3 with two override bubbles",
            )
            observations.append(
                {
                    "tick": record["tick"],
                    "visible": visible,
                    "bubbles": record["bubbles"],
                }
            )
    require(records, "no actual prediction-stage traces were recorded")
    return records, observations


def audit_case(name, case, output, experiment):
    directory = output / name
    record = {"result": "fail", "errors": []}
    try:
        process = json.loads((directory / "process.json").read_text())
        require(
            not process.get("timed_out"), "native case exceeded its timeout"
        )
        require(
            process["binary_sha256"]
            == process["binary_sha256_after"]
            == experiment["binary_sha256"],
            "binary identity changed during native case",
        )
        logs = (
            (directory / "stdout.log").read_text(errors="replace")
            + "\n"
            + (directory / "stderr.log").read_text(errors="replace")
        )
        if case.get("reject"):
            require(
                process.get("returncode") not in (None, 0),
                "invalid configuration was accepted",
            )
            require(
                case["reject"] in logs,
                "configuration failed for an unexpected reason",
            )
            require(
                "H2P_PHASE" not in logs
                and "a thread reached the max instruction count" not in logs,
                "invalid configuration reached simulation",
            )
            record.update(result="pass", expected_rejection=case["reject"])
            return record
        require(
            process.get("returncode") == 0,
            "native case did not exit successfully",
        )
        require(
            "a thread reached the max instruction count" in logs,
            "instruction-limit exit is missing",
        )
        require(
            not re.search(
                r"\bpanic:|\bfatal:|HIT BAD TRAP|\bOops:", logs, re.I
            ),
            "failure marker in process logs",
        )
        checker = legacy()
        phases = checker.read_phases(logs)
        require(
            "restore_complete" not in phases,
            "raw fixture waited for GCPT restore",
        )
        warm, measure = phases["warmup_complete"], phases["measure_complete"]
        require(
            warm["warmup"] == measure["warmup"] == experiment["warmup_insts"]
            and warm["measure"] == 0
            and measure["measure"] == experiment["measure_insts"],
            "raw warm-up/measure instruction contract differs",
        )
        split, excess = (
            warm["warmup_fusion_split"],
            measure["terminal_fusion_excess"],
        )
        require(
            split in (0, 1)
            and excess in (0, 1)
            and measure["committed"] - warm["committed"]
            == experiment["measure_insts"] + excess - split,
            "measure architectural retirement accounting does not conserve units",
        )
        require(
            checker.read_stat_values(directory / "stats.txt", "simInsts")
            == [float(experiment["measure_insts"])],
            "statistics include warm-up or extra dumps",
        )
        config = (directory / "config.ini").read_text()
        require(
            "instructionPhaseControl=true\n" in config
            and "gcptRestoreBase=0\n" in config,
            "raw phase configuration differs",
        )
        require(
            f"hintMode={case['mode']}\n" in config
            and f"idealHintIBP={str(case['ideal']).lower()}\n" in config
            and "enable_difftest=true" not in config,
            "Hint mode/storage/difftest differs",
        )
        for field, value in (
            ("hintCamEntries", 16),
            ("hintContextEntries", 4096),
            ("hintWays", 2),
            ("hintSlotEntries", 32),
            ("hintConfQueueDepth", 8),
        ):
            require(f"{field}={value}\n" in config, f"unexpected {field}")
        windows = checker.validate_windows(
            directory, experiment["interval"], experiment["measure_insts"]
        )
        h2p = checker.parse_h2p_output(directory / "h2p-indirect.txt")
        labels = {
            key: int(value, 16)
            for key, value in experiment["program"]["labels"].items()
        }
        allowed = {labels["fallthrough_call"], labels["fallthrough_jump"]}
        require(
            set(h2p) <= allowed, "return/direct/failure-path PC entered H2P"
        )
        debug = (directory / "debug.log").read_text(errors="replace")
        commits = [
            (int(found[1]), int(found[2]), int(found[3], 16))
            for line in debug.splitlines()
            if (found := COMMIT.match(line))
        ]
        traps = [
            (int(found[1]), int(found[2]), int(found[3], 16), int(found[4]))
            for line in debug.splitlines()
            if (found := TRAP.match(line))
        ]
        require(
            commits and len(traps) >= 8,
            "insufficient real commit/trap observations",
        )
        visits = Counter(pc for _, _, pc in commits)
        forbidden = {
            labels[key]
            for key in (
                "bad",
                "bad_spin",
                "fail_near_body",
                "fail_near_handler",
            )
        }
        require(
            not visits.keys() & forbidden and not visits[labels["ecall"]],
            "wrong-path/failure sentinel or faulting ecall retired",
        )
        required = (
            "fallthrough_call",
            "fallthrough_jump",
            "not_taken",
            "pretrap_alu",
            "handler",
            "handler_mret",
            "after_ecall",
            "direct_call",
            "return",
            "pop_push",
        )
        require(
            all(visits[labels[label]] for label in required),
            "a required guest path never retired",
        )
        require(
            all(
                last_pc == labels["pretrap_alu"] for _, _, last_pc, _ in traps
            ),
            "trap restored the wrong retired-prefix boundary",
        )
        if case["fixture"] == "raw":
            require(
                max(row[3] for row in traps) >= 3,
                "cold FTQ did not retain all three controls",
            )
        require(
            "IsCall" in debug and "IsRet" in debug,
            "call/return classification evidence is missing",
        )
        predictions, observations = prediction_records(
            debug, labels["fallthrough_call"], labels["bad"]
        )
        hint = None
        transitions = []
        if case["mode"] == "off":
            require(
                not any(
                    (directory / file).exists()
                    for file in (
                        "hint-stats.json",
                        "hint-profile.jsonl.gz",
                        "conf0.txt",
                    )
                ),
                "off mode created Hint files",
            )
        else:
            assets = output / "inputs"
            candidates = json.loads((assets / case["candidates"]).read_text())[
                "pcs"
            ]
            hint = validate_hint_stats(
                json.loads((directory / "hint-stats.json").read_text()),
                sum(value[0] for value in h2p.values()),
                case["mode"],
                [int(pc, 16) for pc in candidates],
                case["ideal"],
            )
            require(
                hint["storage"]
                == ("unlimited" if case["ideal"] else "finite"),
                "statistics storage identity differs",
            )
            clock = re.search(
                r"\[system\.cpu_clk_domain\][^\[]*\nclock=(\d+)", config
            )
            require(clock is not None, "CPU clock period is missing")
            cycles = measure["tick"] // int(clock[1]) - warm["tick"] // int(
                clock[1]
            )
            metadata = (
                json.loads((assets / case["metadata"]).read_text())
                if case.get("metadata")
                else None
            )
            validate_occupancy(hint, case["mode"], metadata, cycles)
            transitions = [
                json.loads(line)
                for line in (directory / "conf0.txt").read_text().splitlines()
            ]
            require(
                len(transitions) == hint["queue"]["zero_transitions"],
                "conf0 count differs from transitions",
            )
            populated = "populated" in name
            if populated:
                require(
                    observations
                    and hint["global"]["overrides"] > 0
                    and transitions,
                    "populated table never exercised measured overrides and confidence zero",
                )
                require(
                    hint["global"]["harmed"] > 0
                    and hint["global"]["tag_hits"]
                    > hint["global"]["conf_enabled"],
                    "wrong override or disabled shadow prediction was not observed",
                )
                retired = {
                    sequence: (tick, pc) for tick, sequence, pc in commits
                }
                for row in transitions:
                    require(
                        int(row["pc"], 16) == labels["fallthrough_call"]
                        and row["mask"] == 0
                        and int(row["actual_target"], 16)
                        == labels["subroutine"]
                        and int(row["predicted_target"], 16) == labels["bad"]
                        and row["query_cycle"]
                        <= row["commit_cycle"]
                        < row["writeback_cycle"],
                        "conf0 lost actual/prediction target or timing provenance",
                    )
                    require(
                        row["seq"] in retired
                        and retired[row["seq"]][1]
                        == labels["fallthrough_call"]
                        and retired[row["seq"]][0] // int(clock[1])
                        == row["commit_cycle"]
                        and warm["tick"] // int(clock[1])
                        <= row["commit_cycle"]
                        < row["writeback_cycle"]
                        <= measure["tick"] // int(clock[1]),
                        "conf0 did not score a measured committed branch within the run",
                    )
            else:
                require(
                    hint["global"]["overrides"]
                    == hint["queue"]["enqueued"]
                    == 0
                    and not transitions
                    and not observations,
                    "observer/empty table changed prediction",
                )
            if case["mode"] == "collect":
                with gzip.open(
                    directory / "hint-profile.jsonl.gz", "rt"
                ) as source:
                    rows = [json.loads(line) for line in source]
                header, branches = rows[0], rows[1:]
                require(
                    header["kind"] == "header"
                    and header["version"] == (3 if case["ideal"] else 2)
                    and header["pcs"] == candidates,
                    "profile header identity differs",
                )
                require(
                    all(row["kind"] == "branch" for row in branches),
                    "unexpected collect record",
                )
                observed = Counter(int(row["pc"], 16) for row in branches)
                require(
                    set(observed) == allowed
                    and any(row["contexts"] is not None for row in branches),
                    "collect did not observe both indirects with prediction-time context",
                )
                for pc in allowed:
                    expected = next(
                        row["executions"]
                        for row in hint["pcs"]
                        if int(row["pc"], 16) == pc
                    )
                    require(
                        observed[pc] == expected,
                        "collect execution count differs from profile",
                    )
                save(directory / "profile-records.json", branches)
        evidence = {
            "commits": commits,
            "traps": traps,
            "predictions": predictions,
            "stage_override_observations": observations,
            "conf0": transitions,
        }
        save(directory / "pipeline-evidence.json", evidence)
        numeric = stats_values(directory / "stats.txt")
        record.update(
            result="pass",
            phases=phases,
            window_count=windows,
            h2p_misses=sum(value[0] for value in h2p.values()),
            trap_count=len(traps),
            commit_trace_sha256=digest(commits),
            retired_pc_sha256=digest([pc for _, _, pc in commits]),
            prediction_trace_sha256=digest(predictions),
            stage_override_count=len(observations),
            numeric_statistics=len(numeric),
            hint=hint,
        )
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        StopIteration,
        IndexError,
    ) as error:
        record["errors"].append(str(error))
    finally:
        save(directory / "pipeline-status.json", record)
    return record


def audit(output):
    experiment = json.loads((output / "experiment.json").read_text())
    assets = output / "inputs"
    require(
        all(
            sha(assets / name) == value
            for name, value in experiment["input_shas"].items()
        ),
        "input identity changed since launch",
    )
    audited = {
        name: audit_case(name, case, output, experiment)
        for name, case in experiment["cases"].items()
    }
    comparisons = {}
    pairs = [
        ("off", name)
        for name in (
            "finite-collect",
            "finite-empty",
            "ideal-collect",
            "ideal-empty",
        )
    ]
    pairs.append(("finite-populated", "ideal-populated"))
    for left, right in pairs:
        if (
            audited[left]["result"] != "pass"
            or audited[right]["result"] != "pass"
        ):
            continue
        comparison = compare_observer(output / left, output / right)
        for field in (
            "commit_trace_sha256",
            "prediction_trace_sha256",
            "phases",
        ):
            comparison[field + "_equal"] = (
                audited[left][field] == audited[right][field]
            )
        comparison["windows_equal"] = all(
            (output / left / "h2p-indirec" / f"{offset}.txt").read_bytes()
            == (output / right / "h2p-indirec" / f"{offset}.txt").read_bytes()
            for offset in range(
                0, experiment["measure_insts"], experiment["interval"]
            )
        )
        comparison["pass"] &= all(
            value
            for key, value in comparison.items()
            if key.endswith("_equal")
        )
        comparisons[left + ":" + right] = comparison
    profile_equal = False
    if all(
        audited[name]["result"] == "pass"
        for name in ("finite-collect", "ideal-collect")
    ):
        profile_equal = (
            output / "finite-collect/profile-records.json"
        ).read_bytes() == (
            output / "ideal-collect/profile-records.json"
        ).read_bytes()
    recovery = {
        name: audited[name].get("retired_pc_sha256")
        == audited["taken-off"].get("retired_pc_sha256")
        for name in ("finite-populated", "ideal-populated")
    }
    queue_equal = False
    conf0_equal = False
    if all(
        audited[name]["result"] == "pass"
        for name in ("finite-populated", "ideal-populated")
    ):
        finite, ideal = (
            audited[name]["hint"]
            for name in ("finite-populated", "ideal-populated")
        )
        queue_equal = all(
            finite[key] == ideal[key]
            for key in ("global", "pcs", "query", "queue", "occupancy")
        )
        left = json.loads(
            (output / "finite-populated/pipeline-evidence.json").read_text()
        )["conf0"]
        right = json.loads(
            (output / "ideal-populated/pipeline-evidence.json").read_text()
        )["conf0"]
        # Storage location encodings differ; query, targets and update timing
        # must be identical. Entry ID and full key are ideal diagnostics only.
        conf0_equal = [
            {
                key: value
                for key, value in row.items()
                if key not in ("set", "way", "entry_id", "key")
            }
            for row in left
        ] == [
            {
                key: value
                for key, value in row.items()
                if key not in ("set", "way", "entry_id", "key")
            }
            for row in right
        ]
    passed = (
        all(row["result"] == "pass" for row in audited.values())
        and len(comparisons) == len(pairs)
        and all(row["pass"] for row in comparisons.values())
        and profile_equal
        and all(recovery.values())
        and queue_equal
        and conf0_equal
    )
    result = {
        "format": "h2p-hint-ideal-native-pipeline-audit",
        "version": 1,
        "result": "pass" if passed else "fail",
        "audited_utc": utc(),
        "binary_sha256": experiment["binary_sha256"],
        "cases": audited,
        "comparisons": comparisons,
        "shared_pc_profile_records_equal": profile_equal,
        "wrong_target_retired_pc_sequence_equal": recovery,
        "populated_all_hint_counters_equal": queue_equal,
        "populated_conf0_provenance_equal": conf0_equal,
    }
    save(output / "audit.json", result)
    print(
        json.dumps(
            {
                "result": result["result"],
                "audit": str(output / "audit.json"),
                "case_errors": {
                    name: row["errors"]
                    for name, row in audited.items()
                    if row["errors"]
                },
                "comparison_failures": [
                    name
                    for name, row in comparisons.items()
                    if not row["pass"]
                ],
            },
            indent=2,
        )
    )
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=REPO / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--config", type=Path, default=REPO / "configs/example/kmhv3.py"
    )
    parser.add_argument("--binary-sha256")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--library-path", default="")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--warmup-insts", type=int, default=500)
    parser.add_argument("--measure-insts", type=int, default=1500)
    parser.add_argument("--interval", type=int, default=500)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument("--audit-only", action="store_true")
    args = parser.parse_args()
    args.output_root, args.gem5, args.config = (
        path.resolve() for path in (args.output_root, args.gem5, args.config)
    )
    if args.audit_only:
        return 0 if audit(args.output_root) else 1
    if not args.binary_sha256 or not re.fullmatch(
        r"[0-9a-f]{64}", args.binary_sha256
    ):
        parser.error(
            "--binary-sha256 must identify the fully completed final gem5 build"
        )
    if (
        not 1 <= args.jobs <= 25
        or min(
            args.warmup_insts,
            args.measure_insts,
            args.interval,
            args.timeout_seconds,
        )
        <= 0
    ):
        parser.error(
            "jobs must be 1–25; warmup, measure, interval and timeout must be positive"
        )
    verify_binary(args.gem5, args.binary_sha256)
    require(args.config.is_file(), "configuration file is missing")
    args.output_root.mkdir(parents=True, exist_ok=False)
    program, patched, input_shas = prepare_inputs(args.output_root)
    assets = args.output_root / "inputs"
    candidates = json.loads((assets / "candidates.json").read_text())
    save(
        assets / "ideal-fitting-candidates.json",
        {**candidates, "version": 3, "storage": "unlimited"},
    )
    input_shas["ideal-fitting-candidates.json"] = sha(
        assets / "ideal-fitting-candidates.json"
    )
    experiment = {
        "format": "h2p-hint-ideal-native-pipeline-experiment",
        "version": 1,
        "created_utc": utc(),
        "binary": str(args.gem5),
        "binary_sha256": args.binary_sha256,
        "config": str(args.config),
        "config_sha256": sha(args.config),
        "driver_sha256": sha(__file__),
        "input_shas": input_shas,
        "raw_driver_sha256": sha(
            Path(__file__).with_name("raw_pipeline_driver.py")
        ),
        "generator_sha256": sha(
            Path(__file__).with_name("raw_pipeline_program.py")
        ),
        "warmup_insts": args.warmup_insts,
        "measure_insts": args.measure_insts,
        "interval": args.interval,
        "jobs": args.jobs,
        "timeout_seconds": args.timeout_seconds,
        "debug_flags": DEBUG_FLAGS,
        "program": program,
        "taken_fixture": patched,
        "cases": cases(),
    }
    save(args.output_root / "experiment.json", experiment)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(run_case, name, case, args, experiment): name
            for name, case in experiment["cases"].items()
        }
        failures = []
        for future in concurrent.futures.as_completed(futures):
            try:
                print(f"native case completed: {future.result()}", flush=True)
            except Exception as error:
                failures.append({"case": futures[future], "error": str(error)})
        if failures:
            save(args.output_root / "launch-errors.json", failures)
    verify_binary(args.gem5, args.binary_sha256)
    return 0 if audit(args.output_root) else 1


if __name__ == "__main__":
    raise SystemExit(main())
