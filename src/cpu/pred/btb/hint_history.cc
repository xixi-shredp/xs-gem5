#include "cpu/pred/btb/hint_history.hh"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

#include "base/logging.hh"
#include "cpu/pred/btb/common.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

uint32_t
hintFold28(uint64_t value)
{
    return (value ^ (value >> 28) ^ (value >> 56)) & HintHashMask;
}

uint32_t
hintRotate28(uint32_t value, unsigned amount)
{
    value &= HintHashMask;
    amount %= HintHashBits;
    if (!amount)
        return value;
    return ((value << amount) | (value >> (HintHashBits - amount))) &
           HintHashMask;
}

uint32_t
hintHash(Addr pc, const HintContexts &contexts, uint8_t mask)
{
    uint32_t key = hintFold28((pc >> 1) & HintAddressWordMask);
    for (unsigned i = 0; i < contexts.size(); ++i) {
        if (mask & (1U << i))
            key ^= hintRotate28(contexts[i], i + 1);
    }
    return key & HintHashMask;
}

uint64_t
HintPCWindow::atAge(unsigned age) const
{
    assert(age < count);
    return words[(head + age) % HintPCEvents];
}

void
HintPCWindow::push(Addr pc)
{
    if (count == HintPCEvents) {
        folded ^= hintRotate28(hintFold28(atAge(HintPCEvents - 1)),
                               HintPCEvents - 1);
    } else {
        ++count;
    }
    folded = hintRotate28(folded, 1);
    head = (head + HintPCEvents - 1) % HintPCEvents;
    words[head] = (pc >> 1) & HintAddressWordMask;
    folded ^= hintFold28(words[head]);
}

void
HintHistory::configure(const std::vector<Addr> &pcs)
{
    fatal_if(pcs.size() > HintMaxProfilePCs,
             "Hint local history supports at most %u PCs",
             HintMaxProfilePCs);
    auto sorted = pcs;
    std::sort(sorted.begin(), sorted.end());
    fatal_if(std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end(),
             "Duplicate Hint local-history PC");
    localPCs = std::make_shared<const std::vector<Addr>>(std::move(sorted));
    locals.fill(0);
}

uint8_t
HintHistory::local(Addr pc) const
{
    if (!localPCs)
        return 0;
    const auto it = std::lower_bound(localPCs->begin(), localPCs->end(), pc);
    if (it == localPCs->end() || *it != pc)
        return 0;
    return locals[it - localPCs->begin()];
}

uint32_t
HintHistory::globalFold() const
{
    return hintFold28(globalLow) ^ hintRotate28(hintFold28(globalHigh), 64);
}

void
HintHistory::update(Addr pc, Addr target, bool indirect, bool call,
                    bool is_return, bool taken)
{
    if (!taken)
        return;
    if (call)
        calls.push(pc);
    if (is_return)
        returns.push(pc);
    // RISC-V pop-and-push calls update both PC windows, but returns are never
    // indirect-target events, even when the instruction also has IsCall.
    if (!indirect || is_return)
        return;
    const uint8_t bits = (target >> 1) & 3;
    globalHigh = (globalHigh << 2) | (globalLow >> 62);
    globalLow = (globalLow << 2) | bits;
    if (localPCs) {
        const auto it = std::lower_bound(localPCs->begin(), localPCs->end(), pc);
        if (it != localPCs->end() && *it == pc) {
            auto &value = locals[it - localPCs->begin()];
            value = (value << 2) | bits;
        }
    }
}

void
HintHistory::recoverForSquash(const FetchTarget &target, Addr squash_pc,
                              bool control_squash)
{
    fatal_if(!target.hintSnapshot,
             "Hint squash has no block-entry history snapshot");
    *this = target.hintSnapshot->history;
    // Only actual retired events strictly before the recovery boundary are
    // retained. Equal-PC exclusion prevents applying the squash branch twice.
    for (const auto &retired : target.retiredControls) {
        const auto &branch = retired.branch;
        if (branch.pc < squash_pc) {
            update(branch.pc, branch.target, branch.isIndirect,
                   branch.isCall, branch.isReturn, retired.taken);
        }
    }
    if (control_squash) {
        const auto &branch = target.exeBranchInfo;
        update(branch.pc, branch.target, branch.isIndirect,
               branch.isCall, branch.isReturn, target.exeTaken);
    }
}

namespace
{

using HistoryBlock = boost::dynamic_bitset<>::block_type;
constexpr unsigned HistoryBlockBits = sizeof(HistoryBlock) * 8;
using HistoryBlocks = std::array<HistoryBlock,
    (HintGHRBits + HistoryBlockBits - 1) / HistoryBlockBits>;

HistoryBlocks
historyBlocks(const boost::dynamic_bitset<> &history)
{
    fatal_if(history.size() != HintGHRBits,
             "Hint requires the existing %u-bit GHR (got %u)",
             HintGHRBits, history.size());
    HistoryBlocks result{};
    boost::to_block_range(history, result.begin());
    return result;
}

} // anonymous namespace

HintSnapshot::HintSnapshot(const HintHistory &state,
                           const boost::dynamic_bitset<> &global_history)
    : history(state), ghr(global_history), ghrFold(0)
{
    const auto blocks = historyBlocks(ghr);
    for (unsigned i = 0; i < blocks.size(); ++i)
        ghrFold ^= hintRotate28(hintFold28(blocks[i]), i * HistoryBlockBits);
}

HintContexts
HintSnapshot::contextsFor(Addr pc) const
{
    return {ghrFold, history.globalFold(), history.local(pc),
            history.calls.folded, history.returns.folded};
}

std::string
HintSnapshot::historyJson(Addr pc) const
{
    std::ostringstream out;
    out << "{\"ghr\":\"0x" << std::hex << std::setfill('0');
    const auto blocks = historyBlocks(ghr);
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
        out << std::setw(sizeof(HistoryBlock) * 2) << *it;
    out << "\",\"githr\":\"0x" << std::setw(16) << history.globalHigh
        << std::setw(16) << history.globalLow
        << "\",\"lithr\":\"0x" << std::setw(2)
        << static_cast<unsigned>(history.local(pc)) << "\"";
    const auto write_window = [&out](const char *name,
                                     const HintPCWindow &window) {
        out << ",\"" << name << "\":[";
        for (unsigned age = 0; age < window.count; ++age) {
            if (age)
                out << ',';
            out << "\"0x" << window.atAge(age) << "\"";
        }
        out << ']';
    };
    write_window("call_pc_words_newest_first", history.calls);
    write_window("return_pc_words_newest_first", history.returns);
    out << '}';
    return out.str();
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
