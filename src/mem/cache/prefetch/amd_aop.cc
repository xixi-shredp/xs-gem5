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
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/cache/prefetch/amd_aop.hh"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "params/AMDAOPPrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

AMDAOP::AMDAOP(const AMDAOPPrefetcherParams &p)
    : Queued(p),
      strideTableEntries(p.stride_table_entries),
      targetTableEntries(p.target_table_entries),
      recentPointerEntries(p.recent_pointer_entries),
      pendingAddressLoadEntries(p.pending_address_load_entries),
      pointerValueEntries(p.pointer_value_entries),
      confidenceMax((1u << p.confidence_counter_bits) - 1),
      initialConfidence(p.initial_confidence),
      strideConfidenceThreshold(p.stride_confidence_threshold),
      targetConfidenceThreshold(p.target_confidence_threshold),
      useRequestorId(p.use_requestor_id),
      addressLoadDegree(p.address_load_degree),
      targetDegree(p.target_degree),
      lookahead(p.lookahead),
      pointerBytes(p.pointer_bytes),
      pointerAlignBits(p.pointer_align_bits),
      indexScale(p.index_scale),
      minPointerAddr(p.min_pointer_addr),
      maxTargetOffset(p.max_target_offset),
      pointerTrackingWindow(p.pointer_tracking_window),
      cacheStatusThreshold(p.cache_status_threshold),
      minCacheStatusForThrottling(p.min_cache_status_for_throttling),
      lowMissRateThresholdPct(p.low_miss_rate_threshold_pct),
      prefetchCurrentPointer(p.prefetch_current_pointer),
      byteOrder(p.sys->getGuestByteOrder()),
      sequence(0)
{
    fatal_if(p.confidence_counter_bits == 0 || p.confidence_counter_bits > 31,
             "AMDAOP confidence_counter_bits must be in [1, 31]");
    fatal_if(initialConfidence > confidenceMax,
             "AMDAOP initial_confidence exceeds counter maximum");
    fatal_if(strideConfidenceThreshold > confidenceMax,
             "AMDAOP stride_confidence_threshold exceeds counter maximum");
    fatal_if(targetConfidenceThreshold > confidenceMax,
             "AMDAOP target_confidence_threshold exceeds counter maximum");
    fatal_if(pointerBytes != sizeof(uint32_t) &&
                 pointerBytes != sizeof(uint64_t),
             "AMDAOP pointer_bytes must be 4 or 8");
    fatal_if(strideTableEntries == 0,
             "AMDAOP stride_table_entries must be non-zero");
    fatal_if(targetTableEntries == 0,
             "AMDAOP target_table_entries must be non-zero");

    stridingLoads.resize(strideTableEntries);
    targetEntries.resize(targetTableEntries);
    recentPointers.reserve(recentPointerEntries);
    pendingAddressLoads.reserve(pendingAddressLoadEntries);
    pointerValues.reserve(pointerValueEntries);
}

RequestorID
AMDAOP::context(const PrefetchInfo &pfi) const
{
    return useRequestorId ? pfi.getRequestorId() : 0;
}

bool
AMDAOP::accessIsVirtual(const PrefetchInfo &pfi) const
{
    return currentAccessValid ? currentAddressVirtual : useVirtualAddresses;
}

bool
AMDAOP::accessHasContextId(const PrefetchInfo &pfi) const
{
    return currentAccessValid && currentContextValid;
}

ContextID
AMDAOP::accessContextId(const PrefetchInfo &pfi) const
{
    assert(accessHasContextId(pfi));
    return currentContextId;
}

bool
AMDAOP::sameContext(RequestorID lhs, RequestorID rhs) const
{
    return !useRequestorId || lhs == rhs;
}

void
AMDAOP::increment(unsigned &counter) const
{
    counter = std::min(confidenceMax, counter + 1);
}

void
AMDAOP::decrement(unsigned &counter) const
{
    if (counter > 0) {
        counter--;
    }
}

bool
AMDAOP::trainedStride(const StridingLoadEntry &entry) const
{
    return entry.valid && entry.active && entry.trained &&
           entry.pointerSamples > 0;
}

bool
AMDAOP::trainedTarget(const TargetEntry &entry) const
{
    return entry.valid && entry.targetOffset != InvalidOffset &&
           entry.confidence >= targetConfidenceThreshold &&
           shouldPrefetchTarget(entry);
}

bool
AMDAOP::hasTrainedTarget(const StridingLoadEntry &entry) const
{
    for (const auto &target : targetEntries) {
        if (target.sourcePC == entry.pc && target.secure == entry.secure &&
            target.sourceVirtual == entry.addressVirtual &&
            sameContext(target.requestorId, entry.requestorId) &&
            trainedTarget(target)) {
            return true;
        }
    }
    return false;
}

void
AMDAOP::updateStrideState(StridingLoadEntry &entry)
{
    entry.active = entry.valid && entry.confidence != 0;
    entry.trained = entry.valid && entry.stride != 0 &&
                    entry.confidence >= strideConfidenceThreshold;
    entry.insertionMode = trainedStride(entry) && hasTrainedTarget(entry);
}

bool
AMDAOP::sourceInInsertionMode(const StridingLoadEntry &entry) const
{
    return trainedStride(entry) && entry.insertionMode &&
           hasTrainedTarget(entry);
}

AMDAOP::StridingLoadEntry *
AMDAOP::findStrideEntry(Addr pc, bool secure, RequestorID requestor_id,
                        bool address_virtual)
{
    for (auto &entry : stridingLoads) {
        if (entry.valid && entry.pc == pc && entry.secure == secure &&
            entry.addressVirtual == address_virtual &&
            sameContext(entry.requestorId, requestor_id)) {
            return &entry;
        }
    }
    return nullptr;
}

AMDAOP::StridingLoadEntry *
AMDAOP::getStrideEntry(Addr pc, bool secure, RequestorID requestor_id,
                       bool address_virtual)
{
    if (auto *entry =
            findStrideEntry(pc, secure, requestor_id, address_virtual)) {
        return entry;
    }

    auto victim = std::min_element(
        stridingLoads.begin(), stridingLoads.end(),
        [](const StridingLoadEntry &lhs, const StridingLoadEntry &rhs) {
            if (lhs.valid != rhs.valid) {
                return !lhs.valid;
            }
            return lhs.lastTouch < rhs.lastTouch;
        });
    fatal_if(victim == stridingLoads.end(), "empty AMDAOP stride table");

    *victim = StridingLoadEntry{};
    victim->valid = true;
    victim->secure = secure;
    victim->addressVirtual = address_virtual;
    victim->pc = pc;
    victim->requestorId = requestor_id;
    victim->confidence = initialConfidence;
    updateStrideState(*victim);
    return &(*victim);
}

AMDAOP::StridingLoadEntry *
AMDAOP::updateStrideTable(const PrefetchInfo &pfi)
{
    if (!pfi.hasPC() || pfi.isWrite()) {
        return nullptr;
    }

    const Addr pc = pfi.getPC();
    const bool secure = pfi.isSecure();
    const RequestorID requestor_id = context(pfi);
    StridingLoadEntry *entry =
        getStrideEntry(pc, secure, requestor_id, accessIsVirtual(pfi));
    const bool had_previous = entry->lastTouch != 0;
    if (had_previous) {
        detrainAbsentTargets(*entry);
    }
    entry->sourceEpoch++;

    const Addr addr = pfi.getAddr();
    if (had_previous) {
        const int64_t stride = signedDifference(addr, entry->lastAddr);
        if (stride != 0) {
            if (entry->stride == 0) {
                entry->stride = stride;
            } else if (stride == entry->stride) {
                increment(entry->confidence);
            } else {
                decrement(entry->confidence);
                if (entry->confidence < strideConfidenceThreshold) {
                    entry->stride = stride;
                }
            }
        }
    }

    entry->lastAddr = addr;
    entry->lastTouch = sequence;
    updateStrideState(*entry);

    DPRINTF(HWPrefetch,
            "AMDAOP stride pc=%#x addr=%#x stride=%ld conf=%u ptr=%u "
            "active=%u trained=%u insert=%u\n",
            pc, addr, entry->stride, entry->confidence, entry->pointerSamples,
            entry->active, entry->trained, entry->insertionMode);
    return entry;
}

AMDAOP::TargetEntry *
AMDAOP::findTargetEntry(Addr source_pc, Addr target_pc, bool secure,
                        RequestorID requestor_id, bool source_virtual)
{
    for (auto &entry : targetEntries) {
        if (entry.valid && entry.sourcePC == source_pc &&
            entry.targetPC == target_pc && entry.secure == secure &&
            entry.sourceVirtual == source_virtual &&
            sameContext(entry.requestorId, requestor_id)) {
            return &entry;
        }
    }
    return nullptr;
}

AMDAOP::TargetEntry *
AMDAOP::getTargetEntry(Addr source_pc, Addr target_pc, bool secure,
                       RequestorID requestor_id, bool source_virtual)
{
    if (auto *entry = findTargetEntry(source_pc, target_pc, secure,
                                      requestor_id, source_virtual)) {
        return entry;
    }

    auto victim =
        std::min_element(targetEntries.begin(), targetEntries.end(),
                         [](const TargetEntry &lhs, const TargetEntry &rhs) {
                             if (lhs.valid != rhs.valid) {
                                 return !lhs.valid;
                             }
                             return lhs.lastTouch < rhs.lastTouch;
                         });
    fatal_if(victim == targetEntries.end(), "empty AMDAOP target table");

    *victim = TargetEntry{};
    victim->valid = true;
    victim->secure = secure;
    victim->sourceVirtual = source_virtual;
    victim->sourcePC = source_pc;
    victim->targetPC = target_pc;
    victim->requestorId = requestor_id;
    victim->confidence = initialConfidence;
    victim->lastTouch = sequence;
    return &(*victim);
}

void
AMDAOP::updateTargetMapping(TargetEntry &entry, uint64_t scale, int64_t offset)
{
    if (entry.targetOffset == InvalidOffset ||
        (entry.targetScale == scale && entry.targetOffset == offset)) {
        entry.targetScale = scale;
        entry.targetOffset = offset;
        increment(entry.confidence);
    } else {
        decrement(entry.confidence);
        if (entry.confidence < targetConfidenceThreshold) {
            entry.targetScale = scale;
            entry.targetOffset = offset;
        }
    }
}

void
AMDAOP::updateCacheStatus(TargetEntry &entry, bool miss)
{
    entry.cacheStatus++;
    if (miss) {
        entry.cacheMiss++;
    }

    if (entry.cacheStatus >= cacheStatusThreshold &&
        cacheStatusThreshold != 0) {
        entry.cacheStatus >>= 1;
        entry.cacheMiss >>= 1;
    }

    if (entry.cacheStatus >= minCacheStatusForThrottling &&
        entry.cacheStatus != 0 &&
        entry.cacheMiss * 100 < entry.cacheStatus * lowMissRateThresholdPct) {
        decrement(entry.confidence);
    }
}

void
AMDAOP::detrainAbsentTargets(StridingLoadEntry &source)
{
    if (source.sourceEpoch == 0) {
        return;
    }

    for (auto &entry : targetEntries) {
        if (!entry.valid || entry.sourcePC != source.pc ||
            entry.secure != source.secure ||
            entry.sourceVirtual != source.addressVirtual ||
            !sameContext(entry.requestorId, source.requestorId) ||
            entry.lastDetectedEpoch >= source.sourceEpoch) {
            continue;
        }

        decrement(entry.confidence);
        entry.lastTouch = sequence;
        DPRINTF(HWPrefetch,
                "AMDAOP absent target source_pc=%#x target_pc=%#x "
                "source_epoch=%lu detected_epoch=%lu conf=%u\n",
                source.pc, entry.targetPC, source.sourceEpoch,
                entry.lastDetectedEpoch, entry.confidence);
    }
    updateStrideState(source);
}

void
AMDAOP::detectPointerTarget(const PrefetchInfo &pfi)
{
    if (!pfi.hasPC()) {
        return;
    }

    const Addr addr = pfi.getAddr();
    const Addr pc = pfi.getPC();
    const bool secure = pfi.isSecure();
    const RequestorID requestor_id = context(pfi);

    RecentPointer *best = nullptr;
    uint64_t best_scale = 1;
    int64_t best_offset = InvalidOffset;
    uint64_t best_score = std::numeric_limits<uint64_t>::max();

    for (auto &ptr : recentPointers) {
        if (ptr.secure != secure ||
            !sameContext(ptr.requestorId, requestor_id)) {
            continue;
        }
        if (ptr.sourcePC == pc &&
            blockAddress(ptr.sourceAddr) == blockAddress(addr)) {
            continue;
        }

        uint64_t scale = 1;
        int64_t offset = InvalidOffset;
        uint64_t score = sequence - ptr.seq;

        TargetEntry *entry = findTargetEntry(ptr.sourcePC, pc, secure,
                                             requestor_id, ptr.sourceVirtual);
        if (entry != nullptr && entry->targetOffset != InvalidOffset) {
            Addr scaled = 0;
            if (!scaledValue(ptr.value, entry->targetScale, scaled)) {
                continue;
            }
            scale = entry->targetScale;
            offset = signedDifference(addr, scaled);

            const uint64_t offset_delta =
                offset >= entry->targetOffset
                    ? static_cast<uint64_t>(offset - entry->targetOffset)
                    : static_cast<uint64_t>(entry->targetOffset - offset);
            if (offset_delta > maxTargetOffset) {
                continue;
            }
            score += offset_delta * pointerTrackingWindow;
        } else if (!inferInitialTargetMapping(ptr, addr, scale, offset)) {
            continue;
        }

        if (score < best_score) {
            best = &ptr;
            best_scale = scale;
            best_offset = offset;
            best_score = score;
        }
    }

    if (best == nullptr) {
        return;
    }

    TargetEntry *entry = getTargetEntry(best->sourcePC, pc, secure,
                                        requestor_id, best->sourceVirtual);
    updateTargetMapping(*entry, best_scale, best_offset);
    updateCacheStatus(*entry, pfi.isCacheMiss());
    if (auto *source_entry = findStrideEntry(
            best->sourcePC, secure, requestor_id, best->sourceVirtual)) {
        entry->lastDetectedEpoch = source_entry->sourceEpoch;
        updateStrideState(*source_entry);
    }
    entry->minTargetAddr = std::min(entry->minTargetAddr, addr);
    entry->maxTargetAddr = std::max(entry->maxTargetAddr, addr);
    entry->lastTouch = sequence;

    DPRINTF(HWPrefetch,
            "AMDAOP paired source_pc=%#x target_pc=%#x scale=%lu "
            "offset=%ld conf=%u miss=%u/%u\n",
            best->sourcePC, pc, entry->targetScale, entry->targetOffset,
            entry->confidence, entry->cacheMiss, entry->cacheStatus);
}

void
AMDAOP::expireOldState()
{
    auto old = [this](uint64_t seq) {
        return pointerTrackingWindow != 0 &&
               sequence - seq > pointerTrackingWindow;
    };

    recentPointers.erase(std::remove_if(recentPointers.begin(),
                                        recentPointers.end(),
                                        [&](const RecentPointer &entry) {
                                            return old(entry.seq);
                                        }),
                         recentPointers.end());

    pendingAddressLoads.erase(
        std::remove_if(
            pendingAddressLoads.begin(), pendingAddressLoads.end(),
            [&](const PendingAddressLoad &entry) { return old(entry.seq); }),
        pendingAddressLoads.end());
}

bool
AMDAOP::decodePointerValue(const uint8_t *data, unsigned size,
                           Addr &value) const
{
    value = 0;
    if (data == nullptr || size < pointerBytes) {
        return false;
    }

    if (pointerBytes == sizeof(uint32_t)) {
        uint32_t raw = 0;
        std::memcpy(&raw, data, sizeof(raw));
        value = byteOrder == ByteOrder::big ? betoh(raw) : letoh(raw);
        return true;
    }

    uint64_t raw = 0;
    std::memcpy(&raw, data, sizeof(raw));
    value = byteOrder == ByteOrder::big ? betoh(raw) : letoh(raw);
    return true;
}

bool
AMDAOP::pointerFromPrefetchInfo(const PrefetchInfo &pfi, Addr &value) const
{
    value = 0;
    if (pfi.isWrite() || pfi.isCacheMiss() || pfi.getDataPtr() == nullptr ||
        pfi.getSize() != pointerBytes) {
        return false;
    }

    if (pointerBytes == sizeof(uint32_t)) {
        value = pfi.get<uint32_t>(byteOrder);
    } else {
        value = pfi.get<uint64_t>(byteOrder);
    }

    return looksLikeTrackedValue(pfi.getAddr(), value);
}

bool
AMDAOP::pointerFromPacket(PacketPtr pkt, Addr element_addr,
                          bool element_is_virtual, Addr source_addr,
                          Addr &value) const
{
    value = 0;
    if (pkt == nullptr || !pkt->hasData()) {
        return false;
    }
    if (element_is_virtual && !pkt->req->hasVaddr()) {
        return false;
    }

    const Addr packet_addr =
        element_is_virtual ? pkt->req->getVaddr() : pkt->getAddr();
    const Addr block_addr = blockAddress(packet_addr);
    if (element_addr < block_addr ||
        element_addr + pointerBytes > block_addr + blkSize) {
        return false;
    }

    const Addr offset = element_addr - block_addr;
    const uint8_t *data = pkt->getConstPtr<uint8_t>();
    if (!decodePointerValue(data + offset, pointerBytes, value)) {
        return false;
    }
    return looksLikeTrackedValue(source_addr, value);
}

bool
AMDAOP::pointerFromCachedBlock(const CacheAccessor &cache, Addr block_addr,
                               Addr element_addr, Addr source_addr,
                               bool secure, Addr &value) const
{
    value = 0;
    if (element_addr > MaxAddr - pointerBytes) {
        return false;
    }

    const Addr element_block = blockAddress(element_addr);
    if (element_block > MaxAddr - blkSize) {
        return false;
    }
    if (element_addr + pointerBytes > element_block + blkSize) {
        return false;
    }

    const uint8_t *block_data = cache.findBlock(block_addr, secure);
    if (block_data == nullptr) {
        return false;
    }

    const Addr offset = element_addr - element_block;
    if (!decodePointerValue(block_data + offset, pointerBytes, value)) {
        return false;
    }
    return looksLikeTrackedValue(source_addr, value);
}

bool
AMDAOP::looksLikePointer(Addr source_addr, Addr value) const
{
    if (value < minPointerAddr) {
        return false;
    }
    if (pointerAlignBits != 0 &&
        (value & ((static_cast<Addr>(1) << pointerAlignBits) - 1)) != 0) {
        return false;
    }
    return blockAddress(value) != blockAddress(source_addr);
}

bool
AMDAOP::looksLikeTrackedValue(Addr source_addr, Addr value) const
{
    return looksLikePointer(source_addr, value) ||
           (indexScale != 0 && value != 0);
}

bool
AMDAOP::scaledValue(Addr value, uint64_t scale, Addr &scaled) const
{
    if (scale == 0 || value > MaxAddr / scale) {
        return false;
    }
    scaled = value * scale;
    return true;
}

bool
AMDAOP::deriveSamePagePaddr(const PrefetchInfo &pfi, Addr addr,
                            Addr &paddr) const
{
    if (!accessIsVirtual(pfi)) {
        paddr = addr;
        return true;
    }

    if (!samePage(pfi.getAddr(), addr)) {
        return false;
    }

    if (addr >= pfi.getAddr()) {
        const Addr delta = addr - pfi.getAddr();
        if (pfi.getPaddr() > MaxAddr - delta) {
            return false;
        }
        paddr = pfi.getPaddr() + delta;
        return true;
    }

    const Addr delta = pfi.getAddr() - addr;
    if (pfi.getPaddr() < delta) {
        return false;
    }
    paddr = pfi.getPaddr() - delta;
    return true;
}

bool
AMDAOP::inferInitialTargetMapping(const RecentPointer &ptr, Addr target_addr,
                                  uint64_t &scale, int64_t &offset) const
{
    if (looksLikePointer(ptr.sourceAddr, ptr.value)) {
        scale = 1;
        offset = signedDifference(target_addr, ptr.value);
        if (absOffset(offset) <= maxTargetOffset) {
            return true;
        }
    }

    Addr scaled = 0;
    if (indexScale == 0 || !scaledValue(ptr.value, indexScale, scaled) ||
        scaled > target_addr) {
        return false;
    }

    scale = indexScale;
    offset = signedDifference(target_addr, scaled);
    return true;
}

void
AMDAOP::recordPointerValue(Addr source_addr, Addr source_pc,
                           RequestorID requestor_id, bool secure,
                           bool source_virtual, Addr value)
{
    if (!looksLikeTrackedValue(source_addr, value)) {
        return;
    }

    if (StridingLoadEntry *entry =
            findStrideEntry(source_pc, secure, requestor_id, source_virtual)) {
        entry->pointerSamples =
            std::min(entry->pointerSamples + 1, confidenceMax);
        entry->lastTouch = sequence;
        updateStrideState(*entry);
    }

    recentPointers.push_back(RecentPointer{value, source_addr, source_pc,
                                           requestor_id, secure,
                                           source_virtual, sequence});
    while (recentPointers.size() > recentPointerEntries) {
        recentPointers.erase(recentPointers.begin());
    }

    auto existing =
        std::find_if(pointerValues.begin(), pointerValues.end(),
                     [&](const PointerValueEntry &entry) {
                         return entry.elementAddr == source_addr &&
                                entry.secure == secure &&
                                entry.addressVirtual == source_virtual;
                     });
    if (existing != pointerValues.end()) {
        existing->value = value;
        existing->seq = sequence;
    } else {
        if (pointerValues.size() >= pointerValueEntries) {
            auto victim =
                std::min_element(pointerValues.begin(), pointerValues.end(),
                                 [](const PointerValueEntry &lhs,
                                    const PointerValueEntry &rhs) {
                                     return lhs.seq < rhs.seq;
                                 });
            if (victim != pointerValues.end()) {
                *victim = PointerValueEntry{source_addr, value, secure,
                                            source_virtual, sequence};
            }
        } else {
            pointerValues.push_back(PointerValueEntry{
                source_addr, value, secure, source_virtual, sequence});
        }
    }

    DPRINTF(HWPrefetch,
            "AMDAOP recorded pointer pc=%#x source=%#x value=%#x\n", source_pc,
            source_addr, value);
}

bool
AMDAOP::findCachedPointerValue(Addr element_addr, bool secure,
                               bool address_virtual, Addr &value) const
{
    for (const auto &entry : pointerValues) {
        if (entry.elementAddr == element_addr && entry.secure == secure &&
            entry.addressVirtual == address_virtual) {
            value = entry.value;
            return true;
        }
    }
    return false;
}

void
AMDAOP::invalidatePointerValues(Addr addr, unsigned size, bool secure,
                                bool address_virtual)
{
    const Addr end = addr > MaxAddr - size ? MaxAddr : addr + size;
    pointerValues.erase(
        std::remove_if(pointerValues.begin(), pointerValues.end(),
                       [&](const PointerValueEntry &entry) {
                           if (entry.secure != secure ||
                               entry.addressVirtual != address_virtual) {
                               return false;
                           }
                           const Addr entry_end =
                               entry.elementAddr > MaxAddr - pointerBytes
                                   ? MaxAddr
                                   : entry.elementAddr + pointerBytes;
                           return entry.elementAddr < end && addr < entry_end;
                       }),
        pointerValues.end());
}

bool
AMDAOP::addSignedOffset(Addr base, int64_t offset, Addr &result) const
{
    if (offset >= 0) {
        const uint64_t unsigned_offset = static_cast<uint64_t>(offset);
        if (base > MaxAddr - unsigned_offset) {
            return false;
        }
        result = base + unsigned_offset;
        return true;
    }

    const uint64_t unsigned_offset = static_cast<uint64_t>(-offset);
    if (base < unsigned_offset) {
        return false;
    }
    result = base - unsigned_offset;
    return true;
}

bool
AMDAOP::validPrefetchAddress(Addr addr) const
{
    const Addr block_addr = blockAddress(addr);
    if (block_addr > MaxAddr - (blkSize - 1)) {
        return false;
    }
    if (useVirtualAddresses) {
        return true;
    }
    return system != nullptr && system->isMemAddr(block_addr) &&
           system->isMemAddr(block_addr + blkSize - 1);
}

int64_t
AMDAOP::signedDifference(Addr lhs, Addr rhs) const
{
    if (lhs >= rhs) {
        const uint64_t diff = lhs - rhs;
        if (diff >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(diff);
    }

    const uint64_t diff = rhs - lhs;
    if (diff > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::min() + 1;
    }
    return -static_cast<int64_t>(diff);
}

uint64_t
AMDAOP::absOffset(int64_t value) const
{
    return value < 0 ? static_cast<uint64_t>(-value)
                     : static_cast<uint64_t>(value);
}

std::vector<AMDAOP::TargetEntry *>
AMDAOP::trainedTargetsFor(Addr source_pc, bool secure,
                          RequestorID requestor_id, bool source_virtual)
{
    std::vector<TargetEntry *> targets;
    for (auto &entry : targetEntries) {
        if (entry.sourcePC == source_pc && entry.secure == secure &&
            entry.sourceVirtual == source_virtual &&
            sameContext(entry.requestorId, requestor_id) &&
            trainedTarget(entry)) {
            targets.push_back(&entry);
        }
    }
    std::sort(targets.begin(), targets.end(),
              [](const TargetEntry *lhs, const TargetEntry *rhs) {
                  if (lhs->confidence != rhs->confidence) {
                      return lhs->confidence > rhs->confidence;
                  }
                  return lhs->lastTouch > rhs->lastTouch;
              });
    return targets;
}

bool
AMDAOP::shouldPrefetchTarget(const TargetEntry &entry) const
{
    if (entry.cacheStatus < minCacheStatusForThrottling ||
        entry.cacheStatus == 0) {
        return true;
    }
    return entry.cacheMiss * 100 >=
           entry.cacheStatus * lowMissRateThresholdPct;
}

void
AMDAOP::addAddressLoadCandidate(const PrefetchInfo &pfi,
                                StridingLoadEntry &stride_entry,
                                Addr element_addr,
                                std::vector<AddrPriority> &addresses)
{
    if (!validPrefetchAddress(element_addr)) {
        return;
    }

    addresses.push_back(AddrPriority(element_addr, 0, PrefetchSourceType::AMDAOP));

    const bool element_virtual = accessIsVirtual(pfi);
    Addr element_paddr = 0;
    if (!element_virtual) {
        element_paddr = element_addr;
    } else {
        deriveSamePagePaddr(pfi, element_addr, element_paddr);
    }
    const Addr pending_block =
        blockAddress(element_virtual ? element_addr : element_paddr);
    const ContextID context_id =
        accessHasContextId(pfi) ? accessContextId(pfi) : InvalidContextID;
    pendingAddressLoads.push_back(PendingAddressLoad{
        pending_block, element_addr, element_paddr, stride_entry.pc,
        stride_entry.requestorId, context_id, accessHasContextId(pfi),
        pfi.isSecure(), element_virtual, sequence});
    while (pendingAddressLoads.size() > pendingAddressLoadEntries) {
        pendingAddressLoads.erase(pendingAddressLoads.begin());
    }

    DPRINTF(HWPrefetch,
            "AMDAOP address-load candidate source_pc=%#x elem=%#x\n",
            stride_entry.pc, element_addr);
}

void
AMDAOP::addTargetCandidatesFromPointer(const PrefetchInfo &pfi,
                                       StridingLoadEntry &stride_entry,
                                       Addr pointer_value,
                                       std::vector<AddrPriority> &addresses,
                                       unsigned &target_count)
{
    auto targets = trainedTargetsFor(stride_entry.pc, pfi.isSecure(),
                                     stride_entry.requestorId,
                                     stride_entry.addressVirtual);
    for (auto *target : targets) {
        if (target_count >= targetDegree) {
            return;
        }

        Addr scaled_value = 0;
        if (!scaledValue(pointer_value, target->targetScale, scaled_value)) {
            continue;
        }

        Addr target_addr = 0;
        if (!addSignedOffset(scaled_value, target->targetOffset,
                             target_addr)) {
            continue;
        }
        if (target_addr < minPointerAddr) {
            continue;
        }
        if (!validPrefetchAddress(target_addr)) {
            continue;
        }
        if (target->targetScale != 1 && target->minTargetAddr != MaxAddr) {
            const uint64_t range_margin = std::max<uint64_t>(
                maxTargetOffset, static_cast<uint64_t>(target->targetScale) *
                                     (lookahead + addressLoadDegree + 1));
            const Addr low = target->minTargetAddr > range_margin
                                 ? target->minTargetAddr - range_margin
                                 : 0;
            const Addr high = target->maxTargetAddr > MaxAddr - range_margin
                                  ? MaxAddr
                                  : target->maxTargetAddr + range_margin;
            if (target_addr < low || target_addr > high) {
                continue;
            }
        }

        addresses.push_back(AddrPriority(target_addr, -1, PrefetchSourceType::AMDAOP));
        target_count++;
        DPRINTF(HWPrefetch,
                "AMDAOP target candidate source_pc=%#x target_pc=%#x "
                "value=%#x scale=%lu target=%#x\n",
                stride_entry.pc, target->targetPC, pointer_value,
                target->targetScale, target_addr);
    }
}

void
AMDAOP::issueTargetsFromAddressLoad(PacketPtr pkt,
                                    const PendingAddressLoad &pending,
                                    Addr pointer_value)
{
    if (pkt == nullptr) {
        return;
    }

    auto *stride_entry =
        findStrideEntry(pending.sourcePC, pending.secure, pending.requestorId,
                        pending.elementVirtual);
    if (stride_entry == nullptr || !trainedStride(*stride_entry)) {
        return;
    }

    RequestPtr synthetic_req;
    std::unique_ptr<Packet> synthetic_pkt;
    PacketPtr insert_pkt = pkt;
    if (pending.elementVirtual && !pkt->req->hasVaddr()) {
        if (!pending.validContextId || !pkt->req->hasPaddr()) {
            return;
        }
        const Addr block_paddr = blockAddress(pkt->req->getPaddr());
        const Addr element_offset =
            pending.elementAddr - blockAddress(pending.elementAddr);
        const Addr element_paddr = pending.elementPaddr != 0
                                       ? pending.elementPaddr
                                       : block_paddr + element_offset;
        const Addr pc = pkt->req->hasPC() ? pkt->req->getPC() : 0;
        synthetic_req = std::make_shared<Request>(
            pending.elementAddr, blkSize, pkt->req->getFlags(),
            pkt->req->requestorId(), pc, pending.contextId);
        synthetic_req->setPaddr(element_paddr);
        synthetic_req->setFlags(Request::PREFETCH);
        synthetic_pkt =
            std::make_unique<Packet>(synthetic_req, MemCmd::HardPFReq);
        insert_pkt = synthetic_pkt.get();
    }

    PrefetchInfo fill_pfi(insert_pkt, pending.elementAddr, false);
    unsigned target_count = 0;
    std::vector<AddrPriority> targets;
    addTargetCandidatesFromPointer(fill_pfi, *stride_entry, pointer_value,
                                   targets, target_count);

    for (auto &target : targets) {
        target.addr = blockAddress(target.addr);
        PrefetchInfo target_pfi(fill_pfi, target.addr);
        insert(insert_pkt, target_pfi, target);
    }
}

void
AMDAOP::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    currentAccessValid = true;
    currentAddressVirtual = pkt != nullptr && useVirtualAddresses &&
                            pkt->req->hasVaddr() &&
                            pfi.getAddr() == pkt->req->getVaddr();
    currentContextValid = pkt != nullptr && pkt->req->hasContextId();
    currentContextId =
        currentContextValid ? pkt->req->contextId() : InvalidContextID;

    Queued::notify(pkt, pfi);

    currentAccessValid = false;
    currentAddressVirtual = false;
    currentContextValid = false;
    currentContextId = InvalidContextID;
}

void
AMDAOP::calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses)
{
    assert(addresses.empty());
    sequence++;
    expireOldState();

    if (!pfi.hasPC()) {
        return;
    }

    detectPointerTarget(pfi);
    if (pfi.isWrite()) {
        invalidatePointerValues(pfi.getAddr(), pfi.getSize(), pfi.isSecure(),
                                accessIsVirtual(pfi));
        return;
    }

    StridingLoadEntry *stride_entry = updateStrideTable(pfi);
    if (stride_entry == nullptr) {
        return;
    }

    Addr pointer_value = 0;
    const bool has_pointer = pointerFromPrefetchInfo(pfi, pointer_value);
    if (has_pointer) {
        recordPointerValue(pfi.getAddr(), pfi.getPC(), context(pfi),
                           pfi.isSecure(), accessIsVirtual(pfi),
                           pointer_value);
    }

    if (!trainedStride(*stride_entry)) {
        return;
    }

    unsigned target_count = 0;
    if (prefetchCurrentPointer && has_pointer && accessIsVirtual(pfi)) {
        addTargetCandidatesFromPointer(pfi, *stride_entry, pointer_value,
                                       addresses, target_count);
    }

    if (!sourceInInsertionMode(*stride_entry)) {
        return;
    }

    for (unsigned idx = 0; idx < addressLoadDegree; idx++) {
        const unsigned steps = lookahead + idx + 1;
        Addr element_addr = 0;
        const int64_t stride = stride_entry->stride;
        if ((stride > 0 &&
             steps > static_cast<uint64_t>(
                         std::numeric_limits<int64_t>::max() / stride)) ||
            (stride < 0 &&
             steps > static_cast<uint64_t>(
                         std::numeric_limits<int64_t>::max() / -stride))) {
            continue;
        }

        if (!addSignedOffset(pfi.getAddr(),
                             stride * static_cast<int64_t>(steps),
                             element_addr)) {
            continue;
        }

        addAddressLoadCandidate(pfi, *stride_entry, element_addr, addresses);

        Addr cached_value = 0;
        bool has_cached_value = false;
        Addr element_paddr = 0;
        if (cache != nullptr &&
            deriveSamePagePaddr(pfi, element_addr, element_paddr)) {
            has_cached_value = pointerFromCachedBlock(
                *cache, blockAddress(element_paddr), element_addr, element_addr,
                pfi.isSecure(), cached_value);
            if (has_cached_value) {
                recordPointerValue(element_addr, pfi.getPC(), context(pfi),
                                   pfi.isSecure(), accessIsVirtual(pfi),
                                   cached_value);
            }
        }
        if (!has_cached_value) {
            has_cached_value =
                findCachedPointerValue(element_addr, pfi.isSecure(),
                                       accessIsVirtual(pfi), cached_value);
        }
        if (has_cached_value) {
            addTargetCandidatesFromPointer(pfi, *stride_entry, cached_value,
                                           addresses, target_count);
        }
    }
}

void
AMDAOP::notifyFill(const PacketPtr &pkt)
{
    sequence++;
    expireOldState();

    if (pkt == nullptr || !pkt->req->hasPaddr() || !pkt->hasData()) {
        return;
    }

    const bool secure = pkt->isSecure();

    for (auto it = pendingAddressLoads.begin();
         it != pendingAddressLoads.end();) {
        if (it->validContextId && (!pkt->req->hasContextId() ||
                                   pkt->req->contextId() != it->contextId)) {
            ++it;
            continue;
        }
        const bool fill_has_vaddr = it->elementVirtual && pkt->req->hasVaddr();
        if (it->elementVirtual && !fill_has_vaddr && it->elementPaddr == 0) {
            ++it;
            continue;
        }
        const Addr fill_block = fill_has_vaddr
                                    ? blockAddress(pkt->req->getVaddr())
                                    : blockAddress(pkt->getAddr());
        const Addr pending_block = it->elementVirtual && !fill_has_vaddr
                                       ? blockAddress(it->elementPaddr)
                                       : it->blockAddr;
        if (it->secure != secure || pending_block != fill_block) {
            ++it;
            continue;
        }

        Addr pointer_value = 0;
        const bool packet_addr_is_virtual = fill_has_vaddr;
        const Addr packet_element_addr =
            packet_addr_is_virtual ? it->elementAddr : it->elementPaddr;
        if (pointerFromPacket(pkt, packet_element_addr, packet_addr_is_virtual,
                              it->elementAddr, pointer_value)) {
            recordPointerValue(it->elementAddr, it->sourcePC, it->requestorId,
                               it->secure, it->elementVirtual, pointer_value);
            issueTargetsFromAddressLoad(pkt, *it, pointer_value);
        }
        it = pendingAddressLoads.erase(it);
    }

    if (pkt->req->hasPC() && pkt->req->getSize() == pointerBytes) {
        const Addr source_addr = useVirtualAddresses && pkt->req->hasVaddr()
                                     ? pkt->req->getVaddr()
                                     : pkt->req->getPaddr();
        Addr pointer_value = 0;
        const bool source_is_virtual =
            useVirtualAddresses && pkt->req->hasVaddr();
        if (pointerFromPacket(pkt, source_addr, source_is_virtual, source_addr,
                              pointer_value)) {
            const RequestorID requestor_id =
                useRequestorId ? pkt->req->requestorId() : 0;
            recordPointerValue(source_addr, pkt->req->getPC(), requestor_id,
                               secure, source_is_virtual, pointer_value);
        }
    }
}

} // namespace prefetch
} // namespace gem5
