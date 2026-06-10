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

#include "mem/cache/prefetch/streamline.hh"

#include <algorithm>
#include <cstring>

#include "base/logging.hh"
#include "params/StreamlinePrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

namespace
{

constexpr std::size_t MetadataBlockEntryCount = 4;
constexpr std::size_t MetadataSlotFieldCount = 5;
constexpr std::size_t MetadataBlockWordCount = 8;
constexpr Addr MetadataBlockBytes = 64;
constexpr unsigned MetadataTargetBits = 31;
constexpr uint64_t MetadataTargetMask = (1ULL << MetadataTargetBits) - 1;
constexpr uint64_t MetadataTargetSignBit = 1ULL << (MetadataTargetBits - 1);
constexpr __uint128_t InvalidMetadataSlot = ~static_cast<__uint128_t>(0);

std::vector<std::vector<Addr>>
decodeMetadataBuffer(const std::vector<Addr> &current_buffer,
                     std::size_t entry_length)
{
    panic_if(current_buffer.size() % entry_length != 0,
             "Streamline metadata buffer payload size %u is not divisible by "
             "entry length %u",
             current_buffer.size(), entry_length);

    std::vector<std::vector<Addr>> entries;
    for (std::size_t offset = 0; offset < current_buffer.size();
         offset += entry_length) {
        entries.emplace_back(current_buffer.begin() + offset,
                             current_buffer.begin() + offset + entry_length);
    }
    return entries;
}

std::vector<Addr>
encodeMetadataBuffer(const std::vector<std::vector<Addr>> &entries)
{
    std::vector<Addr> flattened;
    for (const auto &entry : entries) {
        flattened.insert(flattened.end(), entry.begin(), entry.end());
    }
    return flattened;
}

const std::vector<Addr> *
findBufferedEntryWithSuccessor(
    const std::vector<std::vector<Addr>> &buffer_entries, Addr address)
{
    for (const auto &entry : buffer_entries) {
        const auto match = std::find(entry.begin(), entry.end(), address);
        if (match != entry.end() && std::next(match) != entry.end()) {
            return &entry;
        }
    }
    return nullptr;
}

void
touchMetadataEntryState(std::array<uint8_t, MetadataBlockEntryCount> &etrs,
                        std::size_t valid_count, std::size_t touched_index)
{
    const auto count = std::min(valid_count, etrs.size());
    panic_if(touched_index >= count,
             "Touched metadata slot %u exceeds valid slot count %u",
             touched_index, count);

    for (std::size_t i = 0; i < count; ++i) {
        if (i == touched_index) {
            etrs[i] = 0;
        } else {
            etrs[i] = std::min<uint8_t>(7, etrs[i] + 1);
        }
    }
}

} // namespace

Streamline::MetadataPort::MetadataPort(const std::string &name,
                                       Streamline &owner)
    : RequestPort(name, &owner), owner(owner)
{}

void
Streamline::MetadataPort::recvReqRetry()
{
    owner.recvMetadataReqRetry();
}

bool
Streamline::MetadataPort::recvTimingResp(PacketPtr pkt)
{
    return owner.recvMetadataResp(pkt);
}

Streamline::StreamlineStatGroup::StreamlineStatGroup(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(metadataBufferHits, statistics::units::Count::get(),
               "number of Streamline metadata-buffer hits"),
      ADD_STAT(metadataBufferMisses, statistics::units::Count::get(),
               "number of Streamline metadata-buffer misses"),
      ADD_STAT(metadataBufferInsertions, statistics::units::Count::get(),
               "number of Streamline metadata-buffer insertions"),
      ADD_STAT(metadataReadIssued, statistics::units::Count::get(),
               "number of Streamline metadata reads issued"),
      ADD_STAT(metadataReadResponses, statistics::units::Count::get(),
               "number of Streamline metadata read responses"),
      ADD_STAT(
          metadataReadHits, statistics::units::Count::get(),
          "number of Streamline metadata reads that found a stream entry"),
      ADD_STAT(metadataReadMisses, statistics::units::Count::get(),
               "number of Streamline metadata reads that missed"),
      ADD_STAT(metadataReadHitRate, statistics::units::Count::get(),
               "hit rate of Streamline metadata reads"),
      ADD_STAT(metadataWritesIssued, statistics::units::Count::get(),
               "number of Streamline metadata writes issued"),
      ADD_STAT(metadataWriteResponses, statistics::units::Count::get(),
               "number of Streamline metadata write responses"),
      ADD_STAT(metadataReqRetries, statistics::units::Count::get(),
               "number of Streamline metadata request retries"),
      ADD_STAT(currentPartitionLevel, statistics::units::Count::get(),
               "current Streamline LLC metadata partition level"),
      ADD_STAT(partitionTransitions, statistics::units::Count::get(),
               "number of Streamline LLC metadata repartition transitions"),
      ADD_STAT(partitionSampledAccesses, statistics::units::Count::get(),
               "number of sampled accesses accumulated in the current "
               "partition epoch"),
      ADD_STAT(partitionScoreLevel0, statistics::units::Count::get(),
               "current Streamline partition score for level 0"),
      ADD_STAT(partitionScoreLevel1, statistics::units::Count::get(),
               "current Streamline partition score for level 1"),
      ADD_STAT(partitionScoreLevel2, statistics::units::Count::get(),
               "current Streamline partition score for level 2")
{
    using namespace statistics;

    metadataReadHitRate.flags(total);
    metadataReadHitRate = metadataReadHits / metadataReadResponses;
}

Streamline::Streamline(const StreamlinePrefetcherParams &p)
    : Base(p),
      metadataBaseAddr(p.metadata_base_addr),
      metadataStoreAssoc(p.metadata_store_assoc),
      metadataStoreEntries(p.metadata_store_entries),
      metadataLineStride(p.metadata_line_stride),
      metadataBufferEntries(p.metadata_buffer_entries),
      maxDegree(p.max_degree),
      epochSize(p.epoch_size),
      lowInsertionThreshold(p.insertions_low_thresh),
      midInsertionThreshold(p.insertions_mid_thresh),
      highInsertionThreshold(p.insertions_high_thresh),
      maxMetadataSets(
          metadataSetCount(p.metadata_store_entries, p.metadata_store_assoc)),
      metadataRequestorId(p.sys->getRequestorId(this, "metadata")),
      metadataPort(name() + ".metadata_port", *this),
      streamlineStats(this)
{
    syncPartitionStats();
}

Streamline::~Streamline()
{
    for (auto &entry : prefetchQueue) {
        delete entry.pkt;
    }
    if (activeMetadataRequest != nullptr) {
        delete activeMetadataRequest->senderState;
        delete activeMetadataRequest;
    }
    for (auto *pkt : metadataRequestQueue) {
        delete pkt->senderState;
        delete pkt;
    }
}

Port &
Streamline::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "metadata_port") {
        return metadataPort;
    }
    return Base::getPort(if_name, idx);
}

void
Streamline::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    static_cast<void>(pkt);
    const bool prefetchHit = pfi.isPfFirstHit();
    const auto block = blockIndex(blockAddress(pfi.getAddr()));

    if (!pfi.isCacheMiss()) {
        recordDataHitSample(block);
    }

    if (!pfi.hasPC() || (!pfi.isCacheMiss() && !prefetchHit)) {
        return;
    }

    auto &tu = trainingUnits[pfi.getPC()];
    const bool metadataMiss = needsMetadataRead(tu.metadataBuffer, block);

    const auto prefetches =
        observeStreamAccess(pfi.getPC(), block, !metadataPortConnected());
    enqueuePrefetches(pfi.getPC(), pfi.getAddr(), pfi.isSecure(), prefetches);

    if (metadataPortConnected() && metadataMiss) {
        enqueueMetadataRead(pfi.getPC(), pfi.getAddr(), pfi.isSecure(), block);
    }
}

PacketPtr
Streamline::getPacket()
{
    if (prefetchQueue.empty() || prefetchQueue.front().ready > curTick()) {
        return nullptr;
    }

    PacketPtr pkt = prefetchQueue.front().pkt;
    prefetchQueue.pop_front();
    prefetchStats.pfIssued++;
    issuedPrefetches++;
    return pkt;
}

void
Streamline::resetStats()
{
    Base::resetStats();
    partitionTransitionStatBase = partitionTransitionCount;
    syncPartitionStats();
}

void
Streamline::preDumpStats()
{
    Base::preDumpStats();
    syncPartitionStats();
}

Tick
Streamline::nextPrefetchReadyTime() const
{
    return prefetchQueue.empty() ? MaxTick : prefetchQueue.front().ready;
}

bool
Streamline::hasPendingPacket()
{
    return nextPrefetchReadyTime() <= curTick();
}

std::pair<uint8_t, uint8_t>
Streamline::splitTriggerHash(uint64_t trigger_hash)
{
    constexpr uint64_t triggerMask = (1ULL << TriggerHashBits) - 1;
    constexpr uint64_t residualMask = (1ULL << ResidualTagBits) - 1;

    const uint64_t masked = trigger_hash & triggerMask;
    const auto partial = static_cast<uint8_t>(masked >> ResidualTagBits);
    const auto residual = static_cast<uint8_t>(masked & residualMask);

    return {partial, residual};
}

Addr
Streamline::metadataTriggerBlock(Addr reference_block, uint64_t residual_tag)
{
    constexpr uint64_t triggerMask = (1ULL << TriggerHashBits) - 1;
    constexpr uint64_t residualMask = (1ULL << ResidualTagBits) - 1;

    panic_if(residual_tag > residualMask,
             "Streamline residual tag %#x exceeds %u bits", residual_tag,
             ResidualTagBits);

    const Addr triggerHash =
        (reference_block & triggerMask & ~residualMask) | residual_tag;
    return (reference_block & ~triggerMask) | triggerHash;
}

bool
Streamline::canEncodeMetadataTarget(Addr trigger_block, Addr target_block)
{
    const auto delta = static_cast<int64_t>(target_block) -
                       static_cast<int64_t>(trigger_block);
    const int64_t minDelta = -(1LL << (MetadataTargetBits - 1));
    const int64_t maxDelta = (1LL << (MetadataTargetBits - 1)) - 1;
    return delta >= minDelta && delta <= maxDelta;
}

uint64_t
Streamline::encodeMetadataTarget(Addr trigger_block, Addr target_block)
{
    panic_if(!canEncodeMetadataTarget(trigger_block, target_block),
             "Streamline target delta %#x from trigger %#x exceeds signed "
             "31-bit metadata encoding",
             target_block, trigger_block);
    const auto delta = static_cast<int64_t>(target_block) -
                       static_cast<int64_t>(trigger_block);
    return static_cast<uint64_t>(delta) & MetadataTargetMask;
}

Addr
Streamline::decodeMetadataTarget(Addr trigger_block, uint64_t encoded_target)
{
    uint64_t raw = encoded_target & MetadataTargetMask;
    int64_t delta = raw & MetadataTargetSignBit ?
        static_cast<int64_t>(raw | ~MetadataTargetMask) :
        static_cast<int64_t>(raw);
    return static_cast<Addr>(static_cast<int64_t>(trigger_block) + delta);
}

Streamline::StreamEntry
Streamline::makeStreamEntry(uint64_t trigger_hash,
                            const std::vector<Addr> &targets)
{
    panic_if(targets.size() != StreamEntryTargetCount,
             "Streamline metadata entries require %u targets, got %u",
             StreamEntryTargetCount, targets.size());

    StreamEntry entry;
    entry.valid = true;
    std::tie(entry.partialTag, entry.residualTag) =
        splitTriggerHash(trigger_hash);
    std::copy(targets.begin(), targets.end(), entry.targets.begin());
    return entry;
}

std::vector<Addr>
Streamline::advanceTrainingStream(const std::vector<Addr> &current_stream,
                                  Addr address)
{
    panic_if(current_stream.size() > CompletedStreamLength,
             "Streamline training stream exceeded %u entries",
             CompletedStreamLength);

    std::vector<Addr> next(current_stream);
    if (next.size() == CompletedStreamLength) {
        next.erase(next.begin());
    }
    next.push_back(address);
    return next;
}

std::pair<std::vector<Addr>, std::vector<Addr>>
Streamline::alignStreams(const std::vector<Addr> &old_stream,
                         const std::vector<Addr> &new_stream)
{
    panic_if(old_stream.size() != CompletedStreamLength,
             "Streamline old stream length must be %u, got %u",
             CompletedStreamLength, old_stream.size());
    panic_if(new_stream.size() != CompletedStreamLength,
             "Streamline new stream length must be %u, got %u",
             CompletedStreamLength, new_stream.size());

    const auto lastMatchable =
        old_stream.begin() + (CompletedStreamLength - 1);
    const auto match =
        std::find(old_stream.begin(), lastMatchable, new_stream.front());
    panic_if(match == lastMatchable,
             "Unable to align Streamline stream on trigger %#x",
             new_stream.front());

    const std::size_t prefixLen = std::distance(old_stream.begin(), match) + 1;
    std::vector<Addr> aligned(old_stream.begin(),
                              old_stream.begin() + prefixLen);

    const std::size_t suffixSlots = CompletedStreamLength - aligned.size();
    const std::size_t copied = std::min(suffixSlots, new_stream.size() - 1);
    aligned.insert(aligned.end(), new_stream.begin() + 1,
                   new_stream.begin() + 1 + copied);

    std::vector<Addr> leftover;
    if (1 + copied < new_stream.size()) {
        leftover.push_back(aligned.back());
        leftover.insert(leftover.end(), new_stream.begin() + 1 + copied,
                        new_stream.end());
    }

    return {aligned, leftover};
}

uint64_t
Streamline::metadataSetCount(uint64_t metadata_entries,
                             uint64_t metadata_assoc)
{
    panic_if(metadata_assoc == 0,
             "Streamline metadata associativity must be non-zero");
    panic_if(metadata_entries % metadata_assoc != 0,
             "Streamline metadata entries (%u) must be divisible by "
             "associativity (%u)",
             metadata_entries, metadata_assoc);
    return metadata_entries / metadata_assoc;
}

uint64_t
Streamline::activeMetadataSetCount(uint64_t partition_level,
                                   uint64_t max_metadata_sets,
                                   uint64_t sample_set_count)
{
    switch (partition_level) {
        case 0:
            return std::min(sample_set_count, max_metadata_sets);
        case 1:
            return max_metadata_sets / 2;
        case 2:
            return max_metadata_sets;
        default:
            panic("Unsupported Streamline partition level %u",
                  partition_level);
    }
}

bool
Streamline::isMetadataSetActive(uint64_t metadata_set,
                                uint64_t partition_level,
                                uint64_t max_metadata_sets,
                                uint64_t sample_set_count)
{
    const auto activeSetCount = activeMetadataSetCount(
        partition_level, max_metadata_sets, sample_set_count);
    panic_if(metadata_set >= max_metadata_sets,
             "Metadata set %u exceeds max metadata set count %u", metadata_set,
             max_metadata_sets);
    panic_if(activeSetCount == 0 || (max_metadata_sets % activeSetCount) != 0,
             "Streamline metadata set geometry requires max sets (%u) to be "
             "divisible by active set count (%u)",
             max_metadata_sets, activeSetCount);

    const auto activeSetStride = max_metadata_sets / activeSetCount;
    return (metadata_set % activeSetStride) == 0;
}

Addr
Streamline::metadataLineAddress(Addr metadata_base,
                                uint64_t metadata_line_stride,
                                uint64_t max_metadata_sets,
                                uint64_t metadata_set, uint64_t partial_tag)
{
    panic_if(max_metadata_sets == 0,
             "Streamline metadata store must contain at least one set");
    panic_if(metadata_set >= max_metadata_sets,
             "Metadata set %u exceeds max metadata set count %u", metadata_set,
             max_metadata_sets);
    panic_if(
        metadata_line_stride < max_metadata_sets,
        "Streamline metadata line stride %u must cover max metadata sets %u",
        metadata_line_stride, max_metadata_sets);
    panic_if(partial_tag >= (1ULL << PartialTagBits),
             "Metadata partial tag %#x exceeds %u bits", partial_tag,
             PartialTagBits);

    const Addr line_index =
        (partial_tag * metadata_line_stride) + metadata_set;
    return metadata_base + (line_index * MetadataBlockBytes);
}

std::vector<uint64_t>
Streamline::packMetadataBlock(const std::vector<uint64_t> &entries,
                              Addr reference_block)
{
    panic_if(
        entries.size() % MetadataSlotFieldCount != 0,
        "Streamline metadata block payload length %u is not divisible by %u",
        entries.size(), MetadataSlotFieldCount);

    const std::size_t num_entries = entries.size() / MetadataSlotFieldCount;
    panic_if(num_entries > MetadataBlockEntryCount,
             "Streamline metadata blocks hold at most %u entries, got %u",
             MetadataBlockEntryCount, num_entries);

    std::vector<uint64_t> packed(MetadataBlockWordCount, UINT64_MAX);
    for (std::size_t i = 0; i < num_entries; ++i) {
        const auto offset = i * MetadataSlotFieldCount;
        const auto residual = entries[offset];
        panic_if(residual >= (1ULL << ResidualTagBits),
                 "Streamline residual tag %#x exceeds %u bits", residual,
                 ResidualTagBits);

        const Addr triggerBlock =
            metadataTriggerBlock(reference_block, residual);
        std::array<uint64_t, StreamEntryTargetCount> encodedTargets = {};
        bool encodable = true;
        for (std::size_t target = 0; target < StreamEntryTargetCount;
             ++target) {
            const auto absoluteTarget = entries[offset + 1 + target];
            if (!canEncodeMetadataTarget(triggerBlock, absoluteTarget)) {
                encodable = false;
                break;
            }
            encodedTargets[target] =
                encodeMetadataTarget(triggerBlock, absoluteTarget);
        }
        if (!encodable) {
            continue;
        }

        __uint128_t slot = residual;
        for (std::size_t target = 0; target < StreamEntryTargetCount;
             ++target) {
            slot |= static_cast<__uint128_t>(encodedTargets[target])
                    << (ResidualTagBits + MetadataTargetBits * target);
        }

        packed[2 * i] = static_cast<uint64_t>(slot);
        packed[2 * i + 1] = static_cast<uint64_t>(slot >> 64);
    }
    return packed;
}

std::vector<uint64_t>
Streamline::unpackMetadataBlock(
    const std::vector<uint64_t> &packed_block, Addr reference_block)
{
    panic_if(packed_block.size() != MetadataBlockWordCount,
             "Streamline metadata blocks require %u 64-bit words, got %u",
             MetadataBlockWordCount, packed_block.size());

    std::vector<uint64_t> entries;
    for (std::size_t i = 0; i < MetadataBlockEntryCount; ++i) {
        const __uint128_t slot =
            static_cast<__uint128_t>(packed_block[2 * i]) |
            (static_cast<__uint128_t>(packed_block[2 * i + 1]) << 64);
        if (slot == InvalidMetadataSlot || slot == 0) {
            continue;
        }

        const auto residual =
            static_cast<uint64_t>(slot & ((1ULL << ResidualTagBits) - 1));
        const Addr triggerBlock =
            metadataTriggerBlock(reference_block, residual);
        entries.push_back(residual);
        for (std::size_t target = 0; target < StreamEntryTargetCount;
             ++target) {
            const auto shift = ResidualTagBits + MetadataTargetBits * target;
            const auto encoded =
                static_cast<uint64_t>((slot >> shift) & MetadataTargetMask);
            entries.push_back(decodeMetadataTarget(triggerBlock, encoded));
        }
    }
    return entries;
}

std::size_t
Streamline::chooseMetadataVictim(const std::vector<uint64_t> &etrs,
                                 const std::vector<bool> &valids)
{
    panic_if(etrs.size() != valids.size(),
             "Streamline metadata replacement state size mismatch: %u vs %u",
             etrs.size(), valids.size());
    panic_if(etrs.empty(), "Streamline metadata victim choice requires slots");

    for (std::size_t i = 0; i < valids.size(); ++i) {
        if (!valids[i]) {
            return i;
        }
    }

    return std::distance(etrs.begin(),
                         std::max_element(etrs.begin(), etrs.end()));
}

uint64_t
Streamline::metadataHitScore(double accuracy)
{
    if (accuracy < 0.10) {
        return 0;
    }
    if (accuracy < 0.25) {
        return 2;
    }
    if (accuracy < 0.50) {
        return 3;
    }
    if (accuracy < 0.70) {
        return 4;
    }
    if (accuracy < 0.90) {
        return 6;
    }
    if (accuracy < 0.95) {
        return 7;
    }
    return 8;
}

uint64_t
Streamline::selectPartitionLevel(const std::vector<uint64_t> &scores,
                                 uint64_t current_level)
{
    panic_if(scores.size() != PartitionLevelCount,
             "Streamline partition score vector size must be %u, got %u",
             PartitionLevelCount, scores.size());
    panic_if(current_level >= PartitionLevelCount,
             "Unsupported current Streamline partition level %u",
             current_level);

    uint64_t best_level = current_level;
    uint64_t best_score = scores[current_level];
    for (std::size_t level = 0; level < scores.size(); ++level) {
        if (scores[level] > best_score) {
            best_level = level;
            best_score = scores[level];
        }
    }
    return best_level;
}

uint64_t
Streamline::sampledPartitionLevel(uint64_t metadata_set,
                                  uint64_t max_metadata_sets,
                                  uint64_t sample_set_count)
{
    const auto sampleSets = std::min(sample_set_count, max_metadata_sets);
    if (!isMetadataSetActive(metadata_set, 0, max_metadata_sets, sampleSets)) {
        return InvalidPartitionLevel;
    }

    const auto sampleSetStride = max_metadata_sets / sampleSets;
    return (metadata_set / sampleSetStride) % PartitionLevelCount;
}

std::vector<Addr>
Streamline::updateMetadataBuffer(const std::vector<Addr> &current_buffer,
                                 const std::vector<Addr> &stream_entry,
                                 uint64_t buffer_entries)
{
    panic_if(stream_entry.size() != CompletedStreamLength,
             "Streamline metadata buffer entries must be %u addresses, got %u",
             CompletedStreamLength, stream_entry.size());
    panic_if(buffer_entries == 0,
             "Streamline metadata buffer must hold entries");

    auto entries = decodeMetadataBuffer(current_buffer, CompletedStreamLength);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&stream_entry](const auto &entry) {
                                     return !entry.empty() &&
                                            entry.front() ==
                                                stream_entry.front();
                                 }),
                  entries.end());
    entries.insert(entries.begin(), stream_entry);

    const auto max_entries = static_cast<std::size_t>(buffer_entries);
    if (entries.size() > max_entries) {
        entries.resize(max_entries);
    }

    return encodeMetadataBuffer(entries);
}

std::vector<Addr>
Streamline::planBufferedPrefetch(const std::vector<Addr> &current_buffer,
                                 Addr address, uint64_t degree)
{
    panic_if(degree == 0, "Streamline prefetch degree must be non-zero");

    const auto entries =
        decodeMetadataBuffer(current_buffer, CompletedStreamLength);
    const auto *entry = findBufferedEntryWithSuccessor(entries, address);
    if (entry == nullptr) {
        return {};
    }

    const auto match = std::find(entry->begin(), entry->end(), address);
    const auto available =
        static_cast<std::size_t>(entry->end() - std::next(match));
    const auto issued = std::min<std::size_t>(available, degree);

    return std::vector<Addr>(std::next(match), std::next(match, issued + 1));
}

bool
Streamline::needsMetadataRead(const std::vector<Addr> &current_buffer,
                              Addr address)
{
    const auto entries =
        decodeMetadataBuffer(current_buffer, CompletedStreamLength);
    return findBufferedEntryWithSuccessor(entries, address) == nullptr;
}

uint64_t
Streamline::updateDegree(uint64_t insertions, uint64_t max_degree,
                         uint64_t low_insertion_threshold,
                         uint64_t mid_insertion_threshold,
                         uint64_t high_insertion_threshold)
{
    if (insertions < low_insertion_threshold) {
        return max_degree;
    }
    if (insertions < mid_insertion_threshold) {
        return std::min<uint64_t>(max_degree, 3);
    }
    if (insertions < high_insertion_threshold) {
        return std::min<uint64_t>(max_degree, 2);
    }
    return 1;
}

std::pair<uint16_t, uint32_t>
Streamline::metadataSignature(const std::vector<Addr> &stream_entry)
{
    panic_if(stream_entry.size() != CompletedStreamLength,
             "Streamline sampler entries must contain %u addresses, got %u",
             CompletedStreamLength, stream_entry.size());

    constexpr uint64_t triggerMask = (1ULL << TriggerHashBits) - 1;
    return {
        static_cast<uint16_t>(stream_entry.front() & triggerMask),
        static_cast<uint32_t>(stream_entry[1] & MetadataTargetMask),
    };
}

std::pair<uint64_t, uint64_t>
Streamline::metadataSamplerCoordinates(uint64_t metadata_set) const
{
    panic_if(metadata_set >= maxMetadataSets,
             "Metadata set %u exceeds max metadata set count %u", metadata_set,
             maxMetadataSets);

    return {
        metadata_set % MetadataSamplerSetCount,
        (metadata_set / MetadataSamplerSetCount) % MetadataSamplerBucketCount,
    };
}

uint64_t
Streamline::predictMetadataSamplerEtr(
    uint64_t metadata_set, const std::vector<Addr> &stream_entry) const
{
    const auto [triggerHash, firstTargetHash] =
        metadataSignature(stream_entry);
    const auto [samplerSet, samplerBucket] =
        metadataSamplerCoordinates(metadata_set);
    const auto &bucket = metadataSampler[samplerSet][samplerBucket];

    for (const auto &entry : bucket) {
        if (!entry.valid) {
            continue;
        }
        if (entry.triggerHash == triggerHash &&
            entry.firstTargetHash == firstTargetHash) {
            return entry.etr;
        }
    }

    return MaxMetadataEtr;
}

uint64_t
Streamline::chooseMetadataVictimForEntries(
    uint64_t metadata_set, const std::vector<Addr> &flattened_entries) const
{
    panic_if(
        flattened_entries.empty() ||
            flattened_entries.size() % CompletedStreamLength != 0,
        "Streamline victim selection entries must be a non-empty multiple of "
        "%u addresses, got %u",
        CompletedStreamLength, flattened_entries.size());

    std::vector<uint64_t> predictedEtrs;
    std::vector<bool> valids;
    for (std::size_t offset = 0; offset < flattened_entries.size();
         offset += CompletedStreamLength) {
        const std::vector<Addr> entry(flattened_entries.begin() + offset,
                                      flattened_entries.begin() + offset +
                                          CompletedStreamLength);
        predictedEtrs.push_back(
            predictMetadataSamplerEtr(metadata_set, entry));
        valids.push_back(true);
    }

    return chooseMetadataVictim(predictedEtrs, valids);
}

void
Streamline::trainMetadataSampler(uint64_t metadata_set,
                                 const std::vector<Addr> &stream_entry,
                                 Addr pc)
{
    const auto [triggerHash, firstTargetHash] =
        metadataSignature(stream_entry);
    const auto [samplerSet, samplerBucket] =
        metadataSamplerCoordinates(metadata_set);
    auto &bucket = metadataSampler[samplerSet][samplerBucket];

    std::size_t touched = MetadataSamplerAssoc;
    for (std::size_t i = 0; i < bucket.size(); ++i) {
        const auto &entry = bucket[i];
        if (!entry.valid) {
            continue;
        }
        if (entry.triggerHash == triggerHash &&
            entry.firstTargetHash == firstTargetHash) {
            touched = i;
            break;
        }
    }

    if (touched == MetadataSamplerAssoc) {
        touched = 0;
        bool foundInvalid = false;
        for (std::size_t i = 0; i < bucket.size(); ++i) {
            if (!bucket[i].valid) {
                touched = i;
                foundInvalid = true;
                break;
            }
        }
        if (!foundInvalid) {
            for (std::size_t i = 1; i < bucket.size(); ++i) {
                if (bucket[i].etr > bucket[touched].etr ||
                    (bucket[i].etr == bucket[touched].etr &&
                     bucket[i].timestamp < bucket[touched].timestamp)) {
                    touched = i;
                }
            }
        }
    }

    for (std::size_t i = 0; i < bucket.size(); ++i) {
        auto &entry = bucket[i];
        if (!entry.valid && i != touched) {
            continue;
        }

        if (i == touched) {
            entry.valid = true;
            entry.triggerHash = triggerHash;
            entry.firstTargetHash = firstTargetHash;
            entry.pcHash = static_cast<uint16_t>(pc & 0xffffu);
            entry.etr = 0;
            entry.timestamp = ++metadataSamplerTimestamp;
        } else {
            entry.etr = std::min<uint8_t>(MaxMetadataEtr, entry.etr + 1);
        }
    }
}

uint64_t
Streamline::computeMetadataSet(Addr address) const
{
    return (address >> TriggerHashBits) % maxMetadataSets;
}

uint64_t
Streamline::computeTriggerHash(Addr address) const
{
    constexpr uint64_t triggerMask = (1ULL << TriggerHashBits) - 1;
    return address & triggerMask;
}

Addr
Streamline::runtimeMetadataLineAddress(Addr address) const
{
    const auto triggerHash = computeTriggerHash(address);
    const auto triggerFields = splitTriggerHash(triggerHash);
    return metadataLineAddress(metadataBaseAddr, metadataLineStride,
                               maxMetadataSets, computeMetadataSet(address),
                               triggerFields.first);
}

double
Streamline::currentPrefetchAccuracy() const
{
    if (issuedPrefetches == 0) {
        return 0.0;
    }
    return static_cast<double>(usefulPrefetches) /
           static_cast<double>(issuedPrefetches);
}

void
Streamline::syncPartitionStats()
{
    streamlineStats.currentPartitionLevel = currentPartitionLevel;
    streamlineStats.partitionTransitions =
        partitionTransitionCount - partitionTransitionStatBase;
    streamlineStats.partitionSampledAccesses = partitionSampledAccesses;
    streamlineStats.partitionScoreLevel0 = partitionScores[0];
    streamlineStats.partitionScoreLevel1 = partitionScores[1];
    streamlineStats.partitionScoreLevel2 = partitionScores[2];
}

void
Streamline::recordPartitionSample(uint64_t partition_level, uint64_t score)
{
    if (partition_level == InvalidPartitionLevel) {
        return;
    }
    panic_if(partition_level >= PartitionLevelCount,
             "Unsupported sampled Streamline partition level %u",
             partition_level);

    partitionScores[partition_level] += score;
    partitionSampledAccesses++;
    syncPartitionStats();
    if (partitionSampledAccesses < PartitionUpdateInterval) {
        return;
    }

    const std::vector<uint64_t> scores(partitionScores.begin(),
                                       partitionScores.end());
    const auto nextPartitionLevel =
        selectPartitionLevel(scores, currentPartitionLevel);
    if (nextPartitionLevel != currentPartitionLevel) {
        currentPartitionLevel = nextPartitionLevel;
        partitionTransitionCount++;
    }
    partitionScores.fill(0);
    partitionSampledAccesses = 0;
    syncPartitionStats();
}

void
Streamline::recordDataHitSample(Addr address)
{
    recordPartitionSample(
        sampledPartitionLevel(computeMetadataSet(address), maxMetadataSets,
                              std::min<uint64_t>(64, maxMetadataSets)),
        DataHitScore);
}

void
Streamline::recordMetadataHitSample(Addr address)
{
    const auto score = metadataHitScore(currentPrefetchAccuracy());
    if (score == 0) {
        return;
    }
    recordPartitionSample(
        sampledPartitionLevel(computeMetadataSet(address), maxMetadataSets,
                              std::min<uint64_t>(64, maxMetadataSets)),
        score);
}

std::vector<Addr>
Streamline::lookupMetadataEntry(Addr address)
{
    const auto triggerHash = computeTriggerHash(address);
    const auto [partial, residual] = splitTriggerHash(triggerHash);
    const auto lineAddr = metadataLineAddress(
        metadataBaseAddr, metadataLineStride, maxMetadataSets,
        computeMetadataSet(address), partial);

    const auto it = metadataLines.find(lineAddr);
    if (it == metadataLines.end()) {
        return {};
    }

    auto &line = it->second;
    const auto entries = unpackMetadataBlock(line.packed, address);
    for (std::size_t offset = 0; offset < entries.size();
         offset += MetadataSlotFieldCount) {
        if (entries[offset] != residual) {
            continue;
        }

        const auto slot = offset / MetadataSlotFieldCount;
        touchMetadataEntryState(line.etrs,
                                entries.size() / MetadataSlotFieldCount, slot);

        std::vector<Addr> stream_entry = {address};
        stream_entry.insert(stream_entry.end(), entries.begin() + offset + 1,
                            entries.begin() + offset + MetadataSlotFieldCount);
        trainMetadataSampler(computeMetadataSet(address), stream_entry, 0);
        return stream_entry;
    }

    return {};
}

void
Streamline::storeMetadataEntry(const std::vector<Addr> &stream_entry, Addr pc)
{
    panic_if(stream_entry.size() != CompletedStreamLength,
             "Streamline metadata entries must contain %u addresses, got %u",
             CompletedStreamLength, stream_entry.size());

    const auto triggerHash = computeTriggerHash(stream_entry.front());
    const auto [partial, residual] = splitTriggerHash(triggerHash);
    const auto lineAddr = metadataLineAddress(
        metadataBaseAddr, metadataLineStride, maxMetadataSets,
        computeMetadataSet(stream_entry.front()), partial);

    auto &line = metadataLines[lineAddr];
    if (line.packed.empty()) {
        line.packed.assign(MetadataBlockWordCount, UINT64_MAX);
    }

    auto entries = unpackMetadataBlock(line.packed, stream_entry.front());
    bool updated = false;
    std::size_t touchedSlot = 0;
    for (std::size_t offset = 0; offset < entries.size();
         offset += MetadataSlotFieldCount) {
        if (entries[offset] != residual) {
            continue;
        }

        entries[offset] = residual;
        std::copy(stream_entry.begin() + 1, stream_entry.end(),
                  entries.begin() + offset + 1);
        updated = true;
        touchedSlot = offset / MetadataSlotFieldCount;
        break;
    }

    if (!updated) {
        if (entries.size() <
            MetadataBlockEntryCount * MetadataSlotFieldCount) {
            entries.push_back(residual);
            entries.insert(entries.end(), stream_entry.begin() + 1,
                           stream_entry.end());
            touchedSlot = (entries.size() / MetadataSlotFieldCount) - 1;
        } else {
            std::vector<Addr> residentEntries;
            residentEntries.reserve(MetadataBlockEntryCount *
                                    CompletedStreamLength);
            for (std::size_t offset = 0; offset < entries.size();
                 offset += MetadataSlotFieldCount) {
                const auto residentTriggerHash =
                    (static_cast<uint64_t>(partial) << ResidualTagBits) |
                    entries[offset];
                residentEntries.push_back(residentTriggerHash);
                residentEntries.insert(
                    residentEntries.end(), entries.begin() + offset + 1,
                    entries.begin() + offset + MetadataSlotFieldCount);
            }

            touchedSlot = chooseMetadataVictimForEntries(
                computeMetadataSet(stream_entry.front()), residentEntries);
            const auto victimOffset = touchedSlot * MetadataSlotFieldCount;
            entries[victimOffset] = residual;
            std::copy(stream_entry.begin() + 1, stream_entry.end(),
                      entries.begin() + victimOffset + 1);
        }
    }

    touchMetadataEntryState(line.etrs, entries.size() / MetadataSlotFieldCount,
                            touchedSlot);
    line.packed = packMetadataBlock(entries, stream_entry.front());
    trainMetadataSampler(computeMetadataSet(stream_entry.front()),
                         stream_entry, pc);
    if (metadataPortConnected()) {
        enqueueMetadataWrite(lineAddr, line.packed);
    }
}

std::vector<Addr>
Streamline::observeStreamAccess(Addr pc, Addr address,
                                bool allow_metadata_lookup)
{
    auto &tu = trainingUnits[pc];
    if (tu.degree == 0) {
        tu.degree = maxDegree;
    }

    std::vector<Addr> prefetches =
        planBufferedPrefetch(tu.metadataBuffer, address, tu.degree);
    if (prefetches.empty()) {
        streamlineStats.metadataBufferMisses++;
    } else {
        streamlineStats.metadataBufferHits++;
    }
    if (prefetches.empty() && allow_metadata_lookup) {
        const auto storedEntry = lookupMetadataEntry(address);
        if (!storedEntry.empty()) {
            recordMetadataHitSample(address);
            tu.metadataBuffer = updateMetadataBuffer(
                tu.metadataBuffer, storedEntry, metadataBufferEntries);
            tu.metadataBufferInsertions++;
            streamlineStats.metadataBufferInsertions++;
            prefetches =
                planBufferedPrefetch(tu.metadataBuffer, address, tu.degree);
        }
    }

    tu.streamBuilder = advanceTrainingStream(tu.streamBuilder, address);
    if (tu.streamBuilder.size() == CompletedStreamLength) {
        std::vector<Addr> candidate = tu.streamBuilder;
        const auto buffered =
            decodeMetadataBuffer(tu.metadataBuffer, CompletedStreamLength);
        for (const auto &entry : buffered) {
            const auto lastMatchable =
                entry.begin() + (CompletedStreamLength - 1);
            if (std::find(entry.begin(), lastMatchable, candidate.front()) ==
                lastMatchable) {
                continue;
            }

            auto [aligned, leftover] = alignStreams(entry, candidate);
            candidate = aligned;
            if (leftover.size() == CompletedStreamLength) {
                storeMetadataEntry(leftover, pc);
            }
            break;
        }

        storeMetadataEntry(candidate, pc);
        tu.metadataBuffer = updateMetadataBuffer(tu.metadataBuffer, candidate,
                                                 metadataBufferEntries);
        tu.metadataBufferInsertions++;
        streamlineStats.metadataBufferInsertions++;
    }

    tu.epochAccesses++;
    if (epochSize != 0 && tu.epochAccesses >= epochSize) {
        tu.degree = updateDegree(tu.metadataBufferInsertions, maxDegree,
                                 lowInsertionThreshold, midInsertionThreshold,
                                 highInsertionThreshold);
        tu.epochAccesses = 0;
        tu.metadataBufferInsertions = 0;
    }

    return prefetches;
}

void
Streamline::enqueuePrefetches(Addr pc, Addr access_addr, bool secure,
                              const std::vector<Addr> &block_indices)
{
    for (const auto block_index : block_indices) {
        const Addr targetAddr = block_index << lBlkSize;
        if (!samePage(targetAddr, access_addr)) {
            continue;
        }

        const auto duplicate =
            std::find_if(prefetchQueue.begin(), prefetchQueue.end(),
                         [targetAddr, secure](const auto &entry) {
                             return entry.pkt != nullptr &&
                                    entry.pkt->getAddr() == targetAddr &&
                                    entry.pkt->isSecure() == secure;
                         });
        if (duplicate != prefetchQueue.end()) {
            continue;
        }

        RequestPtr req =
            std::make_shared<Request>(targetAddr, blkSize, 0, requestorId);
        if (secure) {
            req->setFlags(Request::SECURE);
        }
        req->taskId(context_switch_task_id::Prefetcher);
        req->setPC(pc);

        req->setPFSource(PrefetchSourceType::Streamline);
        req->setPFDepth(1);
        req->setXsMetadata(
            Request::XsMetadata(PrefetchSourceType::Streamline, 1));

        PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
        pkt->allocate();
        prefetchQueue.push_back({curTick() + clockPeriod(), pkt});
    }
}

void
Streamline::enqueueMetadataRead(Addr pc, Addr access_addr, bool secure,
                                Addr trigger_block)
{
    if (!metadataPortConnected()) {
        return;
    }

    const Addr lineAddr = runtimeMetadataLineAddress(trigger_block);
    RequestPtr req = std::make_shared<Request>(lineAddr, MetadataBlockBytes, 0,
                                               metadataRequestorId);
    if (secure) {
        req->setFlags(Request::SECURE);
    }
    req->taskId(context_switch_task_id::Prefetcher);
    req->setPC(pc);

    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();
    pkt->senderState =
        new MetadataSenderState(MetadataSenderState::Type::Read, pc,
                                access_addr, secure, trigger_block, lineAddr);
    streamlineStats.metadataReadIssued++;
    metadataRequestQueue.push_back(pkt);
    sendNextMetadataRequest();
}

void
Streamline::enqueueMetadataWrite(Addr line_addr,
                                 const std::vector<uint64_t> &packed)
{
    if (!metadataPortConnected()) {
        return;
    }

    panic_if(packed.size() != MetadataBlockWordCount,
             "Streamline metadata writes require %u 64-bit words, got %u",
             MetadataBlockWordCount, packed.size());

    RequestPtr req = std::make_shared<Request>(line_addr, MetadataBlockBytes,
                                               0, metadataRequestorId);
    req->taskId(context_switch_task_id::Prefetcher);

    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
    pkt->allocate();
    std::memcpy(pkt->getPtr<uint8_t>(), packed.data(), MetadataBlockBytes);
    pkt->senderState = new MetadataSenderState(
        MetadataSenderState::Type::Write, 0, 0, false, 0, line_addr);
    streamlineStats.metadataWritesIssued++;
    metadataRequestQueue.push_back(pkt);
    sendNextMetadataRequest();
}

void
Streamline::sendNextMetadataRequest()
{
    if (!metadataPortConnected() || activeMetadataRequest != nullptr ||
        metadataRequestQueue.empty()) {
        return;
    }

    activeMetadataRequest = metadataRequestQueue.front();
    metadataRequestBlocked =
        !metadataPort.sendTimingReq(activeMetadataRequest);
}

void
Streamline::recvMetadataReqRetry()
{
    if (activeMetadataRequest == nullptr || !metadataRequestBlocked) {
        return;
    }

    streamlineStats.metadataReqRetries++;
    metadataRequestBlocked =
        !metadataPort.sendTimingReq(activeMetadataRequest);
}

bool
Streamline::recvMetadataResp(PacketPtr pkt)
{
    panic_if(activeMetadataRequest == nullptr || pkt != activeMetadataRequest,
             "Streamline metadata response does not match the active request");

    auto *state = static_cast<MetadataSenderState *>(pkt->senderState);
    if (state->type == MetadataSenderState::Type::Read) {
        streamlineStats.metadataReadResponses++;
        std::vector<uint64_t> packed(MetadataBlockWordCount);
        std::memcpy(packed.data(), pkt->getConstPtr<uint8_t>(),
                    MetadataBlockBytes);
        metadataLines[state->lineAddr].packed = packed;

        const auto storedEntry = lookupMetadataEntry(state->triggerBlock);
        if (!storedEntry.empty()) {
            streamlineStats.metadataReadHits++;
            recordMetadataHitSample(state->triggerBlock);
            auto &tu = trainingUnits[state->pc];
            tu.metadataBuffer = updateMetadataBuffer(
                tu.metadataBuffer, storedEntry, metadataBufferEntries);
            tu.metadataBufferInsertions++;
            streamlineStats.metadataBufferInsertions++;
            const auto prefetches = planBufferedPrefetch(
                tu.metadataBuffer, state->triggerBlock, tu.degree);
            enqueuePrefetches(state->pc, state->accessAddr, state->secure,
                              prefetches);
        } else {
            streamlineStats.metadataReadMisses++;
        }
    } else {
        streamlineStats.metadataWriteResponses++;
    }

    metadataRequestQueue.pop_front();
    activeMetadataRequest = nullptr;
    metadataRequestBlocked = false;
    delete pkt->senderState;
    delete pkt;
    sendNextMetadataRequest();
    return true;
}

bool
Streamline::metadataPortConnected() const
{
    return metadataPort.isConnected();
}

uint64_t
Streamline::debugMetadataEntryTargetCount() const
{
    return StreamEntryTargetCount;
}

std::vector<uint64_t>
Streamline::debugTriggerFields(uint64_t trigger_hash) const
{
    const auto [partial, residual] = splitTriggerHash(trigger_hash);
    return {partial, residual};
}

std::vector<Addr>
Streamline::debugDescribeStreamEntry(uint64_t trigger_hash,
                                     const std::vector<Addr> &targets) const
{
    const auto entry = makeStreamEntry(trigger_hash, targets);

    std::vector<Addr> description = {entry.partialTag, entry.residualTag};
    description.insert(description.end(), entry.targets.begin(),
                       entry.targets.end());
    return description;
}

std::vector<Addr>
Streamline::debugAppendTrainingAddress(const std::vector<Addr> &current_stream,
                                       Addr address) const
{
    return advanceTrainingStream(current_stream, address);
}

std::pair<std::vector<Addr>, std::vector<Addr>>
Streamline::debugAlignStreams(const std::vector<Addr> &old_stream,
                              const std::vector<Addr> &new_stream) const
{
    return alignStreams(old_stream, new_stream);
}

uint64_t
Streamline::debugDegreeForInsertions(uint64_t insertions, uint64_t max_degree,
                                     uint64_t low_insertion_threshold,
                                     uint64_t mid_insertion_threshold,
                                     uint64_t high_insertion_threshold) const
{
    return updateDegree(insertions, max_degree, low_insertion_threshold,
                        mid_insertion_threshold, high_insertion_threshold);
}

uint64_t
Streamline::debugMetadataSetCount(uint64_t metadata_entries,
                                  uint64_t metadata_assoc) const
{
    return metadataSetCount(metadata_entries, metadata_assoc);
}

uint64_t
Streamline::debugActiveMetadataSetCount(uint64_t partition_level,
                                        uint64_t max_metadata_sets,
                                        uint64_t sample_set_count) const
{
    return activeMetadataSetCount(partition_level, max_metadata_sets,
                                  sample_set_count);
}

bool
Streamline::debugIsMetadataSetActive(uint64_t metadata_set,
                                     uint64_t partition_level,
                                     uint64_t max_metadata_sets,
                                     uint64_t sample_set_count) const
{
    return isMetadataSetActive(metadata_set, partition_level,
                               max_metadata_sets, sample_set_count);
}

Addr
Streamline::debugMetadataLineAddress(Addr metadata_base,
                                     uint64_t metadata_line_stride,
                                     uint64_t max_metadata_sets,
                                     uint64_t metadata_set,
                                     uint64_t partial_tag) const
{
    return metadataLineAddress(metadata_base, metadata_line_stride,
                               max_metadata_sets, metadata_set, partial_tag);
}

Addr
Streamline::debugRuntimeMetadataLineAddress(Addr address) const
{
    return runtimeMetadataLineAddress(address);
}

uint64_t
Streamline::debugChooseMetadataVictim(const std::vector<uint64_t> &etrs,
                                      const std::vector<bool> &valids) const
{
    return chooseMetadataVictim(etrs, valids);
}

std::vector<uint64_t>
Streamline::debugMetadataSamplerCoordinates(uint64_t metadata_set) const
{
    const auto [samplerSet, samplerBucket] =
        metadataSamplerCoordinates(metadata_set);
    return {samplerSet, samplerBucket};
}

void
Streamline::debugTrainMetadataSampler(uint64_t metadata_set,
                                      const std::vector<Addr> &stream_entry,
                                      Addr pc)
{
    trainMetadataSampler(metadata_set, stream_entry, pc);
}

uint64_t
Streamline::debugPredictMetadataSamplerEtr(
    uint64_t metadata_set, const std::vector<Addr> &stream_entry) const
{
    return predictMetadataSamplerEtr(metadata_set, stream_entry);
}

uint64_t
Streamline::debugChooseMetadataVictimForEntries(
    uint64_t metadata_set, const std::vector<Addr> &flattened_entries) const
{
    return chooseMetadataVictimForEntries(metadata_set, flattened_entries);
}

uint64_t
Streamline::debugMetadataHitScore(double accuracy) const
{
    return metadataHitScore(accuracy);
}

uint64_t
Streamline::debugSelectPartitionLevel(const std::vector<uint64_t> &scores,
                                      uint64_t current_level) const
{
    return selectPartitionLevel(scores, current_level);
}

uint64_t
Streamline::debugSampledPartitionLevel(uint64_t metadata_set,
                                       uint64_t max_metadata_sets,
                                       uint64_t sample_set_count) const
{
    return sampledPartitionLevel(metadata_set, max_metadata_sets,
                                 sample_set_count);
}

std::vector<uint64_t>
Streamline::debugPackMetadataBlock(const std::vector<uint64_t> &entries) const
{
    return packMetadataBlock(entries, 0);
}

std::vector<uint64_t>
Streamline::debugUnpackMetadataBlock(
    const std::vector<uint64_t> &packed_block) const
{
    return unpackMetadataBlock(packed_block, 0);
}

std::vector<Addr>
Streamline::debugUpdateMetadataBuffer(const std::vector<Addr> &current_buffer,
                                      const std::vector<Addr> &stream_entry,
                                      uint64_t buffer_entries) const
{
    return updateMetadataBuffer(current_buffer, stream_entry, buffer_entries);
}

std::vector<Addr>
Streamline::debugPlanBufferedPrefetch(const std::vector<Addr> &current_buffer,
                                      Addr address, uint64_t degree) const
{
    return planBufferedPrefetch(current_buffer, address, degree);
}

bool
Streamline::debugNeedsMetadataRead(const std::vector<Addr> &current_buffer,
                                   Addr address) const
{
    return needsMetadataRead(current_buffer, address);
}

void
Streamline::debugRecordPartitionSample(uint64_t partition_level,
                                       uint64_t score)
{
    recordPartitionSample(partition_level, score);
}

uint64_t
Streamline::debugCurrentPartitionLevel() const
{
    return currentPartitionLevel;
}

uint64_t
Streamline::debugCurrentPartitionLevelStat() const
{
    return streamlineStats.currentPartitionLevel.value();
}

uint64_t
Streamline::debugPartitionTransitionCount() const
{
    return partitionTransitionCount;
}

uint64_t
Streamline::debugPartitionTransitionsStat() const
{
    return streamlineStats.partitionTransitions.value();
}

std::vector<uint64_t>
Streamline::debugPartitionScores() const
{
    return std::vector<uint64_t>(partitionScores.begin(),
                                 partitionScores.end());
}

uint64_t
Streamline::debugPartitionSampledAccesses() const
{
    return partitionSampledAccesses;
}

uint64_t
Streamline::debugPartitionUpdateInterval() const
{
    return PartitionUpdateInterval;
}

void
Streamline::debugResetRuntimeState()
{
    trainingUnits.clear();
    metadataLines.clear();
    metadataSampler = {};
    partitionScores.fill(0);
    partitionSampledAccesses = 0;
    partitionTransitionCount = 0;
    partitionTransitionStatBase = 0;
    currentPartitionLevel = 2;
    metadataSamplerTimestamp = 0;
    syncPartitionStats();
    for (auto &entry : prefetchQueue) {
        delete entry.pkt;
    }
    prefetchQueue.clear();
    if (activeMetadataRequest != nullptr) {
        delete activeMetadataRequest->senderState;
        delete activeMetadataRequest;
        activeMetadataRequest = nullptr;
    }
    for (auto *pkt : metadataRequestQueue) {
        delete pkt->senderState;
        delete pkt;
    }
    metadataRequestQueue.clear();
    metadataRequestBlocked = false;
}

std::vector<Addr>
Streamline::debugObserveAccess(Addr pc, Addr address)
{
    return observeStreamAccess(pc, address, true);
}

void
Streamline::startup()
{
    Base::startup();
    syncPartitionStats();
}

} // namespace prefetch
} // namespace gem5
