/*
 * Copyright (c) 2026 Beijing Institute of Open Source Chip
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include "arch/riscv/pagetable.hh"

namespace gem5
{
namespace RiscvISA
{

TEST(RiscvPageTableTest, CanonicalizeSv39VirtualAddress)
{
    EXPECT_EQ(0x0000003fffffffffULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::SV39, 0x0000003fffffffffULL));
    EXPECT_EQ(0xffffffc000000000ULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::SV39, 0xffffffc000000000ULL));
    EXPECT_EQ(0x000000003137bb0eULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::SV39, 0x200000003137bb0eULL));
}

TEST(RiscvPageTableTest, CanonicalizeSv48VirtualAddress)
{
    EXPECT_EQ(0x00007fffffffffffULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::SV48, 0x00007fffffffffffULL));
    EXPECT_EQ(0xffff800000000000ULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::SV48, 0xffff800000000000ULL));
    EXPECT_EQ(0x000000003137bb0eULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::SV48, 0x200000003137bb0eULL));
}

TEST(RiscvPageTableTest, PreserveStageTwoExtendedAddressBits)
{
    EXPECT_EQ(0x0000010000000000ULL,
              VADDR_SEXT(AddrXlateMode::SV39, 0x0000010000000000ULL));
    EXPECT_EQ(0x0002000000000000ULL,
              VADDR_SEXT(AddrXlateMode::SV48, 0x0002000000000000ULL));
}

TEST(RiscvPageTableTest, BareCanonicalizationPreservesAddress)
{
    EXPECT_EQ(0x200000003137bb0eULL,
              VADDR_CANONICALIZE(
                  AddrXlateMode::BARE, 0x200000003137bb0eULL));
}

} // namespace RiscvISA
} // namespace gem5
