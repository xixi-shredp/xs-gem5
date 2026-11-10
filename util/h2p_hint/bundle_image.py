#!/usr/bin/env python3
"""Build private CPython images with the approved 128MiB reserved Hint region.

Original source/runtime trees are read-only inputs. This prepares Linux/DTB/
OpenSBI/GCPT images, without Hint CSR startup or claiming PMA NC configuration.
All build/boot actions must run on ps1/ps2; plan/preflight do not build images.
"""
import argparse
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import re
import signal
import shutil
import socket
import struct
import subprocess
import sys
import time

BUILDER_SHA256 = "05af7fb4a75fd077750b050955e8f9132e35d0b0768815ae93b363171fd71952"
LOOPS = {
    "scimark_fft": 3,
    "pickle_pure_python": 148,
    "deltablue": 268,
    "json_dumps": 79,
    "gc_collect": 1111,
    "pyflate": 3,
    "richards": 19,
}
NC_BASE, NC_SIZE = 0x278000000, 0x08000000
TRACE_BASE, TRACE_SIZE = 0x278200000, 0x04000000


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run_logged(command, *, env, cwd, stdout, stderr, timeout):
    # Terminate descendants as well as the top-level shell when a deadline fires.
    # No asynchronous daemon is created; the supervising launch job observes exit.
    with subprocess.Popen(
        command, env=env, cwd=cwd, stdout=stdout, stderr=stderr, start_new_session=True
    ) as proc:
        try:
            return proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            raise


def reserve_dts(text):
    """Fail closed on drift from the frozen original generated DTS shape."""
    if re.search(r"\breserved-memory\s*\{", text):
        raise ValueError(
            "generated DTS already has reserved-memory; inspect instead of merging blindly"
        )
    root = re.search(
        r"/\s*\{\s*#address-cells\s*=\s*<2>;\s*#size-cells\s*=\s*<2>;", text
    )
    memory = re.findall(r"memory@80000000\s*\{([^{}]*)\}", text)
    if (
        not root
        or len(memory) != 1
        or not re.search(r"reg\s*=\s*<0x0 0x80000000 0x2 0x0>;", memory[0])
    ):
        raise ValueError("expected original 2/2-cell 8GiB DRAM DTS")
    anchor = '    model = "XiangShan";'
    if text.count(anchor) != 1:
        raise ValueError("generated DTS root model anchor changed")
    node = """

    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;
        hint_nc: hint-nc@278000000 {
            reg = <0x2 0x78000000 0x0 0x08000000>;
            no-map;
        };
    };"""
    return text.replace(anchor, anchor + node)


def dtb_properties(path):
    data = Path(path).read_bytes()
    if len(data) < 40:
        raise ValueError("short DTB")
    (
        magic,
        total,
        off,
        strings,
        _,
        version,
        _,
        _,
        strings_size,
        struct_size,
    ) = struct.unpack(">10I", data[:40])
    if magic != 0xD00DFEED or version < 17 or total != len(data):
        raise ValueError("invalid DTB header")
    if off + struct_size > total or strings + strings_size > total:
        raise ValueError("DTB blocks outside file")
    end, names, stack, props = (
        off + struct_size,
        data[strings : strings + strings_size],
        [],
        {},
    )
    while off < end:
        (token,) = struct.unpack_from(">I", data, off)
        off += 4
        if token == 1:
            stop = data.index(0, off, end)
            stack.append(data[off:stop].decode())
            off = (stop + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, nameoff = struct.unpack_from(">II", data, off)
            off += 8
            name = names[nameoff : names.index(0, nameoff)].decode()
            value = data[off : off + size]
            if len(value) != size or off + size > end:
                raise ValueError("DTB property outside structure")
            key = ("/" + "/".join(stack[1:]), name)
            if key in props:
                raise ValueError("duplicate DTB property")
            props[key] = value
            off = (off + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise ValueError("unbalanced DTB nodes")
            return props
        else:
            raise ValueError("unexpected DTB structure token")
    raise ValueError("DTB missing END")


def verify_artifacts(work, bench):
    manifest = json.loads((work / "image-build-manifest.json").read_text())
    if (
        manifest.get("loops") != LOOPS[bench]
        or manifest.get("isa_variant") != "no-rvv"
        or manifest.get("uart", {}).get("clock_frequency") != 10000000
    ):
        raise ValueError("image identity changed")
    for name, item in manifest["artifacts"].items():
        if digest(item["path"]) != item["sha256"]:
            raise ValueError("artifact hash mismatch: " + name)
    props = dtb_properties(manifest["artifacts"]["dtb"]["path"])
    expected = {
        ("/", "#address-cells"): struct.pack(">I", 2),
        ("/", "#size-cells"): struct.pack(">I", 2),
        ("/memory@80000000", "reg"): struct.pack(">4I", 0, 0x80000000, 2, 0),
        ("/reserved-memory", "#address-cells"): struct.pack(">I", 2),
        ("/reserved-memory", "#size-cells"): struct.pack(">I", 2),
        ("/reserved-memory", "ranges"): b"",
        ("/reserved-memory/hint-nc@278000000", "reg"): struct.pack(
            ">4I", 2, 0x78000000, 0, NC_SIZE
        ),
        ("/reserved-memory/hint-nc@278000000", "no-map"): b"",
    }
    for key, value in expected.items():
        if props.get(key) != value:
            raise ValueError("DTB reserved-memory proof failed: " + repr(key))
    bootargs = props[("/chosen", "bootargs")].rstrip(b"\0").decode()
    for arg in (f"cpython.bench={bench}", f"cpython.loops={LOOPS[bench]}"):
        if arg not in bootargs.split():
            raise ValueError("DTB bootargs changed: " + arg)
    config = (
        Path(manifest["artifacts"]["linux_config"]["path"]).read_text().splitlines()
    )
    for line in ("CONFIG_OF_RESERVED_MEM=y", "# CONFIG_RISCV_ISA_V is not set"):
        if line not in config:
            raise ValueError("Linux config proof failed: " + line)
    return {
        "image_manifest_sha256": digest(work / "image-build-manifest.json"),
        "artifacts": manifest["artifacts"],
        "loops": LOOPS[bench],
        "bootargs": bootargs,
        "reserved_base": hex(NC_BASE),
        "reserved_size": NC_SIZE,
        "trace_base": hex(TRACE_BASE),
        "trace_size": TRACE_SIZE,
        "dtb_reserved_memory_proof": True,
        "pma_nc_configured_by_this_tool": False,
        "csr_prelude_added": False,
    }


def environment(args):
    root = args.source_root
    env = os.environ.copy()
    for key in tuple(env):
        if key.startswith("CPYTHON_") or key in (
            "NEMU_HOME",
            "NEMU_BIN",
            "GCPT_RESTORER",
            "SIMPOINT_BIN",
            "PYTHONHOME",
            "PYTHONPATH",
        ):
            del env[key]
    env.update(
        {
            "BUNDLE_SOURCE_ROOT": str(root),
            "RISCV": "/home/xuejing/opt/riscv",
            "CROSS_COMPILE": "/home/xuejing/opt/riscv/bin/riscv64-unknown-linux-gnu-",
            "CPYTHON_HOST_DEPS": str(args.host_deps),
            "CPYTHON_BUILD_ENV": str(root / "tmp/no-rvv/build-env"),
            "CPYTHON_SKILL_DIR": str(args.skill_dir),
            "CPYTHON_BUSYBOX_SOURCE": str(root / "tmp/no-rvv/build-env/busybox"),
            "CPYTHON_RUNTIME_SYSROOT": str(root / "bin/scalar-glibc-sysroot"),
            "CPYTHON_UAPI_SYSROOT": "/home/xuejing/opt/riscv/sysroot",
            "CPYTHON_SCALAR_LIBGCC": str(root / "tmp/no-rvv/tools/scalar-libgcc"),
            "CPYTHON_RESTORER_PATCHER": str(root / "tmp/no-rvv/patch_gcpt_restorer.py"),
            "CPYTHON_UART_TYPE": "uart16550",
            "CPYTHON_UART_MMIO": "0x310b0000",
            "CPYTHON_UART_CLOCK_HZ": "10000000",
            "CPYTHON_ISA_VARIANT": "no-rvv",
            "NEMU_HOME": str(root / "tmp/no-rvv/tools/NEMU"),
            "ZSTD_NBTHREADS": "1",
            "OMP_NUM_THREADS": "1",
            "OPENBLAS_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "XSGEM5_PATCH_ZSTD_THREADS": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONUNBUFFERED": "1",
            "PYTHONNOUSERSITE": "1",
            "GIT_OPTIONAL_LOCKS": "0",
        }
    )
    env["PATH"] = (
        str(args.out_root / "recipe/host-bin")
        + ":/home/xuejing/opt/riscv/bin:"
        + str(args.host_deps / "bin")
        + ":"
        + env.get("PATH", "/usr/bin:/bin")
    )
    env["LD_LIBRARY_PATH"] = str(args.host_deps / "lib") + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
    )
    return env


def preflight(args, env):
    source = args.source_root / "xsgem5"
    if digest(source / "build_gcpt_image_uart16550_10mhz") != BUILDER_SHA256:
        raise ValueError("frozen builder source hash mismatch")
    for path in (
        source / "configure_uart_dts.py",
        source / "validate_build_inputs.py",
        source / "no_rvv_validate.py",
        source / "prepare_initramfs.py",
        source / "xsgem5_trap.c",
        args.skill_dir / "scripts/opensbi_fdt_addr.py",
        args.source_root / "bin/cpython_out/manifest.json",
    ):
        if not path.is_file():
            raise ValueError("required input missing: " + str(path))
    for name in (
        "linux-6.10.3",
        "riscv-rootfs",
        "busybox",
        "nemu_board",
        "opensbi",
        "LibCheckpointAlpha",
    ):
        if not (args.source_root / "tmp/no-rvv/build-env" / name).is_dir():
            raise ValueError("private build source missing: " + name)
    missing_deps = []
    for header in ("SDL2/SDL.h", "readline/readline.h", "zstd.h"):
        if not (args.host_deps / "include" / header).is_file():
            missing_deps.append("include/" + header)
    for name in ("libSDL2.so*", "libreadline.so*", "libzstd.so*"):
        if not any(p.is_file() for p in (args.host_deps / "lib").glob(name)):
            missing_deps.append("lib/" + name)
    if missing_deps:
        raise ValueError(
            "isolated host dependencies missing under "
            + str(args.host_deps)
            + ": "
            + ", ".join(missing_deps)
        )
    for tool in (
        "/home/xuejing/opt/riscv/bin/riscv64-unknown-linux-gnu-gcc",
        "/home/xuejing/opt/riscv/bin/riscv64-unknown-linux-gnu-readelf",
        "/home/xuejing/opt/riscv/bin/riscv64-unknown-linux-gnu-objdump",
        "make",
        "git",
        "flock",
        "dtc",
        "python3",
    ):
        if shutil.which(tool, path=env["PATH"]) is None:
            raise ValueError("required executable missing: " + tool)
    hint = json.loads(
        subprocess.check_output(
            [
                sys.executable,
                str(args.skill_dir / "scripts/host_parallelism.py"),
                "--slices",
                "1",
                "--format",
                "json",
            ],
            env=env,
            text=True,
        )
    )
    parent = args.out_root.parent
    while not parent.exists():
        parent = parent.parent
    gib = 1024**3
    required_memory = int((32 + 20 + args.jobs * 1.5) * gib)
    required_disk = 192 * gib
    headroom = max(0, len(os.sched_getaffinity(0)) - math.ceil(os.getloadavg()[0]))
    hint.update(
        required_memory_bytes=required_memory,
        required_disk_bytes=required_disk,
        disk_free_bytes=shutil.disk_usage(parent).free,
        affinity_cpu_headroom=headroom,
    )
    if (
        hint.get("mem_available_bytes") is None
        or hint["mem_available_bytes"] < required_memory
    ):
        raise ValueError("insufficient memory for private build/boot budget")
    if headroom < args.jobs or hint["suggested_jobs_per_slice"] < args.jobs:
        raise ValueError("insufficient current CPU headroom")
    if hint["disk_free_bytes"] < required_disk:
        raise ValueError("insufficient disk space")
    return hint


def private_recipe(args, recipe):
    source = args.source_root / "xsgem5"
    recipe.mkdir()
    host_bin = recipe / "host-bin"
    host_bin.mkdir()
    (host_bin / "python3").symlink_to(Path(sys.executable).resolve())
    source_hashes = {}
    for path in sorted(list(source.glob("*.py")) + [source / "xsgem5_trap.c"]):
        if path.is_symlink() or not path.is_file():
            raise ValueError("unexpected recipe Python source type: " + str(path))
        shutil.copy2(path, recipe / path.name)
        source_hashes[path.name] = digest(path)
    original = source / "build_gcpt_image_uart16550_10mhz"
    text = original.read_text()
    anchor = 'root=$(cd "$script_dir/.." && pwd)'
    if text.count(anchor) != 1 or digest(original) != BUILDER_SHA256:
        raise ValueError("frozen builder root anchor changed")
    text = text.replace(
        anchor, "root=${BUNDLE_SOURCE_ROOT:?explicit read-only source root required}"
    )
    builder = recipe / original.name
    builder.write_text(text)
    builder.chmod(0o755)
    source_hashes[original.name] = digest(original)
    uart = recipe / "configure_uart_dts.py"
    text = uart.read_text()
    anchor = "    args.dts.write_text(text)"
    if text.count(anchor) != 1:
        raise ValueError("frozen UART DTS writer anchor changed")
    uart.write_text(
        text.replace(
            anchor,
            "    from bundle_image import reserve_dts\n    text = reserve_dts(text)\n"
            + anchor,
        )
    )
    shutil.copy2(Path(__file__).resolve(), recipe / "bundle_image.py")
    return {
        "source_sha256": source_hashes,
        "host_python": {
            "path": str(Path(sys.executable).resolve()),
            "version": sys.version,
            "json_module": str(Path(json.__file__).resolve()),
            "subprocess_module": str(Path(subprocess.__file__).resolve()),
            "sha256": digest(Path(sys.executable).resolve()),
        },
        "private_recipe_sha256": {
            p.name: digest(p) for p in sorted(recipe.iterdir()) if p.is_file()
        },
        "patches": [
            "builder root explicitly uses BUNDLE_SOURCE_ROOT (read-only)",
            "private UART configure invokes reserve_dts before writing generated DTS",
        ],
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--action",
        choices=("plan", "dry-run", "preflight", "build", "verify", "boot"),
        default="plan",
    )
    p.add_argument(
        "--source-root", type=Path, default=Path("/home/xuejing/sim/workloads/cpython")
    )
    p.add_argument("--out-root", type=Path, required=True)
    p.add_argument("--benchmark", choices=tuple(LOOPS), default="scimark_fft")
    p.add_argument(
        "--loops", type=int, help="must match the original authorized loop count"
    )
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument(
        "--host-deps",
        type=Path,
        default=Path("/home/xuejing/opt/cpython-ict186-host-deps"),
    )
    p.add_argument(
        "--skill-dir",
        type=Path,
        default=Path("/home/xuejing/opt/codex/skill/make-xs-checkpoints"),
    )
    p.add_argument(
        "--host-deps-identity",
        type=Path,
        help="verified source/destination selected-tree comparison JSON",
    )
    p.add_argument("--expected-host", default="iv-yexl3v2u4gwh2ysmqmn1")
    p.add_argument("--build-timeout-seconds", type=int, default=7200)
    p.add_argument("--boot-timeout-seconds", type=int, default=3600)
    args = p.parse_args()
    if socket.gethostname() != args.expected_host:
        p.error("run builds/checks on the explicitly selected ps1/ps2 host")
    if not 1 <= args.jobs <= 8 or (
        args.loops is not None and args.loops != LOOPS[args.benchmark]
    ):
        p.error("jobs must be1..8 and loops must match the authorized original input")
    if args.build_timeout_seconds < 1 or args.boot_timeout_seconds < 1:
        p.error("timeouts must be positive")
    args.source_root = args.source_root.resolve()
    args.out_root = args.out_root.resolve()
    args.host_deps = args.host_deps.resolve()
    args.skill_dir = args.skill_dir.resolve()
    if args.out_root == args.source_root or args.out_root.is_relative_to(
        args.source_root
    ):
        p.error("output must be outside the immutable migrated source tree")
    work = args.out_root / (args.benchmark + "_test")
    recipe = args.out_root / "recipe"
    image = work / "images" / (args.benchmark + "_test.bin")
    env = environment(args)
    command = [
        str(recipe / "build_gcpt_image_uart16550_10mhz"),
        "--isa-variant",
        "no-rvv",
        "--benchmark",
        args.benchmark,
        "--scale",
        "test",
        "--runtime-root",
        str(args.source_root / "bin/cpython_out/stage"),
        "--runtime-manifest",
        str(args.source_root / "bin/cpython_out/manifest.json"),
        "--workflow",
        str(work),
        "--shared-linux",
        str(args.out_root / "shared-linux"),
        "--output",
        str(image),
        "--jobs",
        str(args.jobs),
        "--loops",
        str(LOOPS[args.benchmark]),
    ]
    summary = {
        "action": args.action,
        "benchmark": args.benchmark,
        "loops": LOOPS[args.benchmark],
        "source_root": str(args.source_root),
        "out_root": str(args.out_root),
        "builder_command": command,
        "reserved_base": hex(NC_BASE),
        "reserved_size": NC_SIZE,
        "trace_base": hex(TRACE_BASE),
        "trace_size": TRACE_SIZE,
        "pma_configuration": "required in simulator/RTL separately",
        "csr_prelude_added": False,
    }
    if args.action == "plan":
        print(json.dumps(summary, indent=2))
        return 0
    if args.action in ("preflight", "build", "boot"):
        summary["resource_preflight"] = preflight(args, env)
    if args.action == "preflight":
        print(json.dumps(summary, indent=2))
        return 0
    if args.action in ("dry-run", "build"):
        if args.out_root.exists():
            raise ValueError(
                "output already exists; preserve it and choose a new private root"
            )
        args.out_root.mkdir(parents=True)
        summary["recipe"] = private_recipe(args, recipe)
        summary["runtime_manifest_sha256"] = digest(
            args.source_root / "bin/cpython_out/manifest.json"
        )
        summary["tool_sha256"] = digest(Path(__file__).resolve())
        summary["driver_environment"] = {
            key: value
            for key, value in env.items()
            if key.startswith("CPYTHON_")
            or key
            in (
                "PATH",
                "LD_LIBRARY_PATH",
                "PYTHONNOUSERSITE",
                "PYTHONDONTWRITEBYTECODE",
                "RISCV",
                "CROSS_COMPILE",
                "NEMU_HOME",
                "BUNDLE_SOURCE_ROOT",
            )
        }
        if args.host_deps_identity is not None:
            identity = json.loads(args.host_deps_identity.read_text())
            if identity.get("pass") is not True or identity.get(
                "source_sha256"
            ) != identity.get("destination_sha256"):
                raise ValueError("host dependency identity is not verified")
            summary["host_deps_identity"] = {
                "path": str(args.host_deps_identity.resolve()),
                "sha256": digest(args.host_deps_identity),
                "comparison": identity,
            }
            shutil.copy2(
                args.host_deps_identity, args.out_root / "host-deps-identity.json"
            )
        if args.action == "dry-run":
            command.append("--dry-run")
        (args.out_root / "build-plan.json").write_text(
            json.dumps(summary, indent=2) + "\n"
        )
        start = time.monotonic()
        status = 1
        try:
            with (args.out_root / "build.stdout.log").open("w") as out, (
                args.out_root / "build.stderr.log"
            ).open("w") as err:
                status = run_logged(
                    command,
                    env=env,
                    cwd=args.source_root,
                    stdout=out,
                    stderr=err,
                    timeout=args.build_timeout_seconds,
                )
            if status == 0 and args.action == "build":
                summary["proof"] = verify_artifacts(work, args.benchmark)
        except Exception as error:
            status = 1
            summary["failure"] = str(error)
        finally:
            summary.update(exit_code=status, wall_seconds=time.monotonic() - start)
            (args.out_root / "build-result.json").write_text(
                json.dumps(summary, indent=2) + "\n"
            )
        print(json.dumps(summary, indent=2))
        return status
    proof = verify_artifacts(work, args.benchmark)
    if args.action == "verify":
        print(json.dumps(proof, indent=2))
        return 0
    boot = args.out_root / "boot"
    boot.mkdir()  # Preserve prior logs; a second boot requires a new explicit output root.
    nemu = args.source_root / "tmp/no-rvv/tools/NEMU/build/riscv64-nemu-interpreter"
    summary.update(
        proof=proof,
        boot_command=[str(nemu), "-b", str(image)],
        nemu_sha256=digest(nemu),
    )
    start = time.monotonic()
    stdout, stderr = boot / "stdout.log", boot / "stderr.log"
    status = 1
    try:
        with stdout.open("w") as out, stderr.open("w") as err:
            status = run_logged(
                summary["boot_command"],
                env=env,
                stdout=out,
                stderr=err,
                cwd=args.source_root,
                timeout=args.boot_timeout_seconds,
            )
        if status == 0:
            sys.path.insert(0, str(recipe))
            validate = importlib.import_module("no_rvv_validate")
            summary["guest_validation"] = validate.verify_guest_recovery_logs(
                stdout, stderr, workload=args.benchmark + "_test"
            )
            uart = importlib.import_module("validate_uart10mhz")
            uart.verify_linux_uart_clock(stderr.read_text(), 0x310B0000, 10000000)
            text = stderr.read_text()
            if not re.search(
                r"OF: reserved mem:.*0x0*278000000\.\.0x0*27fffffff.*nomap.*hint-nc@278000000",
                text,
            ):
                raise ValueError(
                    "guest boot lacks approved reserved-memory nomap evidence"
                )
            summary["guest_reserved_memory_nomap_proof"] = True
    except Exception as error:
        status = 1
        summary["failure"] = str(error)
    finally:
        summary.update(exit_code=status, wall_seconds=time.monotonic() - start)
        (boot / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return status


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("bundle_image: " + str(error), file=sys.stderr)
        raise SystemExit(1)
