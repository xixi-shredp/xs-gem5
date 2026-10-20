"""Exercise the final gem5 binary's production Hint input validator.

Run only after the build has finished. The output root must be fresh. This
script generates test inputs and invokes the in-binary load case; it does not
compile anything or use a replacement parser as its acceptance oracle.
"""
import argparse
import copy
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--good-metadata", type=Path, required=True)
    parser.add_argument("--good-candidates", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--ideal-hint-ibp", action="store_true")
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=False)
    good = json.loads(args.good_metadata.read_text())
    candidates = json.loads(args.good_candidates.read_text())
    cases = []

    def metadata_case(name, mutation, valid=False):
        data = copy.deepcopy(good)
        mutation(data)
        cases.append((name, json.dumps(data), json.dumps(candidates), valid))

    def candidate_case(name, mutation, valid=False):
        data = copy.deepcopy(candidates)
        mutation(data)
        cases.append((name, json.dumps(good), json.dumps(data), valid))

    metadata_case("valid", lambda data: None, True)
    metadata_case(
        "empty", lambda data: data.update(cam=[], slots=[], entries=[]), True
    )
    metadata_case(
        "invalid_bit",
        lambda data: data["entries"][0].update(valid=False, conf=0),
        True,
    )
    metadata_case("version", lambda data: data.update(version=1))
    metadata_case("hash_version", lambda data: data.update(hash_version=1))
    if args.ideal_hint_ibp:
        metadata_case("ghr_length", lambda data: data.update(ghr_bits=971))
    else:
        metadata_case("legacy_v3", lambda data: data.update(version=3, hash_version=3))
        metadata_case("legacy_v5", lambda data: data.update(version=5, hash_version=5))
        metadata_case("legacy_v6", lambda data: data.update(version=6, hash_version=6))
        candidate_case("old_candidate_v5", lambda data: data.update(version=5))
        candidate_case("old_candidate_v6", lambda data: data.update(version=6))
        metadata_case("missing_encoding", lambda data: data.pop("context_encoding"))
        metadata_case("wrong_encoding", lambda data: data.update(context_encoding="rtl-v4"))
        metadata_case("old_ith64", lambda data: data["history"].update(ith_use_bits=64))
        metadata_case("old_lhr16_physical", lambda data: data["history"].update(lhr_bits=16))
        metadata_case("old_lhr8_read", lambda data: data["history"].update(lhr_use_bits=8))
        metadata_case("old_lhr_head4", lambda data: data["history"].update(lhr_head_bits=4))
        metadata_case("missing_lhr_head", lambda data: data["history"].pop("lhr_head_bits"))
        metadata_case("old_inflight4", lambda data: data["layout"].update(lhr_max_uncommitted=4))
        candidate_case("old_candidate_v3", lambda data: data.update(version=3))
        candidate_case("missing_candidate_encoding", lambda data: data.pop("context_encoding"))
    metadata_case("mask_order", lambda data: data["mask_order"].reverse())
    metadata_case(
        "history_length", lambda data: data["history"].update(call_pcs=65)
    )
    metadata_case(
        "target_encoding", lambda data: data["history"].update(target_shift=2)
    )
    if args.ideal_hint_ibp:
        metadata_case(
            "layout", lambda data: data["layout"].update(conf_queue_depth=9)
        )
        metadata_case(
            "finite_geometry_in_ideal",
            lambda data: data["layout"].update(ways=2),
        )
        metadata_case(
            "finite_reference",
            lambda data: data["finite_reference"].update(ways=4),
        )
        metadata_case(
            "missing_finite_reference",
            lambda data: data.pop("finite_reference"),
        )
        metadata_case("storage", lambda data: data.update(storage="finite"))
        metadata_case("missing_storage", lambda data: data.pop("storage"))
        metadata_case("finite_version", lambda data: data.update(version=2))
    else:
        metadata_case("layout", lambda data: data["layout"].update(ways=4))
        metadata_case(
            "unlimited_version",
            lambda data: data.update(version=3, storage="unlimited"),
        )
    metadata_case("mask_range", lambda data: data["cam"][0].update(mask=32))
    metadata_case("negative_mask", lambda data: data["cam"][0].update(mask=-1))
    metadata_case(
        "fractional_mask", lambda data: data["cam"][0].update(mask=1.5)
    )
    metadata_case("quoted_mask", lambda data: data["cam"][0].update(mask="1"))
    metadata_case(
        "pc_alignment", lambda data: data["cam"][0].update(pc="0x1001")
    )
    metadata_case(
        "pc_canonical",
        lambda data: data["cam"][0].update(pc="0x1000000000000"),
    )
    metadata_case(
        "duplicate_pc", lambda data: data["cam"].append(data["cam"][0])
    )
    metadata_case(
        "cam_capacity",
        lambda data: data.update(
            cam=[
                {"pc": hex(0x1000 + index * 2), "mask": 0, "valid": True}
                for index in range(17)
            ]
        ),
        args.ideal_hint_ibp,
    )
    metadata_case(
        "slot_capacity",
        lambda data: data.update(
            slots=[hex(0x4000 + index * 2) for index in range(33)]
        ),
        args.ideal_hint_ibp,
    )
    metadata_case(
        "duplicate_slot", lambda data: data["slots"].append(data["slots"][0])
    )
    metadata_case(
        "target_alignment", lambda data: data["slots"].__setitem__(0, "0x4001")
    )
    metadata_case(
        "unloaded_slot", lambda data: data["entries"][0].update(slot=31)
    )
    if args.ideal_hint_ibp:
        metadata_case(
            "key_width", lambda data: data["entries"][0].update(key=1 << 28)
        )
        metadata_case(
            "key_negative", lambda data: data["entries"][0].update(key=-1)
        )
        metadata_case(
            "key_quoted", lambda data: data["entries"][0].update(key="1")
        )
        metadata_case(
            "duplicate_key",
            lambda data: data["entries"].append(data["entries"][0]),
        )
        metadata_case(
            "duplicate_invalid_key",
            lambda data: data["entries"].append(
                dict(data["entries"][0], valid=False, conf=0)
            ),
        )
        metadata_case(
            "extra_full_pc_field",
            lambda data: data["entries"][0].update(pc="0x1000"),
        )
        metadata_case(
            "slot_integer_width",
            lambda data: data["entries"][0].update(slot=1 << 64),
        )
        metadata_case(
            "invalid_unloaded_slot",
            lambda data: data["entries"][0].update(
                valid=False, slot=(1 << 64) - 1, conf=0
            ),
            True,
        )
        metadata_case(
            "context_capacity",
            lambda data: data.update(
                entries=[
                    {"valid": True, "key": index * 2048, "slot": 0, "conf": 3}
                    for index in range(4097)
                ]
            ),
            True,
        )
    else:
        metadata_case(
            "slot_pointer_width",
            lambda data: data["entries"][0].update(slot=32),
        )
        metadata_case(
            "tag_width", lambda data: data["entries"][0].update(tag=1 << 17)
        )
        metadata_case(
            "set_range", lambda data: data["entries"][0].update(set=2048)
        )
        metadata_case(
            "way_range", lambda data: data["entries"][0].update(way=2)
        )
        metadata_case(
            "duplicate_position",
            lambda data: data["entries"].append(data["entries"][0]),
        )
        metadata_case(
            "duplicate_tag",
            lambda data: data["entries"][1].update(
                tag=data["entries"][0]["tag"]
            ),
        )
    metadata_case(
        "initial_confidence", lambda data: data["entries"][0].update(conf=0)
    )
    metadata_case(
        "confidence_width", lambda data: data["entries"][0].update(conf=4)
    )
    metadata_case(
        "invalid_boolean", lambda data: data["entries"][0].update(valid=1)
    )
    metadata_case(
        "quoted_boolean", lambda data: data["entries"][0].update(valid="true")
    )
    metadata_case("cam_object", lambda data: data.update(cam={}))
    metadata_case("slots_object", lambda data: data.update(slots={}))
    metadata_case("source_array", lambda data: data.update(source=[]))
    metadata_case("missing_history", lambda data: data.pop("history"))
    metadata_case("missing_source", lambda data: data.pop("source"))
    cases.append(
        (
            "duplicate_field",
            '{"version":2,' + json.dumps(good)[1:],
            json.dumps(candidates),
            False,
        )
    )
    candidate_case("candidate_version", lambda data: data.update(version=1))
    candidate_case(
        "candidate_duplicate", lambda data: data["pcs"].append(data["pcs"][0])
    )
    candidate_case(
        "candidate_capacity",
        lambda data: data.update(
            pcs=[hex(0x1000 + index * 2) for index in range(21)]
        ),
        args.ideal_hint_ibp,
    )
    if args.ideal_hint_ibp:
        candidate_case(
            "candidate_storage", lambda data: data.update(storage="finite")
        )
        candidate_case(
            "candidate_finite_version", lambda data: data.update(version=2)
        )
    else:
        candidate_case(
            "candidate_unlimited_version",
            lambda data: data.update(version=3, storage="unlimited"),
        )
    candidate_case(
        "candidate_alignment",
        lambda data: data["pcs"].__setitem__(0, "0x1001"),
    )
    cases.append(
        (
            "wrapped_range_keeps_query_port",
            json.dumps(good),
            json.dumps(candidates),
            False,
        )
    )
    results = []
    config = Path(__file__).with_name("hint_indirect_test_config.py").resolve()
    for name, metadata_text, candidate_text, valid in cases:
        case_root = args.outdir / name
        case_root.mkdir()
        metadata_path = case_root / "metadata.json"
        candidate_path = case_root / "candidates.json"
        metadata_path.write_text(metadata_text + "\n")
        candidate_path.write_text(candidate_text + "\n")
        command = [
            str(args.gem5.resolve()),
            "--outdir",
            str(case_root / "gem5"),
            str(config),
            "--metadata",
            str(metadata_path.resolve()),
            "--candidates",
            str(candidate_path.resolve()),
        ]
        if args.ideal_hint_ibp:
            command.append("--ideal-hint-ibp")
        port_test = name == "wrapped_range_keeps_query_port"
        if port_test:
            command.append("--range-port-reuse")
        with (case_root / "gem5.log").open("w") as log:
            result = subprocess.run(
                command, stdout=log, stderr=subprocess.STDOUT, check=False
            )
        log_text = (case_root / "gem5.log").read_text()
        rejection = (
            "Hint permits at most one ordered block query per CPU cycle"
            if port_test
            else "Invalid Hint input:"
        )
        passed = (
            (result.returncode == 0 and "Hint self-test passed" in log_text)
            if valid
            else (result.returncode != 0 and rejection in log_text)
        )
        results.append(
            {
                "name": name,
                "expected_valid": valid,
                "returncode": result.returncode,
                "passed": passed,
                "command": command,
            }
        )
    report = {
        "passed": sum(row["passed"] for row in results),
        "total": len(results),
        "results": results,
    }
    (args.outdir / "audit.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps({"passed": report["passed"], "total": report["total"]}))
    return 0 if all(row["passed"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
