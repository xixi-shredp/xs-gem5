/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#ifndef __MEM_CACHE_PREFETCH_ARM_HINT_HH__
#define __MEM_CACHE_PREFETCH_ARM_HINT_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/cache_probe_arg.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/request.hh"
#include "sim/byteswap.hh"

namespace gem5
{

struct ARMHintPrefetcherParams;
class IndirectMemoryPrefetchHint;

namespace prefetch
{

class ARMHintPrefetcher : public Queued
{
  private:
    struct IndirectCandidate
    {
        bool valid = false;
        Addr base = 0;
        int shift = 0;
        unsigned confidence = 0;
    };

    struct SourceContext
    {
        bool valid = false;
        bool explicitHint = false;
        bool trainTargets = true;
        bool hasBase = false;
        Addr base = 0;
        int shift = 0;
        bool directPointer = false;
        bool signedIndex = false;
        unsigned elementBytes = 0;
        unsigned elementStride = 0;
        unsigned elements = 0;
        std::vector<unsigned> elementOffsets;
        std::shared_ptr<IndirectMemoryPrefetchHint> hint;
    };

    struct IndexValue
    {
        uint64_t raw = 0;
        int64_t signedValue = 0;
        bool signedIndex = false;
        unsigned valueBytes = 0;
    };

    struct SourceElement
    {
        Addr addr = 0;
        IndexValue value;
    };

    struct StreamEntry
    {
        bool valid = false;
        Addr pc = 0;
        RequestorID requestor =
            static_cast<RequestorID>(Request::invldRequestorId);
        bool secure = false;
        bool lastAddrValid = false;
        Addr lastAddr = 0;
        int64_t stride = 0;
        unsigned strideConfidence = 0;
        bool indirectValid = false;
        Addr base = 0;
        int shift = 0;
        unsigned indirectConfidence = 0;
        unsigned valueBytes = 0;
        uint64_t age = 0;
        std::vector<IndirectCandidate> candidates;
        SourceContext context;
    };

    struct IndirectSource
    {
        Addr sourceAddr = 0;
        Addr pc = 0;
        RequestorID requestor =
            static_cast<RequestorID>(Request::invldRequestorId);
        bool secure = false;
        IndexValue value;
        Tick observed = 0;
        SourceContext context;
    };

    struct PendingIndirect
    {
        Addr sourceAddr = 0;
        Addr pc = 0;
        RequestorID requestor = Request::invldRequestorId;
        bool secure = false;
        SourceContext context;
        int32_t priority = 0;
        bool fromPrefetch = false;
        Tick created = 0;
    };

    const unsigned sourceTableEntries;
    const unsigned recentSourceEntries;
    const unsigned pendingEntries;
    const unsigned strideConfidenceThreshold;
    const unsigned indirectConfidenceThreshold;
    const unsigned degree;
    const unsigned lookahead;
    const unsigned addressIndicatingBytes;
    const unsigned sourceElementBytes;
    const unsigned sourceElementStride;
    const unsigned maxSourceElements;
    const bool signedIndex;
    const Addr minCandidateAddress;
    const Addr maxCandidateAddress;
    const unsigned targetAlignment;
    const std::vector<int> shiftValues;
    const bool requireIndirectHint;
    const std::vector<Addr> hintPCs;
    const bool useRequestorId;
    const bool enableDirectPointer;
    const bool enableStaticOffset;
    const Addr staticBase;
    const int staticShift;
    const bool enableProcessorHintTarget;
    const Addr processorHintBase;
    const int processorHintShift;
    const bool validateCandidateAddresses;
    const ByteOrder byteOrder;

    std::vector<StreamEntry> streamTable;
    std::deque<IndirectSource> recentSources;
    std::deque<PendingIndirect> pending;
    uint64_t nextAge = 0;
    bool currentSourceContextValid = false;
    SourceContext currentSourceContext;

    struct ARMHintStats : public statistics::Group
    {
        ARMHintStats(statistics::Group *parent);

        statistics::Scalar sourceLoadsObserved;
        statistics::Scalar sourcePrefetches;
        statistics::Scalar sourceCacheHits;
        statistics::Scalar pendingInserted;
        statistics::Scalar pendingCompleted;
        statistics::Scalar indirectMappingsLearned;
        statistics::Scalar targetsGenerated;
        statistics::Scalar targetsRejected;
        statistics::Scalar directPointerTargets;
        statistics::Scalar explicitHintsObserved;
        statistics::Scalar vectorElementsDecoded;
    } stats;

    RequestorID normalizeRequestor(RequestorID requestor) const;
    Addr getPC(const PrefetchInfo &pfi) const;
    bool isHintPC(Addr pc) const;
    SourceContext defaultContext(const PrefetchInfo &pfi) const;
    SourceContext contextFromRequest(const RequestPtr &req,
                                     unsigned request_size, Addr pc) const;
    SourceContext contextFromPacket(const PacketPtr &pkt,
                                    const PrefetchInfo &pfi) const;
    void refreshSourceContextFromHint(SourceContext &context) const;
    SourceContext normalizeContext(SourceContext context,
                                   unsigned request_size) const;
    bool sourceAllowed(const SourceContext &context) const;
    unsigned contextElementBytes(const SourceContext &context,
                                 unsigned request_size) const;
    unsigned contextElementStride(const SourceContext &context,
                                  unsigned elem_bytes) const;
    unsigned contextElementCount(const SourceContext &context,
                                 unsigned request_size, unsigned elem_bytes,
                                 unsigned elem_stride) const;
    bool contextElementOffset(const SourceContext &context, unsigned element,
                              unsigned elem_stride, uint64_t &offset) const;
    void updateStreamContext(StreamEntry &entry, const SourceContext &context);
    bool inheritSourceContext(StreamEntry *entry,
                              SourceContext &context) const;
    StreamEntry *findStream(Addr pc, bool secure, RequestorID requestor);
    StreamEntry &getOrCreateStream(Addr pc, bool secure,
                                   RequestorID requestor);
    void updateSourceStream(StreamEntry &entry, Addr addr,
                            unsigned value_bytes);
    bool
    decodeElementsFromPrefetchInfo(const PrefetchInfo &pfi,
                                   const SourceContext &context,
                                   std::vector<SourceElement> &values) const;
    bool decodeElementsFromPacket(const PacketPtr &pkt, Addr base_source_addr,
                                  const SourceContext &context,
                                  std::vector<SourceElement> &values) const;
    bool decodeElementsFromCache(const CacheAccessor &cache,
                                 Addr base_source_addr, bool secure,
                                 const SourceContext &context,
                                 std::vector<SourceElement> &values) const;
    bool decodeValue(const uint8_t *data, unsigned value_bytes,
                     bool signed_index, IndexValue &value) const;
    int64_t signExtend(uint64_t value, unsigned value_bytes) const;
    bool shiftedUnsigned(uint64_t value, int shift, uint64_t &shifted) const;
    bool shiftedSigned(int64_t value, int shift, int64_t &shifted) const;
    bool addSignedOffset(Addr base, int64_t offset, Addr &target) const;
    bool isCandidateAddress(Addr target) const;
    bool generateTarget(const StreamEntry &entry, const SourceContext &context,
                        const IndexValue &value, Addr &target);
    void addRecentSource(const StreamEntry &entry, Addr source_addr,
                         const SourceContext &context,
                         const IndexValue &value);
    void addPending(Addr source_addr, const StreamEntry &entry,
                    const SourceContext &context, int32_t priority,
                    bool from_prefetch);
    void matchRecentSources(Addr target, bool secure, RequestorID requestor);
    bool enqueueTargetFromValue(const PacketPtr &pkt,
                                const CacheAccessor &cache,
                                const StreamEntry &entry,
                                const SourceContext &context,
                                const IndexValue &value, int32_t priority);

  public:
    ARMHintPrefetcher(const ARMHintPrefetcherParams &p);
    ~ARMHintPrefetcher() = default;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void notifyFill(const PacketPtr &pkt) override;
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
    void rxHint(BaseMMU::Translation *) override {}
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_ARM_HINT_HH__
