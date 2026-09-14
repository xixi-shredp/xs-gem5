/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#include "arch/riscv/insts/cbo.hh"

#include <vector>

#include "arch/generic/memhelpers.hh"
#include "arch/riscv/utility.hh"
#include "base/addr_range.hh"
#include "cpu/base.hh"
#include "cpu/exec_context.hh"

namespace gem5
{

namespace RiscvISA
{

CboZero::CboZero(ExtMachInst machInst)
    : RiscvStaticInst("cbo.zero", machInst, OpClass::MemWrite)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr), nullptr);
    setSrcRegIdx(_numSrcRegs++, RegId(IntRegClass, machInst.rs1));
    flags[IsStore] = true;
    flags[IsNonSpeculative] = true;
    flags[IsSerializeBefore] = true;
    flags[IsSerializeAfter] = true;
    flags[IsReadBarrier] = true;
    flags[IsWriteBarrier] = true;
}

static Fault
zeroCacheBlock(ExecContext *xc, const StaticInst *inst, bool timing)
{
    const Addr address = xc->getRegOperand(inst, 0);
    const auto block_size = xc->tcBase()->getCpuPtr()->cacheLineSize();
    const Addr block_addr = roundDown(address, block_size);
    std::vector<uint8_t> zeroes(block_size, 0);
    std::vector<bool> byte_enable(block_size, true);

    if (timing) {
        return writeMemTiming(xc, zeroes.data(), block_addr, block_size,
                              Request::CACHE_BLOCK_ZERO,
                              nullptr, byte_enable);
    }
    return writeMemAtomic(xc, zeroes.data(), block_addr, block_size,
                          Request::CACHE_BLOCK_ZERO,
                          nullptr, byte_enable);
}

Fault
CboZero::execute(ExecContext *xc, Trace::InstRecord *traceData) const
{
    return zeroCacheBlock(xc, this, false);
}

Fault
CboZero::initiateAcc(ExecContext *xc, Trace::InstRecord *traceData) const
{
    return zeroCacheBlock(xc, this, true);
}

Fault
CboZero::completeAcc(PacketPtr pkt, ExecContext *xc,
                     Trace::InstRecord *traceData) const
{
    return NoFault;
}

std::string
CboZero::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    return csprintf("%s %s", mnemonic, registerName(srcRegIdx(0)));
}

} // namespace RiscvISA
} // namespace gem5
