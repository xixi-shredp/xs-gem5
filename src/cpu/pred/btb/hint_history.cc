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
hintFold128(const std::array<uint64_t, 2> &value)
{
    return hintFold28(value[0]) ^ hintRotate28(hintFold28(value[1]), 8);
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

std::array<uint64_t, 2>
HintFiniteHistory::low128(const Ring &ring) const
{
    std::array<uint64_t, 2> value{};
    for (unsigned age = 0; age < 64; ++age) {
        value[age / 32] |=
            uint64_t(ring.cells[(ring.head - age) & 127]) <<
            ((age % 32) * 2);
    }
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
    return localLow16(index) & 0xff;
}

uint16_t
HintFiniteHistory::localLow16(unsigned index) const
{
    assert(index < locals.size());
    uint16_t value = 0;
    const auto &ring = locals[index];
    for (unsigned age = 0; age < 8; ++age)
        value |= uint16_t(ring.cells[(ring.head - age) & 15]) << (age * 2);
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
    assert(index < locals.size() && phase < 32);
    locals[index].phase = phase;
    locals[index].head = phase & 15;
}

HintFiniteHistory::Snapshot
HintFiniteHistory::snapshot() const
{
    Snapshot result;
    const std::array<const Ring *, 3> rings = {&ith, &calls, &returns};
    for (unsigned i = 0; i < rings.size(); ++i) {
        result.heads[i] = rings[i]->head;
        result.writes[i] = rings[i]->writes;
        result.low128[i] = low128(*rings[i]);
    }
    result.pathLow12 = {low12(calls), low12(returns)};
    for (unsigned i = 0; i < locals.size(); ++i) {
        result.localLow16[i] = localLow16(i);
        result.localPhases[i] = localPhase(i);
    }
    return result;
}

void
HintFiniteHistory::restore(const Snapshot &saved)
{
    const std::array<Ring *, 3> rings = {&ith, &calls, &returns};
    for (unsigned i = 0; i < rings.size(); ++i) {
        fatal_if(rings[i]->writes < saved.writes[i] ||
                 rings[i]->writes - saved.writes[i] > 64,
                 "Finite Hint global ring exceeded 64 younger writes");
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
        fatal_if(low128(*rings[i]) != saved.low128[i],
                 "Finite Hint compact global-ring recovery mismatch");
    // Restore every selector on any redirect, including a non-H2P branch.
    // The caller's admission policy protects read-enabled LHR windows. An
    // immutable mask that excludes LHR can bypass that limit; its old cells
    // are not guaranteed to match an observer after arbitrary younger writes.
    for (unsigned i = 0; i < locals.size(); ++i)
        restoreLocalPhase(i, saved.localPhases[i]);
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
        if (writes[i] && rings[i]->writes - oldest.writes[i] >= 64)
            return false;
    }
    return true;
}

void
HintFiniteHistory::push(Ring &ring, uint8_t bits)
{
    ring.head = uint8_t(ring.head + 1);
    ring.cells[ring.head & 127] = bits & 3;
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
        ring.phase = (ring.phase + 1) & 31;
        ring.head = ring.phase & 15;
        ring.cells[ring.head] = bits;
        ++ring.writes;
    }
}

HintV3Contexts
HintFiniteHistory::contexts(const Snapshot &saved, int local_index) const
{
    return {hintFold128(saved.low128[0]),
            local_index >= 0 ? saved.localLow16[local_index] : 0U,
            hintFold128(saved.low128[1]), hintFold128(saved.low128[2])};
}

void
verifyHintContextForkHistory()
{
    const auto require = [](bool ok, const char *message) {
        fatal_if(!ok, "Hint fork history self-test: %s", message);
    };
    // Independent logical shift registers, rather than a second ring reader.
    using Wide = std::array<uint64_t, 4>;
    const auto shift = [](Wide &value, uint8_t bits) {
        for (unsigned i = value.size() - 1; i > 0; --i)
            value[i] = (value[i] << 2) | (value[i - 1] >> 62);
        value[0] = (value[0] << 2) | (bits & 3);
    };
    const auto fold_reference = [](const std::array<uint64_t, 2> &value) {
        uint32_t result = 0;
        for (unsigned bit = 0; bit < 128; ++bit) {
            if ((value[bit / 64] >> (bit % 64)) & 1)
                result ^= uint32_t(1) << (bit % 28);
        }
        return result;
    };
    // Exercise both sides of the 64-bit word boundary and the highest bit.
    for (unsigned bit = 0; bit < 128; ++bit) {
        std::array<uint64_t, 2> value{};
        value[bit / 64] = uint64_t(1) << (bit % 64);
        require(hintFold128(value) == fold_reference(value),
                "128-bit fold bit placement");
    }
    const std::array<uint64_t, 2> mixed = {
        0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    require(hintFold128(mixed) == fold_reference(mixed),
            "128-bit mixed fold");
    const HintV3Contexts hash_contexts = {
        hintFold128(mixed), 0x8001, hintFold128({mixed[1], mixed[0]}),
        hintFold128({0x1, 0x8000000000000000ULL})};
    const Addr hash_pc = 0x123456789abcULL;
    for (unsigned mask = 0; mask < 16; ++mask) {
        uint32_t expected = 0;
        const auto pc_word = (hash_pc >> 1) & HintAddressWordMask;
        for (unsigned bit = 0; bit < 47; ++bit)
            if ((pc_word >> bit) & 1)
                expected ^= uint32_t(1) << (bit % 28);
        for (unsigned context = 0; context < 4; ++context) {
            if (!(mask & (1U << context))) continue;
            for (unsigned bit = 0; bit < 28; ++bit)
                if ((hash_contexts[context] >> bit) & 1)
                    expected ^= uint32_t(1) << ((bit + context + 2) % 28);
        }
        require(hintHashV3(hash_pc, hash_contexts, mask) == expected,
                "all-mask hash encoding");
    }

    std::vector<Addr> pcs;
    for (unsigned index = 0; index < 16; ++index)
        pcs.push_back(0x1000 + index * 0x100);
    HintFiniteHistory history;
    history.configure(pcs);
    Wide ith{}, calls{}, returns{};
    std::array<uint32_t, 16> local{};
    std::array<unsigned, 16> local_events{};
    unsigned ith_events = 0;
    for (unsigned event = 0; event < 640; ++event) {
        const unsigned selector = event % 16;
        const Addr pc = pcs[selector];
        const Addr target = 0x2000 + ((event * 13 + 3) & 0x3f) * 2;
        const bool call = event % 3 == 0;
        const bool is_return = event % 5 == 0;
        const bool indirect = event % 2 == 0;
        history.update(pc, target, indirect, call, is_return, true);
        if (call) {
            shift(calls, 0);
            calls[0] ^= hintFold14(pc);
        }
        if (is_return) {
            shift(returns, 0);
            returns[0] ^= hintFold14(pc);
        }
        if (indirect && !is_return) {
            const uint8_t bits = (target >> 1) & 3;
            shift(ith, bits);
            local[selector] = (local[selector] << 2) | bits;
            ++local_events[selector];
            ++ith_events;
        }
        const auto snapshot = history.snapshot();
        require(snapshot.low128[0] ==
                    std::array<uint64_t, 2>{ith[0], ith[1]} &&
                snapshot.low128[1] ==
                    std::array<uint64_t, 2>{calls[0], calls[1]} &&
                snapshot.low128[2] ==
                    std::array<uint64_t, 2>{returns[0], returns[1]},
                "global ring versus logical shifts across wraps");
        for (unsigned index = 0; index < 16; ++index) {
            require(history.localLow16(index) == (local[index] & 0xffff),
                    "local 16-bit read versus 32-bit physical shifts");
            require(history.localPhase(index) ==
                        ((31 + local_events[index]) & 31),
                    "local five-bit phase wrap");
        }
        require(history.ith.head == uint8_t(255 + ith_events),
                "global eight-bit phase wrap");
    }
    const auto saved = history.snapshot();
    for (unsigned index = 0; index < 16; ++index)
        for (unsigned write = 0; write < 4; ++write)
            history.update(pcs[index], 0x4000 + write * 2,
                           true, false, false, true);
    history.restore(saved);
    require(history.snapshot().localLow16 == saved.localLow16 &&
            history.snapshot().localPhases == saved.localPhases,
            "all selectors recover on a non-H2P redirect");
    // A correct redirected event is reapplied from the entry state; repeat
    // rollback must restore the same window, not accumulate its target twice.
    for (unsigned retry = 0; retry < 2; ++retry) {
        history.update(pcs[0], 0x5006, true, false, false, true);
        require(history.localLow16(0) ==
                    uint16_t((saved.localLow16[0] << 2) | 3),
                "correct target replay");
        history.restore(saved);
        require(history.localLow16(0) == saved.localLow16[0],
                "repeated redirect does not duplicate an event");
    }
    // The guard band protects the last eight old local cells. Admission is
    // still four; this verifies physical capacity without changing policy.
    for (unsigned write = 0; write < 8; ++write)
        history.update(pcs[0], 0x6000 + write * 2,
                       true, false, false, true);
    history.restore(saved);
    require(history.localLow16(0) == saved.localLow16[0],
            "eight-write physical local guard band");
    // Independently exercise the global 64-write protection on all rings.
    const Addr untracked = 0xa000;
    for (unsigned write = 0; write < 64; ++write) {
        require(history.canUpdateAfter(saved, true, true, false, true) &&
                history.canUpdateAfter(saved, false, false, true, true),
                "global admission before the last safe write");
        history.update(untracked, 0xb000 + write * 2,
                       true, true, false, true);
        history.update(untracked + 2, 0xb006,
                       false, false, true, true);
    }
    require(!history.canUpdateAfter(saved, true, false, false, true) &&
            !history.canUpdateAfter(saved, false, true, false, true) &&
            !history.canUpdateAfter(saved, false, false, true, true) &&
            history.canUpdateAfter(saved, false, false, false, true) &&
            history.canUpdateAfter(saved, true, true, true, false),
            "global write 65 is blocked without blocking a non-write");
    history.restore(saved);
    require(history.snapshot().low128 == saved.low128,
            "global heads plus path low12 recover 128-bit windows");
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
    return {hintFold128(finite->low128[0]),
            finite->localLow16[local_index],
            hintFold128(finite->low128[1]),
            hintFold128(finite->low128[2])};
}

HintContexts
HintSnapshot::contextsFor(Addr pc) const
{
    assert(history);
    return {ghrFold, history->globalFold(), history->local(pc),
            history->calls.folded, history->returns.folded};
}

std::string
HintSnapshot::historyJson(Addr pc, int local_index) const
{
    std::ostringstream out;
    if (finite) {
        out << '{' << std::hex << std::setfill('0');
        const std::array<const char *, 3> names = {
            "ith_low128", "call_low128", "ret_low128"};
        for (unsigned i = 0; i < names.size(); ++i) {
            if (i) out << ',';
            out << '"' << names[i] << "\":\"0x"
                << std::setw(16) << finite->low128[i][1]
                << std::setw(16) << finite->low128[i][0] << '"';
        }
        const int index = local_index >= 0 ? local_index :
            (collectHistory ? collectHistory->localIndex(pc) : -1);
        // Replay supplies the immutable CAM selector; collect can also find
        // it in the observer's PC mapping. A missing index means no local PC.
        assert(index < 16);
        if (index >= 0)
            out << ",\"lhr_low16\":\"0x" << std::setw(4)
                << finite->localLow16[index] << '"';
        out << '}';
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
