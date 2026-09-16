#ifndef __CPU_PRED_BTB_TEST_HINT_HISTORY_IDEAL_TEST_HH__
#define __CPU_PRED_BTB_TEST_HINT_HISTORY_IDEAL_TEST_HH__

#include <algorithm>
#include <memory>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "base/logging.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/hint_history.hh"

namespace gem5::branch_prediction::btb_pred
{

// Run through the final gem5.opt production self-test SimObject. The expected
// values below are independent event histories, not a second tree algorithm.
inline void
testIdealHistory()
{
    const auto require = [](bool condition, const char *message) {
        fatal_if(!condition, "Ideal Hint history self-test: %s", message);
    };
    const auto fold_bits = [](const boost::dynamic_bitset<> &bits) {
        uint32_t result = 0;
        for (size_t bit = 0; bit < bits.size(); ++bit) {
            if (bits[bit])
                result ^= uint32_t(1) << (bit % HintHashBits);
        }
        return result;
    };
    const auto compare = [&require](const HintHistory &actual,
                                    const HintHistory &expected,
                                    const std::vector<Addr> &pcs) {
        require(actual.globalLow == expected.globalLow &&
                actual.globalHigh == expected.globalHigh,
                "recovery must restore both global target-history words");
        require(actual.calls.count == expected.calls.count &&
                actual.returns.count == expected.returns.count &&
                actual.calls.folded == expected.calls.folded &&
                actual.returns.folded == expected.returns.folded,
                "call/return recovery classification");
        for (unsigned age = 0; age < actual.calls.count; ++age)
            require(actual.calls.atAge(age) == expected.calls.atAge(age),
                    "call event ages after recovery");
        for (unsigned age = 0; age < actual.returns.count; ++age)
            require(actual.returns.atAge(age) == expected.returns.atAge(age),
                    "return event ages after recovery");
        for (const auto pc : pcs)
            require(actual.local(pc) == expected.local(pc),
                    "all local histories after recovery");
    };

    // Non-power-of-two PC count exercises both ends and uneven tree levels.
    std::vector<Addr> pcs;
    for (size_t index = 0; index < 257; ++index)
        pcs.push_back(0x1000 + index * 4);
    auto unsorted_pcs = pcs;
    std::reverse(unsorted_pcs.begin(), unsorted_pcs.end());
    HintHistory history;
    history.configure(unsorted_pcs, true);
    boost::dynamic_bitset<> ghr(HintGHRBits);
    for (size_t bit = 0; bit < HintGHRBits; ++bit)
        ghr[bit] = (bit % 9) < 4;
    const HintSnapshot empty(history, ghr);
    std::vector<uint8_t> expected_local(pcs.size(), 0);
    boost::dynamic_bitset<> expected_global(HintGlobalTargets * 2);
    for (unsigned round = 0; round < 9; ++round) {
        for (size_t index = 0; index < pcs.size(); ++index) {
            const uint8_t bits = (index * 3 + round) & 3;
            expected_local[index] = (expected_local[index] << 2) | bits;
            expected_global <<= 2;
            expected_global[0] = bits & 1;
            expected_global[1] = bits & 2;
            history.update(pcs[index], 0x8000 | (bits << 1),
                           true, false, false, true);
            require(history.local(pcs[index]) == expected_local[index],
                    "unlimited locals retain exactly four targets per PC");
            require(history.globalFold() == fold_bits(expected_global),
                    "unlimited mode keeps exactly 64 global target events");
        }
    }
    for (size_t index = 0; index < pcs.size(); ++index) {
        require(history.local(pcs[index]) == expected_local[index],
                "unlimited local update isolation");
        require(empty.history.local(pcs[index]) == 0,
                "older zero-root snapshot is immutable");
    }
    require(history.local(0x3000) == 0,
            "uncatalogued PCs do not acquire local histories");
    const HintSnapshot populated(history, ghr);
    const auto saved_context = populated.contextsFor(pcs.back());
    for (unsigned event = 0; event < 5; ++event)
        history.update(pcs.back(), 0x8002, true, true, false, true);
    require(populated.contextsFor(pcs.back()) == saved_context &&
            history.local(pcs.back()) == 0x55,
            "new updates cannot mutate a populated prediction snapshot");
    history = populated.history;
    require(history.local(pcs.back()) == expected_local.back(),
            "rollback restores an older shared root");
    history.update(pcs.front(), 0x8006, true, false, false, true);
    require(populated.history.local(pcs.front()) == expected_local.front(),
            "divergent update after rollback leaves original root intact");
    history = empty.history;
    require(history.local(pcs.front()) == 0 && history.local(pcs.back()) == 0,
            "rollback can recover an empty root after populated roots");

    // Verify exact event eviction using a direct age-vector oracle, including
    // calls that also return and therefore do not enter target histories.
    history = populated.history;
    const auto old_global = history.globalFold();
    std::vector<uint64_t> events;
    for (unsigned event = 0; event < 130; ++event) {
        const Addr pc = 0x4000 + event * 6;
        events.insert(events.begin(), (pc >> 1) & HintAddressWordMask);
        if (events.size() > HintPCEvents)
            events.pop_back();
        history.update(pc, 0x8006, true, true, true, true);
        uint32_t folded = 0;
        for (size_t age = 0; age < events.size(); ++age) {
            const auto word = fold_bits(boost::dynamic_bitset<>(64,
                                                               events[age]));
            for (unsigned bit = 0; bit < HintHashBits; ++bit) {
                if (word & (uint32_t(1) << bit))
                    folded ^= uint32_t(1) << ((bit + age) % HintHashBits);
            }
            require(history.calls.atAge(age) == events[age] &&
                    history.returns.atAge(age) == events[age],
                    "64-event windows retain exact age order");
        }
        require(history.calls.count == events.size() &&
                history.returns.count == events.size() &&
                history.calls.folded == folded &&
                history.returns.folded == folded,
                "the 65th call/return event evicts the oldest contribution");
        require(history.globalFold() == old_global,
                "call+return excludes GITHR in unlimited mode");
    }
    for (size_t index = 0; index < pcs.size(); ++index)
        require(history.local(pcs[index]) == expected_local[index],
                "call+return does not alter LITHR");

    // A >20-PC snapshot recovers the same retired prefix for memory, control,
    // and trap squashes; wrong-path events and equal-PC retired events drop.
    const auto base = populated.history;
    BranchInfo call;
    call.pc = pcs[33];
    call.target = 0x9002;
    call.size = 4;
    call.isCall = call.isIndirect = true;
    call.isDirect = false;
    BranchInfo ret = call;
    ret.pc = pcs[35];
    ret.isCall = false;
    ret.isReturn = true;
    BranchInfo jump = call;
    jump.pc = pcs[36];
    jump.target = 0x9004;
    jump.isCall = false;
    FetchTarget target;
    target.hintSnapshot = std::make_shared<const HintSnapshot>(base, ghr);
    target.recordRetiredBranch(call, true, 16);
    target.recordRetiredBranch(ret, true, 16);
    target.recordRetiredBranch(jump, true, 16);
    target.lastRetiredPC = pcs[37];
    auto recovered = base;
    recovered.update(pcs[250], 0x9006, true, true, true, true);
    recovered.update(pcs[251], 0x9006, true, true, false, true);
    recovered.recoverForSquash(target, ret.pc, false);
    auto expected = base;
    expected.update(call.pc, call.target, true, true, false, true);
    compare(recovered, expected, pcs);
    target.exeBranchInfo = ret;
    target.exeTaken = true;
    recovered.recoverForSquash(target, ret.pc, true);
    expected.update(ret.pc, ret.target, true, false, true, true);
    compare(recovered, expected, pcs);
    target.exeBranchInfo = jump;
    target.exeBranchInfo.target = 0x9006;
    recovered.recoverForSquash(target, jump.pc, true);
    expected.update(jump.pc, 0x9006, true, false, false, true);
    compare(recovered, expected, pcs);
    target.exeBranchInfo = call;
    target.exeBranchInfo.target = 0x9000;
    recovered.recoverForSquash(target, call.pc, true);
    expected = base;
    expected.update(call.pc, 0x9000, true, true, false, true);
    compare(recovered, expected, pcs);
    recovered.recoverForSquash(target, target.lastRetiredPC, false);
    expected = base;
    expected.update(call.pc, call.target, true, true, false, true);
    expected.update(ret.pc, ret.target, true, false, true, true);
    expected.update(jump.pc, jump.target, true, false, false, true);
    compare(recovered, expected, pcs);
    compare(target.hintSnapshot->history, base, pcs);

    // Removing the flag must retain the finite array semantics. Compare the
    // full encoding and every mask after mixed branch events.
    const std::vector<Addr> small_pcs(pcs.begin(), pcs.begin() + 20);
    HintHistory finite, ideal;
    finite.configure(small_pcs);
    ideal.configure(small_pcs, true);
    for (unsigned event = 0; event < 256; ++event) {
        const auto pc = pcs[event % 32];
        const auto target_pc = 0xa000 | ((event % 4) << 1);
        const bool indirect = (event % 3) != 0;
        const bool call_event = (event % 7) == 0;
        const bool return_event = (event % 11) == 0;
        const bool taken = (event % 13) != 0;
        finite.update(pc, target_pc, indirect, call_event, return_event, taken);
        ideal.update(pc, target_pc, indirect, call_event, return_event, taken);
        compare(ideal, finite, small_pcs);
        const HintSnapshot finite_snapshot(finite, ghr);
        const HintSnapshot ideal_snapshot(ideal, ghr);
        require(ideal_snapshot.historyJson(pc) ==
                finite_snapshot.historyJson(pc),
                "finite and unlimited history encodings match");
        for (unsigned mask = 0; mask < 32; ++mask)
            require(hintHash(pc, ideal_snapshot.contextsFor(pc), mask) ==
                    hintHash(pc, finite_snapshot.contextsFor(pc), mask),
                    "all masks preserve finite hashes and aliases");
    }
    HintHistory no_candidates;
    no_candidates.configure({}, true);
    no_candidates.update(0x1000, 0x8006, true, false, false, true);
    require(no_candidates.local(0x1000) == 0 &&
            no_candidates.globalLow == 3,
            "empty ideal candidate set retains global history semantics");
}

} // namespace gem5::branch_prediction::btb_pred

#endif // __CPU_PRED_BTB_TEST_HINT_HISTORY_IDEAL_TEST_HH__
