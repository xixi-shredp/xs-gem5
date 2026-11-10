"""Prepare an isolated copy of a proven NEMU ref; never patch the live source.

Run on ps1/ps2 with --source pointing to the successful CBO reference tree.
This tool only prepares source; the experiment driver owns build/deployment.
"""
import argparse
import difflib
import hashlib
import json
import shutil
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(source, destination):
    source, destination = source.resolve(), destination.resolve()
    root = Path(__file__).resolve().parents[3]
    allowed = root / "results/model/smoke-test/bundle-20261103"
    if not destination.is_relative_to(allowed) or destination == allowed:
        raise ValueError("Destination must be a new directory below Bundle results")
    if (
        destination.exists()
        or destination.is_relative_to(source)
        or source.is_relative_to(destination)
    ):
        raise ValueError("Destination exists or overlaps reference source")
    relative = Path("src/isa/riscv64/system/priv.c")
    original = (source / relative).read_text()
    config = source / ".config"
    if not config.is_file() or "CONFIG_RV_CBO=y" not in config.read_text().splitlines():
        raise ValueError("Source must retain the already successful CBO configuration")
    anchor = "void init_csr() {\n  MAP(CSRS, CSRS_EXIST)"
    if original.count(anchor) != 1 or "difftest_hint_csr" in original:
        raise ValueError("Unexpected/already patched CSR source")
    for required in (
        "default: return *src;",
        "default: *dest = src;",
        "csr_normal_permit_check",
        "csr_readonly_permit_check",
    ):
        if required not in original:
            raise ValueError("Expected normal CSR execution/permission path missing")
    patched = original.replace(
        anchor,
        anchor
        + """
  /* Hint device CSRs: keep the generic S/M privilege and write checks. */
  csr_exist[0x5c0] = csr_exist[0x5c1] = csr_exist[0x5c2] = 1;""",
    )
    patched += """
/* Seed the DUT-observed old value before a CSR instruction with rd != x0.
 * CSRRS/CSRRC/CSRRW still execute normally; CPU/register ABI is unchanged.
 * Architectural device write side effects remain the DUT's responsibility.
 */
__attribute__((visibility("default")))
void difftest_hint_csr(uint32_t csr, uint64_t oldvalue) {
  assert(csr >= 0x5c0 && csr <= 0x5c2);
  csr_array[csr] = oldvalue;
}
"""
    destination.parent.mkdir(parents=True, exist_ok=True)

    def ignored(directory, names):
        # SoftFloat tracks its platform Makefiles under repo/build: those are
        # source inputs, unlike NEMU/fixdep output directories named build.
        softfloat_source = (
            Path(directory).relative_to(source).as_posix() == "resource/softfloat/repo"
        )
        return [
            name
            for name in names
            if name in (".git", "__pycache__")
            or (name == "build" and not softfloat_source)
        ]

    shutil.copytree(source, destination, ignore=ignored)
    (destination / relative).write_text(patched)
    patch = "".join(
        difflib.unified_diff(
            original.splitlines(True),
            patched.splitlines(True),
            fromfile="a/" + str(relative),
            tofile="b/" + str(relative),
        )
    )
    (destination / "bundle-hint-csr.patch").write_text(patch)
    evidence = {
        "status": "SOURCE_PREPARED_NOT_BUILT",
        "source": str(source),
        "destination": str(destination),
        "config_sha256": sha(config),
        "original_priv_sha256": sha(source / relative),
        "patched_priv_sha256": sha(destination / relative),
        "tool_sha256": sha(Path(__file__)),
        "abi": "void difftest_hint_csr(uint32_t csr, uint64_t oldvalue)",
        "contract": "Seed captured old value before rd!=x0 Hint CSR instructions; rd=x0 executes normally without seeding; never skip execution",
        "excluded": [
            ".git",
            "build except resource/softfloat/repo/build source",
            "__pycache__",
        ],
    }
    (destination / "bundle-reference-source.json").write_text(
        json.dumps(evidence, indent=2) + "\n"
    )
    return evidence


if __name__ == "__main__":
    from spec_smoke import require_experiment_host

    require_experiment_host()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source, args.destination), indent=2))
