#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include "arch/riscv/rvv_encoding.hh"
#include "cpu/pred/btb/h2p_indirect_stats.hh"
#include "mem/dram_completion_tracker.hh"

using gem5::RiscvISA::isRvvEncoding;
using gem5::memory::DramCompletionTracker;

int main()
{
    // All RVV arithmetic/config encodings and vector memory widths are denied;
    // scalar FP memory, FMVs and FLI retain their execution resources.
    assert(isRvvEncoding(0x02000057));
    assert(isRvvEncoding(0x00007057));
    for (unsigned op : {0x07u, 0x27u}) {
        for (unsigned width = 0; width < 8; ++width)
            assert(isRvvEncoding(op | width << 12) == (width == 0 || width >= 5));
    }
    for (unsigned csr : {0x008u, 0x009u, 0x00au, 0x00fu, 0xc20u, 0xc21u, 0xc22u}) {
        for (unsigned funct = 1; funct < 8; ++funct)
            assert(isRvvEncoding((csr << 20) | (funct << 12) | 0x73));
    }
    for (unsigned inst : {0x00002007u, 0x00003027u, 0xf0000053u,
                          0xe2000053u, 0x30002073u, 0x00000013u})
        assert(!isRvvEncoding(inst));

    DramCompletionTracker<uintptr_t> tracker;
    constexpr uint64_t pad = 66000;
    // Interleaved repeated-address read/write callbacks must consume different
    // accepted packets. No capacity or ownership is released at raw callback.
    tracker.accept(0x1000, 11, false, true);
    tracker.accept(0x1000, 12, false, true);
    tracker.accept(0x1000, 21, true, true);
    tracker.accept(0x1000, 22, true, false);
    assert(tracker.outstanding() == 4);
    tracker.callback(0x1000, true, 4000, pad);
    tracker.callback(0x1000, false, 76000, pad);
    tracker.callback(0x1000, false, 76000, pad);
    tracker.callback(0x1000, true, 4000, pad);
    assert(tracker.outstanding() == 4);
    assert(!tracker.ready(69999));
    assert(tracker.nextReady() == 70000);
    auto first = tracker.takeReady(70000);
    assert(first.packet == 21 && first.write && first.needsResponse);
    assert(tracker.outstanding() == 3);
    auto second = tracker.takeReady(70000);
    assert(second.packet == 22 && second.write && !second.needsResponse);
    assert(!tracker.ready(141999));
    assert(tracker.takeReady(142000).packet == 11);
    assert(tracker.takeReady(142000).packet == 12);
    assert(tracker.outstanding() == 0 && !tracker.hasPending());
    // The response queue still owns an acknowledged packet until sendTimingResp
    // succeeds. Its size is added to tracker.outstanding() by the actual driver.
    unsigned blockedResponse = 1;
    assert(tracker.outstanding() + blockedResponse != 0); // drain/retry blocked
    --blockedResponse;
    assert(tracker.outstanding() + blockedResponse == 0);
    // Zero-padding compatibility retains FIFO and immediately matures callback.
    tracker.accept(0, 31, false, true);
    tracker.callback(0, false, 1, 0);
    assert(tracker.ready(1) && tracker.takeReady(1).packet == 31);
    using gem5::branch_prediction::btb_pred::H2PIndirectStats;
    assert(H2PIndirectStats::eligibleIndirect(true, false));
    assert(!H2PIndirectStats::eligibleIndirect(true, true));
    assert(!H2PIndirectStats::eligibleIndirect(false, false));
    H2PIndirectStats targets;
    targets.record(0x1000, 0x2000, std::nullopt);
    targets.record(0x1000, 0x2000, 0x3000);
    targets.record(0x1000, 0x2000, 0x2000);
    targets.record(0x4000, 0x5000, 0x5000); // all-correct PC still in denominator
    std::ostringstream summary;
    targets.writeSummary(summary);
    assert(summary.str().find("\"executions\":4") != std::string::npos);
    assert(summary.str().find("\"absent_target\":1") != std::string::npos);
    assert(summary.str().find("\"valid_wrong_target\":1") != std::string::npos);
    assert(summary.str().find("\"target_misses\":2") != std::string::npos);
    assert(summary.str().find("\"pc\":\"0x4000\"") != std::string::npos);
    targets.clear();
    summary.str("");
    targets.writeSummary(summary);
    assert(summary.str().find("\"executions\":0") != std::string::npos);
    std::cout << "RVV encoding and padded completion ownership checks passed\n";
}
