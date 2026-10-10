// Decode-time RVV policy, independent of vtype and of the shared decode cache.
#ifndef __ARCH_RISCV_RVV_ENCODING_HH__
#define __ARCH_RISCV_RVV_ENCODING_HH__
#include <cstdint>
namespace gem5 { namespace RiscvISA {
constexpr bool
isRvvEncoding(uint32_t inst)
{
    const auto opcode = inst & 0x7f;
    const auto width = (inst >> 12) & 7;
    const auto csr = (inst >> 20) & 0xfff;
    // Scalar FP loads/stores use widths 2, 3 (and 4 for Q).
    const bool vectorMemory = (opcode == 0x07 || opcode == 0x27) &&
        (width == 0 || width >= 5);
    const bool vectorCsr = opcode == 0x73 && width != 0 &&
        (csr == 0x008 || csr == 0x009 || csr == 0x00a || csr == 0x00f ||
         csr == 0xc20 || csr == 0xc21 || csr == 0xc22);
    return opcode == 0x57 || vectorMemory || vectorCsr;
}
} }
#endif
