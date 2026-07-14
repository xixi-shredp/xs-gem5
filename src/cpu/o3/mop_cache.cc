/*
 * Copyright (c) 2026 The Regents of The University of California
 * All rights reserved.
 *
 * See src/cpu/o3/mop_cache.hh for license terms.
 */

#include "cpu/o3/mop_cache.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gem5
{

namespace o3
{

bool
MopContext::operator==(const MopContext &other) const
{
    return tid == other.tid && satp == other.satp &&
           vsatp == other.vsatp && hgatp == other.hgatp &&
           privilege == other.privilege && virt == other.virt &&
           vtypeReady == other.vtypeReady && vtype == other.vtype;
}

MopCache::MopCache(const Config &config)
    : config(config), readTokens(config.readPorts),
      fillTokens(config.fillWidth)
{
    if (config.entries == 0) {
        throw std::invalid_argument("MOP cache entries must be non-zero");
    }
    if (config.ways == 0) {
        throw std::invalid_argument("MOP cache ways must be non-zero");
    }
    if (config.entries % config.ways != 0) {
        throw std::invalid_argument(
                "MOP cache entries must be divisible by ways");
    }
    if (config.lookupWidth == 0) {
        throw std::invalid_argument("MOP cache lookup width must be non-zero");
    }
    if (config.readPorts == 0) {
        throw std::invalid_argument("MOP cache read ports must be non-zero");
    }
    if (config.fillWidth == 0) {
        throw std::invalid_argument("MOP cache fill width must be non-zero");
    }

    sets.resize(config.entries / config.ways);
    for (auto &set : sets) {
        set.resize(config.ways);
    }
}

void
MopCache::beginCycle()
{
    readTokens = config.readPorts;
    fillTokens = config.fillWidth;
}

size_t
MopCache::setIndex(Addr pc) const
{
    // RISC-V instructions are at least two-byte aligned.  Discarding that
    // invariant zero bit prevents adjacent instructions from wasting sets.
    return (pc >> 1) % sets.size();
}

MopEntry *
MopCache::find(Addr pc, const MopContext &context)
{
    auto &set = sets[setIndex(pc)];
    auto it = std::find_if(set.begin(), set.end(),
            [pc, &context](const MopEntry &entry) {
                return entry.valid && entry.pc == pc &&
                       entry.context == context;
            });
    return it == set.end() ? nullptr : &*it;
}

void
MopCache::touch(MopEntry &entry)
{
    // A 64-bit sequence cannot wrap in a practical simulation.  Keeping LRU
    // updates O(1) preserves the lookup complexity bound.
    entry.lruSequence = ++lruSequence;
}

MopCache::LookupResult
MopCache::lookupBundle(Addr start_pc, const MopContext &context, Addr end_pc)
{
    LookupResult result;
    result.nextPC = start_pc;
    result.missPC = start_pc;

    if (readTokens == 0) {
        return result;
    }
    --readTokens;
    result.portAvailable = true;

    Addr current_pc = start_pc;
    for (size_t i = 0; i < config.lookupWidth; ++i) {
        if (end_pc != 0 && current_pc >= end_pc) {
            result.reachedEnd = true;
            result.nextPC = current_pc;
            return result;
        }

        MopEntry *entry = find(current_pc, context);
        if (!entry) {
            result.missed = true;
            result.missPC = current_pc;
            result.nextPC = current_pc;
            return result;
        }

        touch(*entry);
        result.entries.push_back(*entry);
        current_pc = entry->nextPC;
        result.nextPC = current_pc;
    }

    if (end_pc != 0 && current_pc >= end_pc) {
        result.reachedEnd = true;
    } else {
        result.widthLimited = true;
    }
    return result;
}

MopCache::FillResult
MopCache::fill(Addr pc, Addr next_pc, const StaticInstPtr &static_inst,
               bool compressed, const MopContext &context)
{
    if (fillTokens == 0) {
        return FillResult::NoBandwidth;
    }
    --fillTokens;

    if (MopEntry *entry = find(pc, context)) {
        entry->nextPC = next_pc;
        entry->staticInst = static_inst;
        entry->compressed = compressed;
        touch(*entry);
        return FillResult::Updated;
    }

    auto &set = sets[setIndex(pc)];
    auto victim = std::find_if(set.begin(), set.end(),
            [](const MopEntry &entry) { return !entry.valid; });
    FillResult result = FillResult::Filled;
    if (victim == set.end()) {
        victim = std::min_element(set.begin(), set.end(),
                [](const MopEntry &lhs, const MopEntry &rhs) {
                    return lhs.lruSequence < rhs.lruSequence;
                });
        result = FillResult::Evicted;
    } else {
        ++validEntries;
    }

    victim->pc = pc;
    victim->nextPC = next_pc;
    victim->staticInst = static_inst;
    victim->compressed = compressed;
    victim->context = context;
    victim->valid = true;
    touch(*victim);
    return result;
}

size_t
MopCache::invalidate()
{
    const size_t invalidated = validEntries;
    for (auto &set : sets) {
        for (auto &entry : set) {
            entry = MopEntry{};
        }
    }
    validEntries = 0;
    lruSequence = 0;
    return invalidated;
}

} // namespace o3
} // namespace gem5
