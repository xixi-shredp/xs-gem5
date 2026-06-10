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

#include "mem/cache/prefetch/dspatch.hh"

#include <algorithm>

#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "params/DSPatchPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

DSPatch::DSPatch(const DSPatchPrefetcherParams &p)
    : Queued(p),
      pageBufferEntries(p.page_buffer_entries),
      signatureTableEntries(p.signature_table_entries),
      regionSize(p.region_size),
      staticBandwidthQuartile(
          std::min<unsigned>(p.bandwidth_utilization_quartile, 3)),
      useMemoryControllerBandwidth(p.use_memory_controller_bandwidth),
      dynamicBandwidth(p.dynamic_bandwidth_monitor),
      bandwidthWindowCycles(p.bandwidth_window_cycles),
      bandwidthLowThreshold(p.bandwidth_low_threshold),
      bandwidthMidThreshold(p.bandwidth_mid_threshold),
      bandwidthHighThreshold(p.bandwidth_high_threshold),
      orCountMax(std::min<unsigned>(p.max_or_count, SatCounterMax)),
      accuracyThresholdPct(p.accuracy_threshold_pct),
      coverageThresholdPct(p.coverage_threshold_pct),
      currentBandwidthQuartile(staticBandwidthQuartile),
      signatureTable(signatureTableEntries)
{
    fatal_if(pageBufferEntries == 0,
             "DSPatch requires at least one page buffer entry");
    fatal_if(signatureTableEntries == 0,
             "DSPatch requires at least one signature table entry");
    fatal_if(regionSize == 0 || (regionSize % blkSize) != 0,
             "DSPatch region_size must be a positive multiple of block size");

    blocksPerRegion = regionSize / blkSize;
    fatal_if(blocksPerRegion != 64,
             "DSPatch currently models the paper's 4KB/64B, 64-line region");
    fatal_if(blocksPerRegion % TriggerSegments != 0,
             "DSPatch region must split evenly into two trigger segments");

    blocksPerSegment = blocksPerRegion / TriggerSegments;
    compressedBits = blocksPerRegion / LinesPerCompressedBit;
    fatal_if(compressedBits != 32,
             "DSPatch 128B-granularity compression requires 32 bits");
    fatal_if(dynamicBandwidth && bandwidthWindowCycles == 0,
             "DSPatch dynamic bandwidth window must be non-zero");
    fatal_if(dynamicBandwidth &&
                 !(bandwidthLowThreshold <= bandwidthMidThreshold &&
                   bandwidthMidThreshold <= bandwidthHighThreshold),
             "DSPatch bandwidth thresholds must be monotonically increasing");

}

DSPatch::~DSPatch()
{}

DSPatch::PageBufferIterator
DSPatch::findPage(Addr page, bool secure)
{
    for (auto it = pageBuffer.begin(); it != pageBuffer.end(); ++it) {
        if (it->page == page && it->secure == secure) {
            return it;
        }
    }
    return pageBuffer.end();
}

DSPatch::PageBufferEntry &
DSPatch::touchPage(Addr page, bool secure)
{
    auto it = findPage(page, secure);
    if (it != pageBuffer.end()) {
        pageBuffer.splice(pageBuffer.begin(), pageBuffer, it);
        return pageBuffer.front();
    }

    if (pageBuffer.size() >= pageBufferEntries) {
        commitPage(pageBuffer.back());
        pageBuffer.pop_back();
    }

    PageBufferEntry entry;
    entry.page = page;
    entry.secure = secure;
    pageBuffer.push_front(entry);
    return pageBuffer.front();
}

void
DSPatch::commitPage(const PageBufferEntry &entry)
{
    for (unsigned segment = 0; segment < TriggerSegments; ++segment) {
        const TriggerInfo &trigger = entry.triggers[segment];
        if (!trigger.valid) {
            continue;
        }

        const uint64_t training_pattern = maskPatternForSegment(
            entry.accessPattern & ~trigger.observedPattern, segment);
        const uint64_t anchored_pattern =
            anchorPattern(training_pattern, trigger.lineOffset);
        const uint32_t compressed_pattern = compressPattern(anchored_pattern);

        std::array<uint64_t, TriggerSegments> metric_patterns = {{0, 0}};
        std::array<uint64_t, TriggerSegments> metric_masks = {{0, 0}};
        std::array<bool, TriggerSegments> metric_valid = {{false, false}};
        for (unsigned prediction_segment = segment;
             prediction_segment < TriggerSegments; ++prediction_segment) {
            metric_masks[prediction_segment] =
                anchoredSegmentMask(trigger.lineOffset, prediction_segment);
            metric_patterns[prediction_segment] =
                anchored_pattern & metric_masks[prediction_segment];
            metric_valid[prediction_segment] = true;
        }

        updateSignature(trigger.pc, metric_patterns, metric_masks,
                        metric_valid, compressed_pattern);
    }
}

unsigned
DSPatch::signatureIndex(Addr pc) const
{
    uint64_t folded = pc;
    folded ^= folded >> 16;
    folded ^= folded >> 32;
    return folded % signatureTableEntries;
}

uint64_t
DSPatch::maskPatternForSegment(uint64_t pattern, unsigned segment) const
{
    if (segment == 0) {
        return pattern;
    }

    uint64_t segment_mask = 0;
    for (unsigned line = blocksPerSegment; line < blocksPerRegion; ++line) {
        segment_mask |= (1ULL << line);
    }
    return pattern & segment_mask;
}

uint64_t
DSPatch::anchoredSegmentMask(unsigned triggerLine,
                             unsigned predictionSegment) const
{
    uint64_t segment_mask = 0;
    for (unsigned relative_line = 0; relative_line < blocksPerRegion;
         ++relative_line) {
        const unsigned line = (triggerLine + relative_line) % blocksPerRegion;
        if ((line / blocksPerSegment) == predictionSegment) {
            segment_mask |= (1ULL << relative_line);
        }
    }
    return segment_mask;
}

uint64_t
DSPatch::maskAnchoredPatternForPredictionSegment(
    uint64_t pattern, unsigned triggerLine, unsigned predictionSegment) const
{
    return pattern & anchoredSegmentMask(triggerLine, predictionSegment);
}

uint64_t
DSPatch::anchorPattern(uint64_t pattern, unsigned triggerLine) const
{
    uint64_t anchored = 0;
    for (unsigned line = 0; line < blocksPerRegion; ++line) {
        if ((pattern & (1ULL << line)) == 0) {
            continue;
        }
        const unsigned anchored_line =
            (line + blocksPerRegion - triggerLine) % blocksPerRegion;
        anchored |= (1ULL << anchored_line);
    }
    return anchored;
}

uint32_t
DSPatch::compressPattern(uint64_t anchoredPattern) const
{
    uint32_t compressed = 0;
    for (unsigned line = 0; line < blocksPerRegion; ++line) {
        if ((anchoredPattern & (1ULL << line)) != 0) {
            compressed |= (1U << (line / LinesPerCompressedBit));
        }
    }
    return compressed;
}

uint64_t
DSPatch::expandPattern(uint32_t compressedPattern) const
{
    uint64_t expanded = 0;
    for (unsigned bit = 0; bit < compressedBits; ++bit) {
        if ((compressedPattern & (1U << bit)) == 0) {
            continue;
        }
        const unsigned line = bit * LinesPerCompressedBit;
        expanded |= (1ULL << line);
        expanded |= (1ULL << (line + 1));
    }
    return expanded;
}

bool
DSPatch::isBelowThreshold(unsigned numerator, unsigned denominator,
                          unsigned thresholdPct) const
{
    if (denominator == 0) {
        return false;
    }
    return (numerator * 100) < (denominator * thresholdPct);
}

unsigned
DSPatch::popCount(uint64_t value) const
{
    return __builtin_popcountll(value);
}

unsigned
DSPatch::popCount(uint32_t value) const
{
    return __builtin_popcount(value);
}

void
DSPatch::satIncrement(uint8_t &counter) const
{
    if (counter < SatCounterMax) {
        ++counter;
    }
}

void
DSPatch::satDecrement(uint8_t &counter) const
{
    if (counter > 0) {
        --counter;
    }
}

bool
DSPatch::updateQuality(SignatureEntry &entry, unsigned segment,
                       uint64_t programPattern, uint64_t predictionMask,
                       uint32_t prevCovPattern, uint32_t prevAccPattern)
{
    const uint64_t cov_prediction =
        expandPattern(prevCovPattern) & predictionMask;
    const uint64_t acc_prediction =
        expandPattern(prevAccPattern) & predictionMask;
    const uint64_t cov_hits = cov_prediction & programPattern;
    const bool cov_accuracy_low = isBelowThreshold(
        popCount(cov_hits), popCount(cov_prediction), accuracyThresholdPct);
    const bool cov_coverage_low = isBelowThreshold(
        popCount(cov_hits), popCount(programPattern), coverageThresholdPct);

    if (cov_accuracy_low || cov_coverage_low) {
        satIncrement(entry.measureCov[segment]);
    }

    const uint64_t acc_hits = acc_prediction & programPattern;
    const bool acc_accuracy_low = isBelowThreshold(
        popCount(acc_hits), popCount(acc_prediction), accuracyThresholdPct);

    if (acc_accuracy_low) {
        satIncrement(entry.measureAcc[segment]);
    } else {
        satDecrement(entry.measureAcc[segment]);
    }

    return cov_coverage_low;
}

void
DSPatch::updateSignature(
    Addr pc, const std::array<uint64_t, TriggerSegments> &metricPatterns,
    const std::array<uint64_t, TriggerSegments> &metricMasks,
    const std::array<bool, TriggerSegments> &metricValid,
    uint32_t compressedPattern)
{
    SignatureEntry &entry = signatureTable[signatureIndex(pc)];

    if (!entry.valid) {
        if (compressedPattern == 0) {
            return;
        }
        entry.valid = true;
        entry.covPattern = compressedPattern;
        entry.accPattern = compressedPattern;
        entry.measureCov.fill(0);
        entry.measureAcc.fill(0);
        entry.orCount.fill(0);
        return;
    }

    const uint32_t prev_cov = entry.covPattern;
    const uint32_t prev_acc = entry.accPattern;
    std::array<bool, TriggerSegments> cov_coverage_low = {{false, false}};
    for (unsigned segment = 0; segment < TriggerSegments; ++segment) {
        if (!metricValid[segment]) {
            continue;
        }
        cov_coverage_low[segment] =
            updateQuality(entry, segment, metricPatterns[segment],
                          metricMasks[segment], prev_cov, prev_acc);
    }

    for (unsigned segment = 0; segment < TriggerSegments; ++segment) {
        if (!metricValid[segment]) {
            continue;
        }

        const uint32_t segment_mask = compressPattern(metricMasks[segment]);
        const uint32_t segment_pattern = compressedPattern & segment_mask;

        if (entry.measureCov[segment] == SatCounterMax &&
            (currentBandwidthQuartile >= 3 || cov_coverage_low[segment])) {
            entry.covPattern =
                (entry.covPattern & ~segment_mask) | segment_pattern;
            entry.measureCov[segment] = 0;
            entry.orCount[segment] = 0;
        } else {
            const uint32_t new_bits = segment_pattern & ~entry.covPattern;
            if (new_bits != 0 && entry.orCount[segment] < orCountMax) {
                entry.covPattern |= segment_pattern;
                satIncrement(entry.orCount[segment]);
            }
        }

        entry.accPattern =
            (entry.accPattern & ~segment_mask) | (prev_cov & segment_pattern);
    }
}

uint32_t
DSPatch::selectPattern(const SignatureEntry &entry, unsigned segment) const
{
    if (currentBandwidthQuartile >= 3) {
        if (entry.measureAcc[segment] == SatCounterMax) {
            return 0;
        }
        return entry.accPattern;
    }

    if (currentBandwidthQuartile >= 2) {
        return entry.measureCov[segment] == SatCounterMax ? entry.accPattern
                                                          : entry.covPattern;
    }

    return entry.covPattern;
}

void
DSPatch::generateSegmentFromPattern(const PageBufferEntry &entry,
                                    unsigned triggerSegment,
                                    unsigned predictionSegment,
                                    std::vector<AddrPriority> &addresses) const
{
    const TriggerInfo &trigger = entry.triggers[triggerSegment];
    const SignatureEntry &sig = signatureTable[signatureIndex(trigger.pc)];
    const uint32_t pattern = selectPattern(sig, predictionSegment);
    if (pattern == 0) {
        return;
    }

    const bool low_priority =
        currentBandwidthQuartile < 2 &&
        sig.measureCov[predictionSegment] == SatCounterMax;
    const int32_t priority = low_priority ? -1 : 0;

    for (unsigned bit = 0; bit < compressedBits; ++bit) {
        if ((pattern & (1U << bit)) == 0) {
            continue;
        }

        for (unsigned subline = 0; subline < LinesPerCompressedBit;
             ++subline) {
            const unsigned relative_line =
                bit * LinesPerCompressedBit + subline;
            const unsigned line =
                (trigger.lineOffset + relative_line) % blocksPerRegion;

            if ((line / blocksPerSegment) != predictionSegment) {
                continue;
            }
            if ((entry.accessPattern & (1ULL << line)) != 0) {
                continue;
            }

            const Addr pf_addr = entry.page + line * blkSize;
            addresses.push_back(AddrPriority(
                pf_addr, priority, PrefetchSourceType::DSPatch));
        }
    }
}

void
DSPatch::generateFromPattern(const PageBufferEntry &entry, unsigned segment,
                             std::vector<AddrPriority> &addresses) const
{
    if (segment == 0) {
        generateSegmentFromPattern(entry, segment, 0, addresses);
        generateSegmentFromPattern(entry, segment, 1, addresses);
    } else {
        generateSegmentFromPattern(entry, segment, 1, addresses);
    }
}

void
DSPatch::updateBandwidthQuartile()
{
    if (bandwidthEvents >= bandwidthHighThreshold) {
        currentBandwidthQuartile = 3;
    } else if (bandwidthEvents >= bandwidthMidThreshold) {
        currentBandwidthQuartile = 2;
    } else if (bandwidthEvents >= bandwidthLowThreshold) {
        currentBandwidthQuartile = 1;
    } else {
        currentBandwidthQuartile = 0;
    }
}

void
DSPatch::sampleBandwidth()
{
    if (!dynamicBandwidth) {
        currentBandwidthQuartile = staticBandwidthQuartile;
        return;
    }

    const Tick window_ticks = clockPeriod() * bandwidthWindowCycles;
    if (bandwidthWindowEnd == 0) {
        bandwidthWindowEnd = curTick() + window_ticks;
    }

    while (curTick() >= bandwidthWindowEnd) {
        bandwidthEvents >>= 1;
        bandwidthWindowEnd += window_ticks;
    }

    ++bandwidthEvents;
    updateBandwidthQuartile();
}

void
DSPatch::calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses)
{
    sampleBandwidth();

    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "DSPatch ignores requests with no PC.\n");
        return;
    }

    const Addr blk_addr = blockAddress(pfi.getAddr());
    const Addr page = roundDown(blk_addr, regionSize);
    const unsigned line = (blk_addr - page) / blkSize;
    if (line >= blocksPerRegion) {
        return;
    }

    PageBufferEntry &entry = touchPage(page, pfi.isSecure());
    entry.accessPattern |= (1ULL << line);

    const unsigned segment = line / blocksPerSegment;
    TriggerInfo &trigger = entry.triggers[segment];
    if (trigger.valid) {
        return;
    }

    trigger.valid = true;
    trigger.pc = pfi.getPC();
    trigger.lineOffset = line;
    trigger.observedPattern = entry.accessPattern;
    generateFromPattern(entry, segment, addresses);
}

} // namespace prefetch
} // namespace gem5
