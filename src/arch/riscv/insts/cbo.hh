/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __ARCH_RISCV_INSTS_CBO_HH__
#define __ARCH_RISCV_INSTS_CBO_HH__

#include <string>

#include "arch/riscv/insts/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

class CboZero : public RiscvStaticInst
{
  private:
    RegId srcRegIdxArr[1];

  public:
    CboZero(ExtMachInst machInst);

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override;
    Fault initiateAcc(ExecContext *xc,
                      Trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      Trace::InstRecord *traceData) const override;

  protected:
    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INSTS_CBO_HH__
