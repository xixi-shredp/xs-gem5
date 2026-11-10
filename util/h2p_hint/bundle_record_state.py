"""Record Bundle source/document identities on the experiment host."""
import argparse
from pathlib import Path
import spec_smoke as smoke


def main():
    smoke.require_experiment_host()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gem5", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    allowed = root / "results/model/smoke-test/bundle-20261103"
    smoke.need(
        args.output.resolve().is_relative_to(allowed.resolve()),
        "Output outside Bundle results",
    )
    source = root / "xs-gem5"
    paths = set()
    for directory in ("src/cpu", "src/arch/riscv", "configs/common", "util/h2p_hint"):
        for path in (source / directory).rglob("*"):
            if path.is_file() and path.suffix in (".cc", ".hh", ".py", ".isa"):
                paths.add(path)
    paths.update(
        (
            source / "src/cpu/pred/SConscript",
            source / "configs/example/backend_v2_norvv_nol3.py",
        )
    )
    documents = [p for p in (root / "docs").glob("*.md")]
    smoke.write(
        args.output,
        {
            "schema": "hint-bundle-source-evidence/v1",
            "host": smoke.execution_host(),
            "binary": smoke.identity(args.gem5),
            "source_files": [smoke.identity(p) for p in sorted(paths)],
            "architecture_documents": [smoke.identity(p) for p in sorted(documents)],
            "scope": "Post-build source identities; no claim of full workload validation",
        },
    )


if __name__ == "__main__":
    main()
