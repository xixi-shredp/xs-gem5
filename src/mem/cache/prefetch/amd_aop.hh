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

#ifndef __MEM_CACHE_PREFETCH_AMD_AOP_HH__
#define __MEM_CACHE_PREFETCH_AMD_AOP_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "sim/byteswap.hh"

namespace gem5
{

struct AMDAOPPrefetcherParams;

namespace prefetch
{

/**
 * Approximation of AMD US12050916B2 "array of pointers prefetching".
 *
 * The patent describes decode-stage register tracking and injected address
 * load/pointer-target prefetch micro-ops. This cache prefetcher interface
 * cannot observe destination/source registers or receive results from injected
 * loads as architected temporary registers. The implementation maps the same
 * tables onto observable cache events:
 *
 * - table 304: a PC-indexed striding load table;
 * - table 306: a PC-pair table learned when data returned by a striding load
 *   is later observed as the base address of a memory operation;
 * - address-load injection: a normal cache prefetch for future pointer-array
 *   elements;
 * - temporary register: the data returned by the prefetched cache line on
 * fill;
 * - pointer-target injection: a second cache prefetch using the fetched
 * pointer value plus the learned target offset.
 */
class AMDAOP : public Queued
{
  private:
    static constexpr int64_t InvalidOffset =
        static_cast<int64_t>(0x7fffffffffffffffLL);

    const unsigned strideTableEntries;
    const unsigned targetTableEntries;
    const unsigned recentPointerEntries;
    const unsigned pendingAddressLoadEntries;
    const unsigned pointerValueEntries;

    const unsigned confidenceMax;
    const unsigned initialConfidence;
    const unsigned strideConfidenceThreshold;
    const unsigned targetConfidenceThreshold;

    const bool useRequestorId;
    const unsigned addressLoadDegree;
    const unsigned targetDegree;
    const unsigned lookahead;
    const unsigned pointerBytes;
    const unsigned pointerAlignBits;
    const unsigned indexScale;
    const Addr minPointerAddr;
    const uint64_t maxTargetOffset;
    const uint64_t pointerTrackingWindow;
    const unsigned cacheStatusThreshold;
    const unsigned minCacheStatusForThrottling;
    const unsigned lowMissRateThresholdPct;
    const bool prefetchCurrentPointer;

    const ByteOrder byteOrder;
    uint64_t sequence;

    struct StridingLoadEntry
    {
        bool valid = false;
        bool active = false;
        bool trained = false;
        bool insertionMode = false;
        bool secure = false;
        bool addressVirtual = false;
        Addr pc = 0;
        RequestorID requestorId = 0;
        Addr lastAddr = 0;
        int64_t stride = 0;
        unsigned confidence = 0;
        unsigned pointerSamples = 0;
        uint64_t sourceEpoch = 0;
        uint64_t lastTouch = 0;
    };

    struct TargetEntry
    {
        bool valid = false;
        bool secure = false;
        bool sourceVirtual = false;
        Addr sourcePC = 0;
        Addr targetPC = 0;
        RequestorID requestorId = 0;
        uint64_t targetScale = 1;
        int64_t targetOffset = InvalidOffset;
        Addr minTargetAddr = MaxAddr;
        Addr maxTargetAddr = 0;
        unsigned confidence = 0;
        unsigned cacheStatus = 0;
        unsigned cacheMiss = 0;
        uint64_t lastDetectedEpoch = 0;
        uint64_t lastTouch = 0;
    };

    struct RecentPointer
    {
        Addr value = 0;
        Addr sourceAddr = 0;
        Addr sourcePC = 0;
        RequestorID requestorId = 0;
        bool secure = false;
        bool sourceVirtual = false;
        uint64_t seq = 0;
    };

    struct PendingAddressLoad
    {
        Addr blockAddr = 0;
        Addr elementAddr = 0;
        Addr elementPaddr = 0;
        Addr sourcePC = 0;
        RequestorID requestorId = 0;
        ContextID contextId = InvalidContextID;
        bool validContextId = false;
        bool secure = false;
        bool elementVirtual = false;
        uint64_t seq = 0;
    };

    struct PointerValueEntry
    {
        Addr elementAddr = 0;
        Addr value = 0;
        bool secure = false;
        bool addressVirtual = false;
        uint64_t seq = 0;
    };

    std::vector<StridingLoadEntry> stridingLoads;
    std::vector<TargetEntry> targetEntries;
    std::vector<RecentPointer> recentPointers;
    std::vector<PendingAddressLoad> pendingAddressLoads;
    std::vector<PointerValueEntry> pointerValues;

    bool currentAccessValid = false;
    bool currentAddressVirtual = false;
    bool currentContextValid = false;
    ContextID currentContextId = InvalidContextID;

    RequestorID context(const PrefetchInfo &pfi) const;
    bool sameContext(RequestorID lhs, RequestorID rhs) const;
    bool accessIsVirtual(const PrefetchInfo &pfi) const;
    bool accessHasContextId(const PrefetchInfo &pfi) const;
    ContextID accessContextId(const PrefetchInfo &pfi) const;

    void increment(unsigned &counter) const;
    void decrement(unsigned &counter) const;
    bool trainedStride(const StridingLoadEntry &entry) const;
    bool trainedTarget(const TargetEntry &entry) const;
    bool hasTrainedTarget(const StridingLoadEntry &entry) const;
    void updateStrideState(StridingLoadEntry &entry);
    bool sourceInInsertionMode(const StridingLoadEntry &entry) const;

    StridingLoadEntry *findStrideEntry(Addr pc, bool secure,
                                       RequestorID requestor_id,
                                       bool address_virtual);
    StridingLoadEntry *getStrideEntry(Addr pc, bool secure,
                                      RequestorID requestor_id,
                                      bool address_virtual);
    StridingLoadEntry *updateStrideTable(const PrefetchInfo &pfi);

    TargetEntry *findTargetEntry(Addr source_pc, Addr target_pc, bool secure,
                                 RequestorID requestor_id,
                                 bool source_virtual);
    TargetEntry *getTargetEntry(Addr source_pc, Addr target_pc, bool secure,
                                RequestorID requestor_id, bool source_virtual);
    void updateTargetMapping(TargetEntry &entry, uint64_t scale,
                             int64_t offset);
    void updateCacheStatus(TargetEntry &entry, bool miss);
    void detrainAbsentTargets(StridingLoadEntry &source);
    void detectPointerTarget(const PrefetchInfo &pfi);

    void expireOldState();
    bool decodePointerValue(const uint8_t *data, unsigned size,
                            Addr &value) const;
    bool pointerFromPrefetchInfo(const PrefetchInfo &pfi, Addr &value) const;
    bool pointerFromPacket(PacketPtr pkt, Addr element_addr,
                           bool element_is_virtual, Addr source_addr,
                           Addr &value) const;
    bool pointerFromCachedBlock(const CacheAccessor &cache, Addr block_addr,
                                Addr element_addr, Addr source_addr,
                                bool secure, Addr &value) const;
    bool looksLikePointer(Addr source_addr, Addr value) const;
    bool looksLikeTrackedValue(Addr source_addr, Addr value) const;
    bool scaledValue(Addr value, uint64_t scale, Addr &scaled) const;
    bool deriveSamePagePaddr(const PrefetchInfo &pfi, Addr addr,
                             Addr &paddr) const;
    bool inferInitialTargetMapping(const RecentPointer &ptr, Addr target_addr,
                                   uint64_t &scale, int64_t &offset) const;
    void recordPointerValue(Addr source_addr, Addr source_pc,
                            RequestorID requestor_id, bool secure,
                            bool source_virtual, Addr value);
    void invalidatePointerValues(Addr addr, unsigned size, bool secure,
                                 bool address_virtual);
    bool findCachedPointerValue(Addr element_addr, bool secure,
                                bool address_virtual, Addr &value) const;

    bool addSignedOffset(Addr base, int64_t offset, Addr &result) const;
    bool validPrefetchAddress(Addr addr) const;
    int64_t signedDifference(Addr lhs, Addr rhs) const;
    uint64_t absOffset(int64_t value) const;

    std::vector<TargetEntry *> trainedTargetsFor(Addr source_pc, bool secure,
                                                 RequestorID requestor_id,
                                                 bool source_virtual);
    bool shouldPrefetchTarget(const TargetEntry &entry) const;
    void addAddressLoadCandidate(const PrefetchInfo &pfi,
                                 StridingLoadEntry &stride_entry,
                                 Addr element_addr,
                                 std::vector<AddrPriority> &addresses);
    void addTargetCandidatesFromPointer(const PrefetchInfo &pfi,
                                        StridingLoadEntry &stride_entry,
                                        Addr pointer_value,
                                        std::vector<AddrPriority> &addresses,
                                        unsigned &target_count);
    void issueTargetsFromAddressLoad(PacketPtr pkt,
                                     const PendingAddressLoad &pending,
                                     Addr pointer_value);

  public:
    AMDAOP(const AMDAOPPrefetcherParams &p);
    ~AMDAOP() = default;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
    void notifyFill(const PacketPtr &pkt) override;
    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_AMD_AOP_HH__
