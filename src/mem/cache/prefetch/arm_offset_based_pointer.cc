/*
 * Copyright (c) 2026
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

#include "mem/cache/prefetch/arm_offset_based_pointer.hh"

#include <algorithm>
#include <cstring>
#include <limits>

#include "params/ARMOffsetBasedPointerPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

ARMOffsetBasedPointerPrefetcher::ARMOffsetBasedPointerPrefetcher(
    const ARMOffsetBasedPointerPrefetcherParams &p)
    : Queued(p),
      historyEntries(p.history_entries),
      pointerCacheEntries(p.pointer_cache_entries),
      structureEntries(p.structure_entries),
      pendingEntries(p.pending_entries),
      spatialEntries(p.spatial_entries),
      recentPointerSearchEntries(p.recent_pointer_search_entries),
      maxElementBytes(p.max_element_bytes),
      maxPointerOffsetBytes(p.max_pointer_offset_bytes),
      maxPointerTargetOffsetBytes(p.max_pointer_target_offset_bytes),
      minPointerAddress(p.min_pointer_address),
      pointerBytes(p.pointer_bytes),
      pointerMswMatchBits(p.pointer_msw_match_bits),
      pointerAlignBits(p.pointer_align_bits),
      confidenceBits(p.confidence_bits),
      minConfidence(p.min_confidence),
      degree(p.degree),
      lookahead(p.lookahead),
      scanCachelineOnFill(p.scan_cacheline_on_fill),
      enableTableDetector(p.enable_table_detector),
      enableLinkedListDetector(p.enable_linked_list_detector),
      enablePointerTableDetector(p.enable_pointer_table_detector),
      byteOrder(p.sys->getGuestByteOrder()),
      maxConfidence((1u << std::min(confidenceBits, 31u)) - 1)
{}

ARMOffsetBasedPointerPrefetcher::HistoryEntry *
ARMOffsetBasedPointerPrefetcher::findHistoryEntry(Addr pc)
{
    for (auto &entry : historyBuffer) {
        if (entry.valid && entry.pc == pc) {
            return &entry;
        }
    }
    return nullptr;
}

const ARMOffsetBasedPointerPrefetcher::HistoryEntry *
ARMOffsetBasedPointerPrefetcher::findHistoryEntry(Addr pc) const
{
    for (const auto &entry : historyBuffer) {
        if (entry.valid && entry.pc == pc) {
            return &entry;
        }
    }
    return nullptr;
}

ARMOffsetBasedPointerPrefetcher::HistoryEntry &
ARMOffsetBasedPointerPrefetcher::getHistoryEntry(Addr pc)
{
    if (HistoryEntry *entry = findHistoryEntry(pc)) {
        return *entry;
    }

    if (historyBuffer.size() >= historyEntries && !historyBuffer.empty()) {
        historyBuffer.pop_front();
    }

    historyBuffer.push_back(HistoryEntry());
    HistoryEntry &entry = historyBuffer.back();
    entry.pc = pc;
    entry.valid = true;
    return entry;
}

ARMOffsetBasedPointerPrefetcher::StructureEntry &
ARMOffsetBasedPointerPrefetcher::getStructureEntry(Addr triggering_pc,
                                                   Addr triggered_pc,
                                                   RelationType type,
                                                   int64_t offset1,
                                                   int64_t offset2)
{
    for (auto &entry : dataStructureTable) {
        if (entry.triggeringPc == triggering_pc &&
            entry.triggeredPc == triggered_pc && entry.type == type &&
            entry.offset1 == offset1 && entry.offset2 == offset2) {
            entry.confidence = std::min(entry.confidence + 1, maxConfidence);
            return entry;
        }
    }

    if (dataStructureTable.size() >= structureEntries &&
        !dataStructureTable.empty()) {
        dataStructureTable.pop_front();
    }

    dataStructureTable.push_back(StructureEntry());
    StructureEntry &entry = dataStructureTable.back();
    entry.triggeringPc = triggering_pc;
    entry.triggeredPc = triggered_pc;
    entry.type = type;
    entry.offset1 = offset1;
    entry.offset2 = offset2;
    entry.confidence = 1;
    return entry;
}

bool
ARMOffsetBasedPointerPrefetcher::addOffset(Addr base, int64_t offset,
                                           Addr &result) const
{
    if (offset >= 0) {
        const Addr positive = static_cast<Addr>(offset);
        if (base > std::numeric_limits<Addr>::max() - positive) {
            return false;
        }
        result = base + positive;
    } else {
        const Addr negative = static_cast<Addr>(-offset);
        if (base < negative) {
            return false;
        }
        result = base - negative;
    }
    return true;
}

bool
ARMOffsetBasedPointerPrefetcher::diffWithin(Addr a, Addr b, Addr threshold,
                                            int64_t &diff) const
{
    const Addr distance = a >= b ? a - b : b - a;
    if (distance > threshold ||
        distance > static_cast<Addr>(std::numeric_limits<int64_t>::max())) {
        return false;
    }

    diff = a >= b ? static_cast<int64_t>(distance)
                  : -static_cast<int64_t>(distance);
    return true;
}

bool
ARMOffsetBasedPointerPrefetcher::looksLikePointer(Addr pointer,
                                                  Addr reference) const
{
    if (pointer < minPointerAddress) {
        return false;
    }

    if (pointerAlignBits >= sizeof(Addr) * 8) {
        return false;
    }

    const Addr align_mask = (Addr(1) << pointerAlignBits) - 1;
    if ((pointer & align_mask) != 0) {
        return false;
    }

    if (pointerMswMatchBits == 0 || reference == 0) {
        return true;
    }

    const unsigned shift = 64 - std::min(pointerMswMatchBits, 63u);
    return (pointer >> shift) == (reference >> shift);
}

bool
ARMOffsetBasedPointerPrefetcher::readPointerValue(const PrefetchInfo &pfi,
                                                  Addr &value) const
{
    if (pfi.isCacheMiss() || pfi.isWrite() || pfi.getDataPtr() == nullptr) {
        return false;
    }

    const uint8_t *data =
        reinterpret_cast<const uint8_t *>(pfi.getDataPtr());

    switch (pfi.getSize()) {
        case sizeof(uint32_t): {
            uint32_t raw = 0;
            std::memcpy(&raw, data, sizeof(raw));
            value = byteOrder == ByteOrder::big ? betoh(raw) : letoh(raw);
            return looksLikePointer(value, 0);
        }
        case sizeof(uint64_t): {
            uint64_t raw = 0;
            std::memcpy(&raw, data, sizeof(raw));
            value = byteOrder == ByteOrder::big ? betoh(raw) : letoh(raw);
            return looksLikePointer(value, 0);
        }
        default:
            return false;
    }
}

bool
ARMOffsetBasedPointerPrefetcher::readPointerFromLine(const uint8_t *data,
                                                     unsigned size,
                                                     unsigned offset,
                                                     Addr &value) const
{
    if (pointerBytes != sizeof(uint32_t) && pointerBytes != sizeof(uint64_t)) {
        return false;
    }

    if (offset + pointerBytes > size) {
        return false;
    }

    if (pointerBytes == sizeof(uint32_t)) {
        uint32_t raw = 0;
        std::memcpy(&raw, data + offset, sizeof(raw));
        value = byteOrder == ByteOrder::big ? betoh(raw) : letoh(raw);
    } else {
        uint64_t raw = 0;
        std::memcpy(&raw, data + offset, sizeof(raw));
        value = byteOrder == ByteOrder::big ? betoh(raw) : letoh(raw);
    }

    return true;
}

bool
ARMOffsetBasedPointerPrefetcher::findPointerCacheEntry(Addr pointer_addr,
                                                       bool secure,
                                                       Addr &target) const
{
    for (const auto &entry : pointerCache) {
        if (entry.pointerAddr == pointer_addr && entry.secure == secure) {
            target = entry.target;
            return true;
        }
    }
    return false;
}

Addr
ARMOffsetBasedPointerPrefetcher::findPointerReferenceAddress(Addr pointer,
                                                             bool secure) const
{
    for (const auto &history : historyBuffer) {
        if (!history.valid || history.lastTrigger == 0 ||
            history.secure != secure) {
            continue;
        }

        if (looksLikePointer(pointer, history.lastTrigger)) {
            return history.lastTrigger;
        }
    }

    return 0;
}

bool
ARMOffsetBasedPointerPrefetcher::addPointerCacheEntry(Addr pointer_addr,
                                                      Addr target,
                                                      Addr reference_addr,
                                                      bool secure)
{
    if (!looksLikePointer(target, reference_addr)) {
        return false;
    }

    for (auto it = pointerCache.begin(); it != pointerCache.end(); ++it) {
        if (it->pointerAddr == pointer_addr && it->secure == secure) {
            pointerCache.erase(it);
            break;
        }
    }

    pointerCache.push_front({pointer_addr, target, secure});
    while (pointerCache.size() > pointerCacheEntries) {
        pointerCache.pop_back();
    }
    return true;
}

void
ARMOffsetBasedPointerPrefetcher::scanLineForPointers(PacketPtr pkt)
{
    if (!scanCachelineOnFill || !pkt->hasData() ||
        (pointerBytes != sizeof(uint32_t) &&
         pointerBytes != sizeof(uint64_t))) {
        return;
    }

    const Addr base_addr = blockAddress(pkt->getAddr());
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    HistoryEntry *history =
        pkt->req->hasPC() ? &getHistoryEntry(pkt->req->getPC()) : nullptr;

    for (unsigned offset = 0; offset + pointerBytes <= blkSize;
         offset += pointerBytes) {
        Addr value = 0;
        const Addr pointer_addr = base_addr + offset;
        const bool secure = pkt->isSecure();
        if (readPointerFromLine(data, blkSize, offset, value) &&
            addPointerCacheEntry(pointer_addr, value,
                                 findPointerReferenceAddress(value, secure),
                                 secure)) {
            if (history != nullptr) {
                history->secure = secure;
                learnPointerTable(history->pc, pointer_addr, value, *history);
            }
        }
    }
}

void
ARMOffsetBasedPointerPrefetcher::learnSpatialOffsets(Addr current_pc,
                                                     Addr current_trigger,
                                                     bool secure)
{
    if (spatialEntries == 0) {
        return;
    }

    for (auto &history : historyBuffer) {
        if (!history.valid || history.pc == current_pc ||
            history.lastTrigger == 0 || history.secure != secure) {
            continue;
        }

        int64_t offset = 0;
        if (!diffWithin(current_trigger, history.lastTrigger, maxElementBytes,
                        offset) ||
            offset == 0) {
            continue;
        }

        auto &offsets = history.spatialOffsets;
        if (std::find(offsets.begin(), offsets.end(), offset) !=
            offsets.end()) {
            continue;
        }

        if (offsets.size() >= spatialEntries && !offsets.empty()) {
            offsets.erase(offsets.begin());
        }
        offsets.push_back(offset);
    }
}

void
ARMOffsetBasedPointerPrefetcher::learnTable(Addr current_pc,
                                            Addr current_trigger,
                                            HistoryEntry &history)
{
    int64_t offset1 = 0;
    if (!enableTableDetector ||
        !diffWithin(current_trigger, history.lastTrigger, maxElementBytes,
                    offset1) ||
        offset1 == 0) {
        return;
    }

    getStructureEntry(history.pc, current_pc, RelationType::Table, offset1, 0);
}

void
ARMOffsetBasedPointerPrefetcher::learnLinkedList(Addr current_pc,
                                                 Addr current_trigger,
                                                 const HistoryEntry &history,
                                                 bool secure)
{
    if (!enableLinkedListDetector || !history.valid ||
        history.lastTrigger == 0 || history.secure != secure) {
        return;
    }

    unsigned searched = 0;
    for (const auto &ptr : pointerCache) {
        if (ptr.secure != secure) {
            continue;
        }
        if (searched++ >= recentPointerSearchEntries) {
            break;
        }

        int64_t offset1 = 0;
        int64_t offset2 = 0;
        if (diffWithin(ptr.pointerAddr, history.lastTrigger,
                       maxPointerOffsetBytes, offset1) &&
            diffWithin(current_trigger, ptr.target,
                       maxPointerTargetOffsetBytes, offset2)) {
            getStructureEntry(history.pc, current_pc, RelationType::LinkedList,
                              offset1, offset2);
        }
    }
}

void
ARMOffsetBasedPointerPrefetcher::learnPointerTarget(Addr current_pc,
                                                    Addr current_trigger,
                                                    bool secure)
{
    if (!enablePointerTableDetector) {
        return;
    }

    for (auto &history : historyBuffer) {
        if (!history.valid || history.secure != secure ||
            !history.lastPointerValid || !history.pointerStrideValid) {
            continue;
        }

        int64_t offset2 = 0;
        if (diffWithin(current_trigger, history.lastPointerTarget,
                       maxPointerTargetOffsetBytes, offset2)) {
            getStructureEntry(history.pc, current_pc,
                              RelationType::PointerTable,
                              history.pointerStride, offset2);
        }
    }
}

void
ARMOffsetBasedPointerPrefetcher::learnPointerTable(Addr pc, Addr pointer_addr,
                                                   Addr pointer_target,
                                                   HistoryEntry &history)
{
    if (!enablePointerTableDetector) {
        return;
    }

    if (history.lastPointerValid) {
        int64_t stride = 0;
        if (diffWithin(pointer_addr, history.lastPointerAddr,
                       maxPointerOffsetBytes, stride) &&
            stride != 0) {
            history.pointerStride = stride;
            history.pointerStrideValid = true;
        }
    }

    history.lastPointerAddr = pointer_addr;
    history.lastPointerTarget = pointer_target;
    history.lastPointerValid = true;
}

void
ARMOffsetBasedPointerPrefetcher::appendDataPrefetchAddresses(
    Addr addr, Addr triggered_pc, std::vector<AddrPriority> &addresses)
{
    appendDataPrefetchAddresses(addr, snapshotSpatialOffsets(triggered_pc),
                                addresses);
}

void
ARMOffsetBasedPointerPrefetcher::appendDataPrefetchAddresses(
    Addr addr, const std::vector<int64_t> &spatial_offsets,
    std::vector<AddrPriority> &addresses)
{
    addresses.push_back(AddrPriority(
        blockAddress(addr), 0,
        PrefetchSourceType::ARMOffsetBasedPointer));

    for (const auto offset : spatial_offsets) {
        Addr spatial_addr = 0;
        if (addOffset(addr, offset, spatial_addr)) {
            addresses.push_back(AddrPriority(
                blockAddress(spatial_addr), 0,
                PrefetchSourceType::ARMOffsetBasedPointer));
        }
    }
}

std::vector<int64_t>
ARMOffsetBasedPointerPrefetcher::snapshotSpatialOffsets(
    Addr triggered_pc) const
{
    const HistoryEntry *history = findHistoryEntry(triggered_pc);
    if (history == nullptr) {
        return {};
    }
    return history->spatialOffsets;
}

bool
ARMOffsetBasedPointerPrefetcher::issueComposedLookahead(
    Addr trigger_addr, Addr triggering_pc, unsigned depth, bool secure,
    std::vector<AddrPriority> &addresses, const CacheAccessor &cache)
{
    if (depth >= lookahead) {
        return false;
    }

    bool issued = false;
    for (const auto &next_structure : dataStructureTable) {
        if (next_structure.triggeringPc != triggering_pc ||
            next_structure.confidence < minConfidence) {
            continue;
        }

        if (next_structure.type == RelationType::Table) {
            Addr next = trigger_addr;
            for (unsigned d = 1; d <= degree; ++d) {
                if (!addOffset(next, next_structure.offset1, next)) {
                    break;
                }
                appendDataPrefetchAddresses(next, next_structure.triggeredPc,
                                            addresses);
                issued = true;
            }
        } else if (next_structure.type == RelationType::LinkedList ||
                   next_structure.type == RelationType::PointerTable) {
            Addr next_pointer = 0;
            if (!addOffset(trigger_addr, next_structure.offset1,
                           next_pointer)) {
                continue;
            }

            if (resolvePointerCacheHit(next_pointer, next_structure, depth,
                                       secure, addresses, cache)) {
                issued = true;
            } else if (cache.inCache(blockAddress(next_pointer), secure)) {
                if (resolveResidentPointerBlock(next_pointer, next_structure,
                                                depth, secure, addresses,
                                                cache)) {
                    issued = true;
                }
            } else if (!cache.inCache(blockAddress(next_pointer), secure)) {
                addresses.push_back(
                    AddrPriority(blockAddress(next_pointer), 0,
                                 PrefetchSourceType::ARMOffsetBasedPointer));
                recordPendingPointer(next_pointer, next_structure, depth,
                                     secure);
                issued = true;
            }
        }
    }

    return issued;
}

bool
ARMOffsetBasedPointerPrefetcher::queueComposedLookahead(
    const PacketPtr &pkt, const PrefetchInfo &source, Addr trigger_addr,
    Addr triggering_pc, unsigned depth, int32_t priority,
    const CacheAccessor &cache)
{
    if (depth >= lookahead) {
        return false;
    }

    bool issued = false;
    for (const auto &next_structure : dataStructureTable) {
        if (next_structure.triggeringPc != triggering_pc ||
            next_structure.confidence < minConfidence) {
            continue;
        }

        if (next_structure.type == RelationType::Table) {
            Addr next = trigger_addr;
            for (unsigned d = 1; d <= degree; ++d) {
                if (!addOffset(next, next_structure.offset1, next)) {
                    break;
                }
                queueDataPrefetches(pkt, source, next,
                                    next_structure.triggeredPc, priority,
                                    cache);
                issued = true;
            }
        } else if (next_structure.type == RelationType::LinkedList ||
                   next_structure.type == RelationType::PointerTable) {
            Addr next_pointer = 0;
            if (!addOffset(trigger_addr, next_structure.offset1,
                           next_pointer)) {
                continue;
            }

            queuePointerRead(pkt, source, next_pointer, next_structure, depth,
                             priority, cache);
            issued = true;
        }
    }

    return issued;
}

bool
ARMOffsetBasedPointerPrefetcher::resolvePointerCacheHit(
    Addr pointer_addr, const StructureEntry &structure, unsigned depth,
    bool secure, std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    Addr pointer_target = 0;
    if (!findPointerCacheEntry(pointer_addr, secure, pointer_target)) {
        return false;
    }

    Addr data_addr = 0;
    if (!addOffset(pointer_target, structure.offset2, data_addr)) {
        return true;
    }

    appendDataPrefetchAddresses(data_addr, structure.triggeredPc, addresses);

    if (depth + 1 >= lookahead) {
        return true;
    }

    issueComposedLookahead(data_addr, structure.triggeredPc, depth + 1, secure,
                           addresses, cache);
    return true;
}

bool
ARMOffsetBasedPointerPrefetcher::resolvePointerCacheHit(
    const PacketPtr &pkt, const PrefetchInfo &source, Addr pointer_addr,
    const StructureEntry &structure, unsigned depth, int32_t priority,
    const CacheAccessor &cache)
{
    Addr pointer_target = 0;
    if (!findPointerCacheEntry(pointer_addr, source.isSecure(),
                               pointer_target)) {
        return false;
    }

    Addr data_addr = 0;
    if (!addOffset(pointer_target, structure.offset2, data_addr)) {
        return true;
    }

    queueDataPrefetches(pkt, source, data_addr, structure.triggeredPc,
                        priority, cache);

    if (depth + 1 >= lookahead) {
        return true;
    }

    queueComposedLookahead(pkt, source, data_addr, structure.triggeredPc,
                           depth + 1, priority, cache);
    return true;
}

bool
ARMOffsetBasedPointerPrefetcher::resolveResidentPointerBlock(
    Addr pointer_addr, const StructureEntry &structure, unsigned depth,
    bool secure, std::vector<AddrPriority> &addresses,
    const CacheAccessor &cache)
{
    const Addr pointer_block = blockAddress(pointer_addr);
    const uint8_t *data = cache.findBlock(pointer_block, secure);
    if (data == nullptr) {
        return false;
    }

    const unsigned offset =
        static_cast<unsigned>(pointer_addr - pointer_block);
    Addr pointer_target = 0;
    if (!readPointerFromLine(data, blkSize, offset, pointer_target)) {
        return false;
    }

    Addr data_addr = 0;
    if (!addOffset(pointer_target, structure.offset2, data_addr)) {
        return true;
    }

    if (!addPointerCacheEntry(pointer_addr, pointer_target, data_addr,
                              secure)) {
        return true;
    }

    appendDataPrefetchAddresses(data_addr, structure.triggeredPc, addresses);

    if (depth + 1 >= lookahead) {
        return true;
    }

    issueComposedLookahead(data_addr, structure.triggeredPc, depth + 1, secure,
                           addresses, cache);
    return true;
}

bool
ARMOffsetBasedPointerPrefetcher::resolveResidentPointerBlock(
    const PacketPtr &pkt, const PrefetchInfo &source, Addr pointer_addr,
    const StructureEntry &structure, unsigned depth, int32_t priority,
    const CacheAccessor &cache)
{
    const Addr pointer_block = blockAddress(pointer_addr);
    const uint8_t *data = cache.findBlock(pointer_block, source.isSecure());
    if (data == nullptr) {
        return false;
    }

    const unsigned offset =
        static_cast<unsigned>(pointer_addr - pointer_block);
    Addr pointer_target = 0;
    if (!readPointerFromLine(data, blkSize, offset, pointer_target)) {
        return false;
    }

    Addr data_addr = 0;
    if (!addOffset(pointer_target, structure.offset2, data_addr)) {
        return true;
    }

    if (!addPointerCacheEntry(pointer_addr, pointer_target, data_addr,
                              source.isSecure())) {
        return true;
    }

    queueDataPrefetches(pkt, source, data_addr, structure.triggeredPc,
                        priority, cache);

    if (depth + 1 >= lookahead) {
        return true;
    }

    queueComposedLookahead(pkt, source, data_addr, structure.triggeredPc,
                           depth + 1, priority, cache);
    return true;
}

void
ARMOffsetBasedPointerPrefetcher::queuePointerRead(
    const PacketPtr &pkt, const PrefetchInfo &source, Addr pointer_addr,
    const StructureEntry &structure, unsigned depth, int32_t priority,
    const CacheAccessor &cache)
{
    if (resolvePointerCacheHit(pkt, source, pointer_addr, structure, depth,
                               priority, cache)) {
        return;
    }

    const Addr pointer_block = blockAddress(pointer_addr);
    if (cache.inCache(pointer_block, source.isSecure())) {
        resolveResidentPointerBlock(pkt, source, pointer_addr, structure,
                                    depth, priority, cache);
        return;
    }

    PrefetchInfo new_pfi(source, pointer_block);
    AddrPriority pointer_cmd(pointer_block, priority,
                             PrefetchSourceType::ARMOffsetBasedPointer);
    insert(pkt, new_pfi, pointer_cmd);
    recordPendingPointer(pointer_addr, structure, depth, source.isSecure());
}

void
ARMOffsetBasedPointerPrefetcher::recordPendingPointer(
    Addr pointer_addr, const StructureEntry &structure, unsigned depth,
    bool secure)
{
    const Addr pointer_block = blockAddress(pointer_addr);

    if (pendingTable.size() >= pendingEntries && !pendingTable.empty()) {
        pendingTable.pop_front();
    }

    pendingTable.push_back(
        {structure.triggeringPc, structure.triggeredPc, structure.type,
         pointer_addr, pointer_block, structure.offset1, structure.offset2,
         snapshotSpatialOffsets(structure.triggeredPc), depth, secure});
}

void
ARMOffsetBasedPointerPrefetcher::queueDataPrefetch(const PacketPtr &pkt,
                                                   const PrefetchInfo &source,
                                                   Addr addr, int32_t priority,
                                                   const CacheAccessor &cache)
{
    PrefetchInfo new_pfi(source, blockAddress(addr));
    AddrPriority data_cmd(blockAddress(addr), priority,
                          PrefetchSourceType::ARMOffsetBasedPointer);
    insert(pkt, new_pfi, data_cmd);
}

void
ARMOffsetBasedPointerPrefetcher::queueDataPrefetches(
    const PacketPtr &pkt, const PrefetchInfo &source, Addr addr,
    Addr triggered_pc, int32_t priority, const CacheAccessor &cache)
{
    queueDataPrefetches(pkt, source, addr,
                        snapshotSpatialOffsets(triggered_pc), priority, cache);
}

void
ARMOffsetBasedPointerPrefetcher::queueDataPrefetches(
    const PacketPtr &pkt, const PrefetchInfo &source, Addr addr,
    const std::vector<int64_t> &spatial_offsets, int32_t priority,
    const CacheAccessor &cache)
{
    queueDataPrefetch(pkt, source, addr, priority, cache);

    for (const auto offset : spatial_offsets) {
        Addr spatial_addr = 0;
        if (addOffset(addr, offset, spatial_addr)) {
            queueDataPrefetch(pkt, source, spatial_addr, priority, cache);
        }
    }
}

void
ARMOffsetBasedPointerPrefetcher::issueFromStructures(
    const PrefetchInfo &pfi, Addr pointer_value, bool pointer_value_valid,
    std::vector<AddrPriority> &addresses, const CacheAccessor &cache)
{
    const Addr trigger = pfi.getAddr();

    for (const auto &structure : dataStructureTable) {
        if (structure.triggeringPc != pfi.getPC() ||
            structure.confidence < minConfidence) {
            continue;
        }

        if (structure.type == RelationType::Table) {
            Addr next = trigger;
            for (unsigned d = 1; d <= degree; ++d) {
                if (!addOffset(next, structure.offset1, next)) {
                    break;
                }
                appendDataPrefetchAddresses(next, structure.triggeredPc,
                                            addresses);
            }
        } else if (structure.type == RelationType::LinkedList) {
            Addr pointer_addr = 0;
            if (addOffset(trigger, structure.offset1, pointer_addr)) {
                if (!resolvePointerCacheHit(pointer_addr, structure, 0,
                                            pfi.isSecure(), addresses,
                                            cache)) {
                    if (cache.inCache(blockAddress(pointer_addr),
                                      pfi.isSecure())) {
                        resolveResidentPointerBlock(pointer_addr, structure, 0,
                                                    pfi.isSecure(), addresses,
                                                    cache);
                    } else {
                        addresses.push_back(
                            AddrPriority(
                                blockAddress(pointer_addr), 0,
                                PrefetchSourceType::ARMOffsetBasedPointer));
                        recordPendingPointer(pointer_addr, structure, 0,
                                             pfi.isSecure());
                    }
                }
            }
        } else if (structure.type == RelationType::PointerTable) {
            if (pointer_value_valid) {
                Addr data_addr = 0;
                if (addOffset(pointer_value, structure.offset2, data_addr)) {
                    appendDataPrefetchAddresses(
                        data_addr, structure.triggeredPc, addresses);
                }
            }

            Addr pointer_addr = 0;
            if (addOffset(trigger, structure.offset1, pointer_addr)) {
                if (!resolvePointerCacheHit(pointer_addr, structure, 0,
                                            pfi.isSecure(), addresses,
                                            cache)) {
                    if (cache.inCache(blockAddress(pointer_addr),
                                      pfi.isSecure())) {
                        resolveResidentPointerBlock(pointer_addr, structure, 0,
                                                    pfi.isSecure(), addresses,
                                                    cache);
                    } else {
                        addresses.push_back(
                            AddrPriority(
                                blockAddress(pointer_addr), 0,
                                PrefetchSourceType::ARMOffsetBasedPointer));
                        recordPendingPointer(pointer_addr, structure, 0,
                                             pfi.isSecure());
                    }
                }
            }
        }
    }
}

void
ARMOffsetBasedPointerPrefetcher::resolvePendingPointers(
    const CacheAccessProbeArg &acc)
{
    const PacketPtr pkt = acc.pkt;
    if (!pkt->hasData() || !pkt->req->hasPaddr()) {
        return;
    }

    const Addr fill_block = blockAddress(pkt->getAddr());
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    PrefetchInfo source(pkt, pkt->req->getPaddr(), false);

    for (auto it = pendingTable.begin(); it != pendingTable.end();) {
        PendingPointer pending = *it;
        if (pending.pointerBlock != fill_block ||
            pending.secure != pkt->isSecure()) {
            ++it;
            continue;
        }

        it = pendingTable.erase(it);

        const unsigned offset =
            static_cast<unsigned>(pending.pointerAddr - fill_block);
        Addr pointer_target = 0;
        if (!readPointerFromLine(data, blkSize, offset, pointer_target)) {
            continue;
        }

        Addr data_addr = 0;
        if (!addOffset(pointer_target, pending.offset2, data_addr)) {
            continue;
        }

        if (!addPointerCacheEntry(pending.pointerAddr, pointer_target,
                                  data_addr, pkt->isSecure())) {
            continue;
        }

        queueDataPrefetches(pkt, source, data_addr, pending.spatialOffsets, 0,
                            acc.cache);

        if (pending.depth + 1 >= lookahead) {
            continue;
        }

        queueComposedLookahead(pkt, source, data_addr, pending.triggeredPc,
                               pending.depth + 1, 0, acc.cache);
    }
}

void
ARMOffsetBasedPointerPrefetcher::notifyFill(const PacketPtr &pkt)
{
    scanLineForPointers(pkt);
    if (cache != nullptr) {
        CacheAccessProbeArg acc(pkt, *cache);
        resolvePendingPointers(acc);
    }
}

void
ARMOffsetBasedPointerPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC() || pfi.isWrite()) {
        return;
    }

    Addr pointer_value = 0;
    const bool pointer_value_valid = readPointerValue(pfi, pointer_value);
    Addr pointer_reference = 0;
    if (pointer_value_valid) {
        pointer_reference =
            findPointerReferenceAddress(pointer_value, pfi.isSecure());
        if (pointer_reference == 0 &&
            looksLikePointer(pointer_value, pfi.getAddr())) {
            pointer_reference = pfi.getAddr();
        }
    }
    const bool pointer_value_accepted =
        pointer_value_valid &&
        addPointerCacheEntry(pfi.getAddr(), pointer_value, pointer_reference,
                             pfi.isSecure());

    const Addr pc = pfi.getPC();
    const Addr trigger = pfi.getAddr();
    learnPointerTarget(pc, trigger, pfi.isSecure());
    learnSpatialOffsets(pc, trigger, pfi.isSecure());

    HistoryEntry &history = getHistoryEntry(pc);
    history.secure = pfi.isSecure();
    if (history.lastTrigger != 0) {
        learnTable(pc, trigger, history);
    }

    for (const auto &previous_history : historyBuffer) {
        learnLinkedList(pc, trigger, previous_history, pfi.isSecure());
    }

    if (pointer_value_accepted) {
        learnPointerTable(pc, trigger, pointer_value, history);
    }

    if (cache != nullptr) {
        issueFromStructures(pfi, pointer_value, pointer_value_accepted,
                            addresses, *cache);
    }

    history.lastTrigger = trigger;
}

} // namespace prefetch
} // namespace gem5
