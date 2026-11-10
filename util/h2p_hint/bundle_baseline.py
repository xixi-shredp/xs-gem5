"""Build and run the frozen single-table baseline on the experiment host."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import spec_smoke as smoke

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "results/model/smoke-test/bundle-20261103/baseline"


def main():
    smoke.require_experiment_host()
    OUT.mkdir(parents=True, exist_ok=True)
    old = smoke.read(ROOT / "results/model/smoke-test/cam58-20261023-v2/sources.json")
    sources = [x for x in old if "/src/" in x["path"]]
    for item in sources:
        smoke.need(
            smoke.identity(item["path"]) == item, "Frozen baseline source changed"
        )
    smoke.write(OUT / "sources.json", sources)
    budget = smoke.resource_snapshot(memory_per_job_gib=4, reserve_gib=32)
    cpus = sorted(os.sched_getaffinity(0))[: budget["job_capacity"]]
    smoke.need(bool(cpus), "No build headroom")
    smoke.write(OUT / "resources.json", budget)
    env = os.environ.copy()
    env["PATH"] = str(ROOT / "deps/build-env/bin") + ":" + env["PATH"]
    with (OUT / "build.log").open("w") as log:
        subprocess.run(
            [
                "taskset",
                "-c",
                ",".join(map(str, cpus)),
                "bash",
                "-c",
                "scons -j$(nproc) build/RISCV/gem5.opt",
            ],
            cwd=ROOT / "xs-gem5",
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    elf = ROOT / "xs-gem5/build/RISCV/gem5.opt"
    smoke.need(
        os.access(elf, os.X_OK) and elf.open("rb").read(4) == b"\x7fELF", "Invalid ELF"
    )
    with (OUT / "elf.log").open("w") as log:
        subprocess.run(["file", str(elf)], stdout=log, check=True)
        subprocess.run(["ls", "-l", str(elf)], stdout=log, check=True)
    frozen = OUT / "gem5-v10.opt"
    shutil.copy2(elf, frozen)
    smoke.write(OUT / "binary.json", smoke.identity(frozen))
    print("BASELINE BUILD PASS", flush=True)
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "xs-gem5/util/h2p_hint/spec_smoke.py"),
            "--manifest",
            str(ROOT / "manifest/smoke-test.json"),
            "--workload-manifest",
            str(ROOT / "workload/cpython-norvv-uart16550-100m200m-heavy.json"),
            "--input-root",
            "/home/xuejing/workloads/cpython/cpts/no-rvv-uart16550-100m200m",
            "--ref-so",
            str(
                ROOT
                / "results/model/full-test/cam58-100m200m-cbo-20261023/nemu/build/riscv64-nemu-interpreter-so"
            ),
            "--gem5",
            str(frozen),
            "--output-root",
            str(OUT / "pipeline"),
            "--stage",
            "all",
            "--jobs",
            "auto",
            "--reserve-gib",
            "32",
        ],
        cwd=ROOT / "xs-gem5",
        check=True,
    )
    print("BASELINE PIPELINE PASS", flush=True)


if __name__ == "__main__":
    main()
