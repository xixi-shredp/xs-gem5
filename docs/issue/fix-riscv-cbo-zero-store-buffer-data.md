# riscv,cpu-o3: Implement CBO.ZERO and preserve zero data in StoreBuffer

branch: xs-dev
commit: 80035e4f5d6754f50aa9bf2ddffd216216bf997a


`xs-dev` has no `CboZero` instruction source, SCons registration, ISA format,
or decode for SYSTEM/`FUNCT3=0x2`/`IMM12=0x004`. Separately,
`LSQUnit::offloadToStoreBuffer()` passes `(uint8_t *)storeWBIt->data()` into
`insertStoreBuffer()` for split and non-split stores. A
`Request::CACHE_BLOCK_ZERO` entry has `isAllZeros()` but no initialized normal
data buffer, so this can place arbitrary data in the StoreBuffer.

## Code

1. Add `CboZero : RiscvStaticInst` in new `insts/cbo.hh/.cc`. Mark it a
   serializing, non-speculative store and a read/write barrier. Read `rs1`,
   align it down to `cpu->cacheLineSize()`, and use `writeMemTiming` or
   `writeMemAtomic` to issue a complete cache-line zero write with
   `Request::CACHE_BLOCK_ZERO`.
2. Register `cbo.cc` in `insts/SConscript`, include `cbo.hh` in
   `isa/includes.isa`, add `CboZeroOp` in `formats/mem.isa`, and decode it at
   the encoding above in `decoder.isa`.
3. Before StoreBuffer insertion, materialize the bytes for an all-zero entry:

   ```cpp
   std::vector<uint8_t> zero_data;
   uint8_t *store_data = reinterpret_cast<uint8_t *>(storeWBIt->data());
   if (storeWBIt->isAllZeros()) {
       zero_data.assign(storeWBIt->size(), 0);
       store_data = zero_data.data();
   }
   ```

   Use `store_data + offset` for split stores and `store_data` otherwise. The
   vector stays live until `insertStoreBuffer()` has copied the bytes.

My Solution:

- `src/arch/riscv/insts/cbo.cc`, `cbo.hh` (new), `SConscript`
- `src/arch/riscv/isa/decoder.isa`, `formats/mem.isa`, `includes.isa`
- `src/cpu/o3/lsq_unit.cc`

My Solution (specific xs-dev locations):

All line numbers refer to `xs-dev` base `80035e4f5d`.

| Baseline location | Current xs-dev code | Required edit |
|---|---|---|
| `src/arch/riscv/insts/SConscript:30-37` | Lists ISA sources; no CBO source exists. | Insert `Source('cbo.cc', tags='riscv isa')` after `standard.cc` (or beside other instruction sources). |
| `src/arch/riscv/isa/includes.isa:49-58` | Includes existing RISC-V instruction headers. | Insert `#include "arch/riscv/insts/cbo.hh"` after `amo.hh`. |
| `src/arch/riscv/isa/formats/mem.isa:227-233` | Ends the Store format; `LoadH` begins at line 235. | Insert `CboZeroOp` here, with `decode_block = 'return new CboZero(machInst);\\n'`. |
| `src/arch/riscv/isa/decoder.isa:582-591` | `0x03` decodes fence/fence.i; `0x04` begins next decoding group. | Add `0x2 -> IMM12 0x004 -> cbo_zero()` in a `CboZeroOp` format between these groups. |
| `src/cpu/o3/lsq_unit.cc:2549-2553` | Obtains `inst` and `request`, then branches by request type. | Insert `zero_data`/`store_data` initialization after line 2551. |
| `src/cpu/o3/lsq_unit.cc:2586-2588` | Split insertion uses `storeWBIt->data() + offset`. | Replace only the data argument with `store_data + offset`. |
| `src/cpu/o3/lsq_unit.cc:2609-2611` | Normal insertion uses `storeWBIt->data()`. | Replace only the data argument with `store_data`. |
| new `src/arch/riscv/insts/cbo.hh/.cc` | No xs-dev location: files do not exist. | Add the `CboZero` declaration and implementation described above. |

## Validation

Build `gem5.opt` and execute `cbo.zero` through the StoreBuffer path; verify
the entire addressed cache line becomes zero. The reference patch was rebased
onto the latest xs-dev Virtual SQ implementation and full-build compiled.
