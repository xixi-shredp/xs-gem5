#include "cpu/pred/btb/hint_history.hh"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <utility>

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

uint16_t
hintFold14(Addr pc)
{
    uint64_t word = (pc >> 1) & HintAddressWordMask;
    uint16_t result = 0;
    while (word) {
        result ^= word & 0x3fff;
        word >>= 14;
    }
    return result;
}

uint32_t
hintHashV3(Addr pc, const HintV3Contexts &contexts, uint8_t mask)
{
    assert(mask < 16);
    uint32_t key = hintFold28((pc >> 1) & HintAddressWordMask);
    for (unsigned i = 0; i < contexts.size(); ++i) {
        if (mask & (1U << i))
            key ^= hintRotate28(contexts[i], i + 2);
    }
    return key & HintHashMask;
}

void
HintFiniteHistory::configure(const std::vector<Addr> &selected)
{
    fatal_if(selected.size() > locals.size(),
             "Finite Hint supports at most %u CAM-indexed LHRs",
             unsigned(locals.size()));
    pcs = selected;
    fatal_if(!std::is_sorted(pcs.begin(), pcs.end()) ||
             std::adjacent_find(pcs.begin(), pcs.end()) != pcs.end(),
             "Finite Hint CAM PCs must be strictly ascending");
    ith = {};
    calls = {};
    returns = {};
    locals = {};
}

int
HintFiniteHistory::localIndex(Addr pc) const
{
    const auto found = std::lower_bound(pcs.begin(), pcs.end(), pc);
    return found == pcs.end() || *found != pc ? -1 : int(found - pcs.begin());
}

uint64_t
HintFiniteHistory::low64(const Ring &ring) const
{
    uint64_t value = 0;
    for (unsigned age = 0; age < 32; ++age)
        value |= uint64_t(ring.cells[(ring.head - age) & 127]) << (age * 2);
    return value;
}

uint16_t
HintFiniteHistory::low12(const Ring &ring) const
{
    uint16_t value = 0;
    for (unsigned age = 0; age < 6; ++age)
        value |= uint16_t(ring.cells[(ring.head - age) & 127]) << (age * 2);
    return value;
}

uint8_t
HintFiniteHistory::localLow8(unsigned index) const
{
    assert(index < locals.size());
    uint8_t value = 0;
    const auto &ring = locals[index];
    for (unsigned age = 0; age < 4; ++age)
        value |= ring.cells[(ring.head - age) & 7] << (age * 2);
    return value;
}

uint8_t
HintFiniteHistory::localHead(unsigned index) const
{
    assert(index < locals.size());
    return locals[index].head;
}

uint8_t
HintFiniteHistory::localPhase(unsigned index) const
{
    assert(index < locals.size());
    return locals[index].phase;
}

void
HintFiniteHistory::restoreLocalPhase(unsigned index, uint8_t phase)
{
    assert(index < locals.size() && phase < 16);
    locals[index].phase = phase;
    locals[index].head = phase & 7;
}

HintFiniteHistory::Snapshot
HintFiniteHistory::snapshot() const
{
    Snapshot result;
    const std::array<const Ring *, 3> rings = {&ith, &calls, &returns};
    for (unsigned i = 0; i < rings.size(); ++i) {
        result.heads[i] = rings[i]->head;
        result.writes[i] = rings[i]->writes;
        result.low64[i] = low64(*rings[i]);
    }
    result.pathLow12 = {low12(calls), low12(returns)};
    for (unsigned i = 0; i < locals.size(); ++i)
        result.localLow8[i] = localLow8(i);
    return result;
}

void
HintFiniteHistory::restore(const Snapshot &saved)
{
    const std::array<Ring *, 3> rings = {&ith, &calls, &returns};
    for (unsigned i = 0; i < rings.size(); ++i) {
        fatal_if(rings[i]->writes < saved.writes[i] ||
                 rings[i]->writes - saved.writes[i] > 96,
                 "Finite Hint global ring exceeded 96 younger writes");
        rings[i]->head = saved.heads[i];
        rings[i]->writes = saved.writes[i];
    }
    for (unsigned path = 0; path < 2; ++path) {
        auto &ring = *rings[path + 1];
        for (unsigned age = 0; age < 6; ++age) {
            ring.cells[(ring.head - age) & 127] =
                (saved.pathLow12[path] >> (age * 2)) & 3;
        }
    }
    for (unsigned i = 0; i < rings.size(); ++i)
        fatal_if(low64(*rings[i]) != saved.low64[i],
                 "Finite Hint compact global-ring recovery mismatch");
}

bool
HintFiniteHistory::canUpdateAfter(const Snapshot &oldest, bool indirect,
                                 bool call, bool is_return, bool taken) const
{
    if (!taken)
        return true;
    const std::array<const Ring *, 3> rings = {&ith, &calls, &returns};
    const std::array<bool, 3> writes = {
        indirect && !is_return, call, is_return};
    for (unsigned i = 0; i < rings.size(); ++i) {
        fatal_if(rings[i]->writes < oldest.writes[i],
                 "Finite Hint global ring write counter moved before FTQ head");
        if (writes[i] && rings[i]->writes - oldest.writes[i] >= 96)
            return false;
    }
    return true;
}

void
HintFiniteHistory::push(Ring &ring, uint8_t bits)
{
    ring.head = (ring.head + 1) & 127;
    ring.cells[ring.head] = bits & 3;
    ++ring.writes;
}

void
HintFiniteHistory::pushPath(Ring &ring, Addr pc)
{
    push(ring, 0);
    const auto folded = hintFold14(pc);
    for (unsigned age = 0; age < 7; ++age)
        ring.cells[(ring.head - age) & 127] ^=
            (folded >> (age * 2)) & 3;
}

void
HintFiniteHistory::update(Addr pc, Addr target, bool indirect, bool call,
                          bool is_return, bool taken)
{
    if (!taken)
        return;
    if (call)
        pushPath(calls, pc);
    if (is_return)
        pushPath(returns, pc);
    if (!indirect || is_return)
        return;
    const uint8_t bits = (target >> 1) & 3;
    push(ith, bits);
    const int index = localIndex(pc);
    if (index >= 0) {
        auto &ring = locals[index];
        ring.phase = (ring.phase + 1) & 15;
        ring.head = ring.phase & 7;
        ring.cells[ring.head] = bits;
        ++ring.writes;
    }
}

HintV3Contexts
HintFiniteHistory::contexts(const Snapshot &saved, int local_index) const
{
    return {hintFold28(saved.low64[0]),
            local_index >= 0 ? saved.localLow8[local_index] : 0U,
            hintFold28(saved.low64[1]), hintFold28(saved.low64[2])};
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

struct HintHistory::LocalNode
{
    LocalNode(LocalRoot left, LocalRoot right, uint8_t value = 0)
        : left(std::move(left)), right(std::move(right)), value(value)
    {}

    const LocalRoot left;
    const LocalRoot right;
    const uint8_t value;
};

uint8_t
HintHistory::localValue(const LocalRoot &root, size_t first, size_t last,
                       size_t index)
{
    if (!root)
        return 0;
    if (last - first == 1)
        return root->value;
    const auto middle = first + (last - first) / 2;
    return index < middle ? localValue(root->left, first, middle, index) :
                            localValue(root->right, middle, last, index);
}

HintHistory::LocalRoot
HintHistory::appendLocal(const LocalRoot &root, size_t first, size_t last,
                        size_t index, uint8_t bits)
{
    if (last - first == 1) {
        const uint8_t old_value = root ? root->value : 0;
        const uint8_t value = (old_value << 2) | bits;
        if (value == old_value)
            return root;
        return value ? std::make_shared<const LocalNode>(nullptr, nullptr,
                                                        value) : nullptr;
    }
    auto left = root ? root->left : nullptr;
    auto right = root ? root->right : nullptr;
    const auto middle = first + (last - first) / 2;
    if (index < middle)
        left = appendLocal(left, first, middle, index, bits);
    else
        right = appendLocal(right, middle, last, index, bits);
    if (root && left == root->left && right == root->right)
        return root;
    if (!left && !right)
        return nullptr;
    return std::make_shared<const LocalNode>(std::move(left), std::move(right));
}

void
HintHistory::configure(const std::vector<Addr> &pcs, bool ideal)
{
    fatal_if(!ideal && pcs.size() > HintMaxProfilePCs,
             "Hint local history supports at most %u PCs",
             HintMaxProfilePCs);
    auto sorted = pcs;
    std::sort(sorted.begin(), sorted.end());
    fatal_if(std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end(),
             "Duplicate Hint local-history PC");
    localPCs = std::make_shared<const std::vector<Addr>>(std::move(sorted));
    locals.fill(0);
    idealLocals = ideal;
    localRoot.reset();
}

uint8_t
HintHistory::local(Addr pc) const
{
    if (!localPCs)
        return 0;
    const auto it = std::lower_bound(localPCs->begin(), localPCs->end(), pc);
    if (it == localPCs->end() || *it != pc)
        return 0;
    const size_t index = it - localPCs->begin();
    return idealLocals ? localValue(localRoot, 0, localPCs->size(), index) :
                         locals[index];
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
            const size_t index = it - localPCs->begin();
            if (idealLocals) {
                localRoot = appendLocal(localRoot, 0, localPCs->size(),
                                        index, bits);
            } else {
                auto &value = locals[index];
                value = (value << 2) | bits;
            }
        }
    }
}

void
HintHistory::recoverForSquash(const FetchTarget &target, Addr squash_pc,
                              bool control_squash)
{
    fatal_if(!target.hintSnapshot,
             "Hint squash has no block-entry history snapshot");
    fatal_if(!target.hintSnapshot->history,
             "Ideal Hint squash lacks legacy history snapshot");
    *this = *target.hintSnapshot->history;
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
    : history(std::make_unique<HintHistory>(state)), ghr(global_history),
      ghrFold(0)
{
    const auto blocks = historyBlocks(ghr);
    for (unsigned i = 0; i < blocks.size(); ++i)
        ghrFold ^= hintRotate28(hintFold28(blocks[i]), i * HistoryBlockBits);
}

HintSnapshot::HintSnapshot(const HintFiniteHistory &state,
                           bool collect_full_copy)
    : ghrFold(0), finite(state.snapshot()),
      collectHistory(collect_full_copy ?
          std::make_unique<HintFiniteHistory>(state) : nullptr)
{}

HintV3Contexts
HintSnapshot::v3ContextsFor(int local_index) const
{
    assert(finite && local_index >= 0 && local_index < 16);
    return {hintFold28(finite->low64[0]),
            finite->localLow8[local_index],
            hintFold28(finite->low64[1]),
            hintFold28(finite->low64[2])};
}

HintContexts
HintSnapshot::contextsFor(Addr pc) const
{
    assert(history);
    return {ghrFold, history->globalFold(), history->local(pc),
            history->calls.folded, history->returns.folded};
}

std::string
HintSnapshot::historyJson(Addr pc) const
{
    std::ostringstream out;
    if (finite) {
        out << "{\"ith_low64\":\"0x" << std::hex << finite->low64[0]
            << "\",\"call_low64\":\"0x" << finite->low64[1]
            << "\",\"ret_low64\":\"0x" << finite->low64[2]
            << "\"}";
        return out.str();
    }
    assert(history);
    out << "{\"ghr\":\"0x" << std::hex << std::setfill('0');
    const auto blocks = historyBlocks(ghr);
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
        out << std::setw(sizeof(HistoryBlock) * 2) << *it;
    out << "\",\"githr\":\"0x" << std::setw(16) << history->globalHigh
        << std::setw(16) << history->globalLow
        << "\",\"lithr\":\"0x" << std::setw(2)
        << static_cast<unsigned>(history->local(pc)) << "\"";
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
    write_window("call_pc_words_newest_first", history->calls);
    write_window("return_pc_words_newest_first", history->returns);
    out << '}';
    return out.str();
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
