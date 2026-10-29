"""Final-binary real-O3 SE microprobe; no checkpoint/Difftest or IPC claim."""
import argparse
import json
from pathlib import Path
import sys

import m5
from m5.objects import (
    AddrRange, Cache, Process, RiscvO3CPU, Root, SEWorkload, SimpleMemory,
    SrcClockDomain, System, SystemXBar, VoltageDomain,
)

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "configs"))
from common.BackendV2NoRVV import BPU_PARAMS, CPU_PARAMS, TICK_FREQUENCY_HZ
from common.FUScheduler import BackendV2NoRVVScheduler

parser = argparse.ArgumentParser()
parser.add_argument("--elf", type=Path, required=True)
parser.add_argument("--metadata", type=Path, required=True)
args = parser.parse_args()
m5.ticks.setGlobalFrequency(TICK_FREQUENCY_HZ)

system = System()
system.clk_domain = SrcClockDomain(clock="3GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("256MiB")]
system.cache_line_size = 64
system.cpu = RiscvO3CPU(**CPU_PARAMS)
system.cpu.scheduler = BackendV2NoRVVScheduler()
system.cpu.enable_difftest = False
system.cpu.max_insts_any_thread = 10000
for component, values in BPU_PARAMS.items():
    owner = getattr(system.cpu.branchPred, component) if component else system.cpu.branchPred
    for name, value in values.items():
        setattr(owner, name, value)
system.cpu.branchPred.hintMode = "replay"
system.cpu.branchPred.hintMetadata = str(args.metadata.resolve())
system.cpu.branchPred.hintProfileContext = "prediction"
system.cpu.branchPred.idealHintIBP = False

system.membus = SystemXBar()
system.cpu.addPrivateSplitL1Caches(
    Cache(size="64KiB", assoc=4, tag_latency=2, data_latency=2,
          response_latency=2, mshrs=4, tgts_per_mshr=20),
    Cache(size="64KiB", assoc=4, tag_latency=2, data_latency=2,
          response_latency=2, mshrs=16, tgts_per_mshr=20),
)
system.cpu.createInterruptController()
system.cpu.connectBus(system.membus)
system.system_port = system.membus.cpu_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="30ns")
system.memory.port = system.membus.mem_side_ports
system.workload = SEWorkload.init_compatible(str(args.elf.resolve()))
process = Process(cmd=[str(args.elf.resolve())])
system.cpu.workload = process
system.cpu.createThreads()
for isa in system.cpu.isa:
    isa.enable_rvv = False
for decoder in system.cpu.decoder:
    decoder.enable_rvv = False
root = Root(full_system=False, system=system)
(Path(m5.options.outdir) / "microprobe-scope.json").write_text(json.dumps({
    "scope": "real O3 SE frontend/IEW/FTQ Hint history recognition microprobe",
    "cpu_params": CPU_PARAMS, "bpu_params": BPU_PARAMS,
    "scheduler": "BackendV2NoRVVScheduler", "rvv_enabled": False,
    "hint": "finite replay with empty tables; ITH active, no Hint override",
    "difftest": False, "checkpoint": False, "performance_comparison": False,
    "memory_difference": "isolated SE L1-to-SystemXBar-to-SimpleMemory, not benchmark memory hierarchy",
}, indent=2) + "\n")
m5.instantiate()
event = m5.simulate(3_000_000_000)
print("microprobe exit:", event.getCause(), "code", event.getCode())
if "exiting" not in event.getCause().lower() or event.getCode() != 0:
    raise SystemExit(1)
