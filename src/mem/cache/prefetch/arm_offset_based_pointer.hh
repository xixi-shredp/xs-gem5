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

#ifndef __MEM_CACHE_PREFETCH_ARM_OFFSET_BASED_POINTER_HH__
#define __MEM_CACHE_PREFETCH_ARM_OFFSET_BASED_POINTER_HH__

#include <cstdint>
#include <deque>
#include <vector>

#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "sim/byteswap.hh"

namespace gem5
{

struct ARMOffsetBasedPointerPrefetcherParams;

namespace prefetch
{

class ARMOffsetBasedPointerPrefetcher : public Queued
{
  private:
    enum class RelationType : uint8_t
    {
        Table,
        LinkedList,
        PointerTable
    };

    struct HistoryEntry
    {
        Addr pc = 0;
        bool valid = false;
        Addr lastTrigger = 0;
        bool lastPointerValid = false;
        Addr lastPointerAddr = 0;
        Addr lastPointerTarget = 0;
        bool pointerStrideValid = false;
        int64_t pointerStride = 0;
        bool secure = false;
        std::vector<int64_t> spatialOffsets;
    };

    struct PointerCacheEntry
    {
        Addr pointerAddr = 0;
        Addr target = 0;
        bool secure = false;
    };

    struct StructureEntry
    {
        Addr triggeringPc = 0;
        Addr triggeredPc = 0;
        RelationType type = RelationType::Table;
        int64_t offset1 = 0;
        int64_t offset2 = 0;
        unsigned confidence = 0;
    };

    struct PendingPointer
    {
        Addr triggeringPc = 0;
        Addr triggeredPc = 0;
        RelationType type = RelationType::LinkedList;
        Addr pointerAddr = 0;
        Addr pointerBlock = 0;
        int64_t offset1 = 0;
        int64_t offset2 = 0;
        std::vector<int64_t> spatialOffsets;
        unsigned depth = 0;
        bool secure = false;
    };

    const unsigned historyEntries;
    const unsigned pointerCacheEntries;
    const unsigned structureEntries;
    const unsigned pendingEntries;
    const unsigned spatialEntries;
    const unsigned recentPointerSearchEntries;
    const Addr maxElementBytes;
    const Addr maxPointerOffsetBytes;
    const Addr maxPointerTargetOffsetBytes;
    const Addr minPointerAddress;
    const unsigned pointerBytes;
    const unsigned pointerMswMatchBits;
    const unsigned pointerAlignBits;
    const unsigned confidenceBits;
    const unsigned minConfidence;
    const unsigned degree;
    const unsigned lookahead;
    const bool scanCachelineOnFill;
    const bool enableTableDetector;
    const bool enableLinkedListDetector;
    const bool enablePointerTableDetector;
    const ByteOrder byteOrder;

    const unsigned maxConfidence;

    std::deque<HistoryEntry> historyBuffer;
    std::deque<PointerCacheEntry> pointerCache;
    std::deque<StructureEntry> dataStructureTable;
    std::deque<PendingPointer> pendingTable;

    HistoryEntry &getHistoryEntry(Addr pc);
    HistoryEntry *findHistoryEntry(Addr pc);
    const HistoryEntry *findHistoryEntry(Addr pc) const;
    StructureEntry &getStructureEntry(Addr triggering_pc, Addr triggered_pc,
                                      RelationType type, int64_t offset1,
                                      int64_t offset2);

    bool addOffset(Addr base, int64_t offset, Addr &result) const;
    bool diffWithin(Addr a, Addr b, Addr threshold, int64_t &diff) const;
    bool looksLikePointer(Addr pointer, Addr reference) const;
    bool readPointerValue(const PrefetchInfo &pfi, Addr &value) const;
    bool readPointerFromLine(const uint8_t *data, unsigned size,
                             unsigned offset, Addr &value) const;
    bool findPointerCacheEntry(Addr pointer_addr, bool secure,
                               Addr &target) const;
    Addr findPointerReferenceAddress(Addr pointer, bool secure) const;

    bool addPointerCacheEntry(Addr pointer_addr, Addr target,
                              Addr reference_addr, bool secure);
    void scanLineForPointers(PacketPtr pkt);

    void learnSpatialOffsets(Addr current_pc, Addr current_trigger,
                             bool secure);
    void learnTable(Addr current_pc, Addr current_trigger,
                    HistoryEntry &history);
    void learnLinkedList(Addr current_pc, Addr current_trigger,
                         const HistoryEntry &history, bool secure);
    void learnPointerTarget(Addr current_pc, Addr current_trigger,
                            bool secure);
    void learnPointerTable(Addr pc, Addr pointer_addr, Addr pointer_target,
                           HistoryEntry &history);

    void appendDataPrefetchAddresses(Addr addr, Addr triggered_pc,
                                     std::vector<AddrPriority> &addresses);
    void
    appendDataPrefetchAddresses(Addr addr,
                                const std::vector<int64_t> &spatial_offsets,
                                std::vector<AddrPriority> &addresses);
    std::vector<int64_t> snapshotSpatialOffsets(Addr triggered_pc) const;
    bool issueComposedLookahead(Addr trigger_addr, Addr triggering_pc,
                                unsigned depth, bool secure,
                                std::vector<AddrPriority> &addresses,
                                const CacheAccessor &cache);
    bool queueComposedLookahead(const PacketPtr &pkt,
                                const PrefetchInfo &source, Addr trigger_addr,
                                Addr triggering_pc, unsigned depth,
                                int32_t priority, const CacheAccessor &cache);
    bool resolvePointerCacheHit(Addr pointer_addr,
                                const StructureEntry &structure,
                                unsigned depth, bool secure,
                                std::vector<AddrPriority> &addresses,
                                const CacheAccessor &cache);
    bool resolvePointerCacheHit(const PacketPtr &pkt,
                                const PrefetchInfo &source, Addr pointer_addr,
                                const StructureEntry &structure,
                                unsigned depth, int32_t priority,
                                const CacheAccessor &cache);
    bool resolveResidentPointerBlock(Addr pointer_addr,
                                     const StructureEntry &structure,
                                     unsigned depth, bool secure,
                                     std::vector<AddrPriority> &addresses,
                                     const CacheAccessor &cache);
    bool resolveResidentPointerBlock(const PacketPtr &pkt,
                                     const PrefetchInfo &source,
                                     Addr pointer_addr,
                                     const StructureEntry &structure,
                                     unsigned depth, int32_t priority,
                                     const CacheAccessor &cache);
    void queuePointerRead(const PacketPtr &pkt, const PrefetchInfo &source,
                          Addr pointer_addr, const StructureEntry &structure,
                          unsigned depth, int32_t priority,
                          const CacheAccessor &cache);
    void queueDataPrefetch(const PacketPtr &pkt, const PrefetchInfo &source,
                           Addr addr, int32_t priority,
                           const CacheAccessor &cache);
    void queueDataPrefetches(const PacketPtr &pkt, const PrefetchInfo &source,
                             Addr addr, Addr triggered_pc, int32_t priority,
                             const CacheAccessor &cache);
    void queueDataPrefetches(const PacketPtr &pkt, const PrefetchInfo &source,
                             Addr addr,
                             const std::vector<int64_t> &spatial_offsets,
                             int32_t priority, const CacheAccessor &cache);
    void recordPendingPointer(Addr pointer_addr,
                              const StructureEntry &structure, unsigned depth,
                              bool secure);
    void issueFromStructures(const PrefetchInfo &pfi, Addr pointer_value,
                             bool pointer_value_valid,
                             std::vector<AddrPriority> &addresses,
                             const CacheAccessor &cache);
    void resolvePendingPointers(const CacheAccessProbeArg &acc);

  public:
    ARMOffsetBasedPointerPrefetcher(
        const ARMOffsetBasedPointerPrefetcherParams &p);
    ~ARMOffsetBasedPointerPrefetcher() = default;

    void notifyFill(const PacketPtr &pkt) override;
    void rxHint(BaseMMU::Translation *) override {}

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_ARM_OFFSET_BASED_POINTER_HH__
