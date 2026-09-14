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
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=False)
    good = json.loads(args.good_metadata.read_text())
    candidates = json.loads(args.good_candidates.read_text())
    cases = []

    def metadata_case(name, mutation, valid=False):
        data = copy.deepcopy(good)
        mutation(data)
        cases.append((name, json.dumps(data), json.dumps(candidates), valid))

    def candidate_case(name, mutation):
        data = copy.deepcopy(candidates)
        mutation(data)
        cases.append((name, json.dumps(good), json.dumps(data), False))

    metadata_case("valid", lambda data: None, True)
    metadata_case("empty", lambda data: data.update(cam=[], slots=[], entries=[]), True)
    metadata_case("invalid_bit", lambda data: data["entries"][0].update(valid=False, conf=0), True)
    metadata_case("version", lambda data: data.update(version=1))
    metadata_case("hash_version", lambda data: data.update(hash_version=1))
    metadata_case("ghr_length", lambda data: data.update(ghr_bits=971))
    metadata_case("mask_order", lambda data: data["mask_order"].reverse())
    metadata_case("history_length", lambda data: data["history"].update(call_pcs=65))
    metadata_case("target_encoding", lambda data: data["history"].update(target_shift=2))
    metadata_case("layout", lambda data: data["layout"].update(ways=4))
    metadata_case("mask_range", lambda data: data["cam"][0].update(mask=32))
    metadata_case("negative_mask", lambda data: data["cam"][0].update(mask=-1))
    metadata_case("fractional_mask", lambda data: data["cam"][0].update(mask=1.5))
    metadata_case("quoted_mask", lambda data: data["cam"][0].update(mask="1"))
    metadata_case("pc_alignment", lambda data: data["cam"][0].update(pc="0x1001"))
    metadata_case("pc_canonical", lambda data: data["cam"][0].update(pc="0x1000000000000"))
    metadata_case("duplicate_pc", lambda data: data["cam"].append(data["cam"][0]))
    metadata_case("cam_capacity", lambda data: data.update(cam=[
        {"pc": hex(0x1000 + index * 2), "mask": 0, "valid": True}
        for index in range(17)
    ]))
    metadata_case("slot_capacity", lambda data: data.update(slots=[
        hex(0x4000 + index * 2) for index in range(33)
    ]))
    metadata_case("duplicate_slot", lambda data: data["slots"].append(data["slots"][0]))
    metadata_case("target_alignment", lambda data: data["slots"].__setitem__(0, "0x4001"))
    metadata_case("unloaded_slot", lambda data: data["entries"][0].update(slot=31))
    metadata_case("slot_pointer_width", lambda data: data["entries"][0].update(slot=32))
    metadata_case("tag_width", lambda data: data["entries"][0].update(tag=1 << 17))
    metadata_case("set_range", lambda data: data["entries"][0].update(set=2048))
    metadata_case("way_range", lambda data: data["entries"][0].update(way=2))
    metadata_case("duplicate_position", lambda data: data["entries"].append(data["entries"][0]))
    metadata_case("duplicate_tag", lambda data: data["entries"][1].update(tag=data["entries"][0]["tag"]))
    metadata_case("initial_confidence", lambda data: data["entries"][0].update(conf=0))
    metadata_case("confidence_width", lambda data: data["entries"][0].update(conf=4))
    metadata_case("invalid_boolean", lambda data: data["entries"][0].update(valid=1))
    metadata_case("quoted_boolean", lambda data: data["entries"][0].update(valid="true"))
    metadata_case("cam_object", lambda data: data.update(cam={}))
    metadata_case("slots_object", lambda data: data.update(slots={}))
    metadata_case("source_array", lambda data: data.update(source=[]))
    metadata_case("missing_history", lambda data: data.pop("history"))
    metadata_case("missing_source", lambda data: data.pop("source"))
    cases.append(("duplicate_field", '{"version":2,' + json.dumps(good)[1:],
                  json.dumps(candidates), False))
    candidate_case("candidate_version", lambda data: data.update(version=1))
    candidate_case("candidate_duplicate", lambda data: data["pcs"].append(data["pcs"][0]))
    candidate_case("candidate_capacity", lambda data: data.update(pcs=[
        hex(0x1000 + index * 2) for index in range(21)
    ]))
    candidate_case("candidate_alignment", lambda data: data["pcs"].__setitem__(0, "0x1001"))
    results = []
    config = Path(__file__).with_name("hint_indirect_test_config.py").resolve()
    for name, metadata_text, candidate_text, valid in cases:
        case_root = args.outdir / name
        case_root.mkdir()
        metadata_path = case_root / "metadata.json"
        candidate_path = case_root / "candidates.json"
        metadata_path.write_text(metadata_text + "\n")
        candidate_path.write_text(candidate_text + "\n")
        command = [str(args.gem5.resolve()), "--outdir", str(case_root / "gem5"),
                   str(config), "--metadata", str(metadata_path.resolve()),
                   "--candidates", str(candidate_path.resolve())]
        with (case_root / "gem5.log").open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    check=False)
        log_text = (case_root / "gem5.log").read_text()
        passed = ((result.returncode == 0 and "Hint self-test passed" in log_text)
                  if valid else
                  (result.returncode != 0 and "Invalid Hint input:" in log_text))
        results.append({"name": name, "expected_valid": valid,
                        "returncode": result.returncode, "passed": passed,
                        "command": command})
    report = {"passed": sum(row["passed"] for row in results),
              "total": len(results), "results": results}
    (args.outdir / "audit.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "total": report["total"]}))
    return 0 if all(row["passed"] for row in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
