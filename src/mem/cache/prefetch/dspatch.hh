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

#ifndef __MEM_CACHE_PREFETCH_DSPATCH_HH__
#define __MEM_CACHE_PREFETCH_DSPATCH_HH__

#include <array>
#include <cstdint>
#include <list>
#include <vector>

#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct DSPatchPrefetcherParams;

namespace prefetch
{

class DSPatch : public Queued
{
  private:
    static constexpr unsigned TriggerSegments = 2;
    static constexpr unsigned LinesPerCompressedBit = 2;
    static constexpr uint8_t SatCounterMax = 3;

    struct TriggerInfo
    {
        bool valid = false;
        Addr pc = 0;
        unsigned lineOffset = 0;
        uint64_t observedPattern = 0;
    };

    struct PageBufferEntry
    {
        Addr page = 0;
        bool secure = false;
        uint64_t accessPattern = 0;
        std::array<TriggerInfo, TriggerSegments> triggers;
    };

    struct SignatureEntry
    {
        bool valid = false;
        uint32_t covPattern = 0;
        uint32_t accPattern = 0;
        std::array<uint8_t, TriggerSegments> measureCov = {{0, 0}};
        std::array<uint8_t, TriggerSegments> measureAcc = {{0, 0}};
        std::array<uint8_t, TriggerSegments> orCount = {{0, 0}};
    };

    const unsigned pageBufferEntries;
    const unsigned signatureTableEntries;
    const Addr regionSize;
    const unsigned staticBandwidthQuartile;
    const bool useMemoryControllerBandwidth;
    const bool dynamicBandwidth;
    const unsigned bandwidthWindowCycles;
    const unsigned bandwidthLowThreshold;
    const unsigned bandwidthMidThreshold;
    const unsigned bandwidthHighThreshold;
    const unsigned orCountMax;
    const unsigned accuracyThresholdPct;
    const unsigned coverageThresholdPct;

    unsigned blocksPerRegion = 0;
    unsigned blocksPerSegment = 0;
    unsigned compressedBits = 0;
    unsigned currentBandwidthQuartile = 0;
    unsigned bandwidthEvents = 0;
    Tick bandwidthWindowEnd = 0;

    std::list<PageBufferEntry> pageBuffer;
    std::vector<SignatureEntry> signatureTable;

    using PageBufferIterator = std::list<PageBufferEntry>::iterator;

    PageBufferIterator findPage(Addr page, bool secure);
    PageBufferEntry &touchPage(Addr page, bool secure);
    void commitPage(const PageBufferEntry &entry);
    void updateSignature(
        Addr pc, const std::array<uint64_t, TriggerSegments> &metricPatterns,
        const std::array<uint64_t, TriggerSegments> &metricMasks,
        const std::array<bool, TriggerSegments> &metricValid,
        uint32_t compressedPattern);
    bool updateQuality(SignatureEntry &entry, unsigned segment,
                       uint64_t programPattern, uint64_t predictionMask,
                       uint32_t prevCovPattern, uint32_t prevAccPattern);
    unsigned signatureIndex(Addr pc) const;
    uint64_t maskPatternForSegment(uint64_t pattern, unsigned segment) const;
    uint64_t anchoredSegmentMask(unsigned triggerLine,
                                 unsigned predictionSegment) const;
    uint64_t
    maskAnchoredPatternForPredictionSegment(uint64_t pattern,
                                            unsigned triggerLine,
                                            unsigned predictionSegment) const;
    uint64_t anchorPattern(uint64_t pattern, unsigned triggerLine) const;
    uint32_t compressPattern(uint64_t anchoredPattern) const;
    uint64_t expandPattern(uint32_t compressedPattern) const;
    uint32_t selectPattern(const SignatureEntry &entry,
                           unsigned segment) const;
    void
    generateSegmentFromPattern(const PageBufferEntry &entry,
                               unsigned triggerSegment,
                               unsigned predictionSegment,
                               std::vector<AddrPriority> &addresses) const;
    void generateFromPattern(const PageBufferEntry &entry, unsigned segment,
                             std::vector<AddrPriority> &addresses) const;
    void sampleBandwidth();
    void updateBandwidthQuartile();
    bool isBelowThreshold(unsigned numerator, unsigned denominator,
                          unsigned thresholdPct) const;
    unsigned popCount(uint64_t value) const;
    unsigned popCount(uint32_t value) const;
    void satIncrement(uint8_t &counter) const;
    void satDecrement(uint8_t &counter) const;

  public:
    DSPatch(const DSPatchPrefetcherParams &p);
    ~DSPatch() override;

    void rxHint(BaseMMU::Translation *) override {}

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_DSPATCH_HH__
