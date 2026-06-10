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

#ifndef __MEM_CACHE_PREFETCH_STREAMLINE_HH__
#define __MEM_CACHE_PREFETCH_STREAMLINE_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem/cache/prefetch/base.hh"
#include "mem/port.hh"

namespace gem5
{

struct StreamlinePrefetcherParams;

namespace prefetch
{

class Streamline : public Base
{
  private:
    static constexpr std::size_t MetadataBlockEntryCount = 4;
    static constexpr std::size_t MetadataSlotFieldCount = 5;
    static constexpr std::size_t MetadataBlockWordCount = 8;
    static constexpr std::size_t MetadataSamplerSetCount = 8;
    static constexpr std::size_t MetadataSamplerBucketCount = 32;
    static constexpr std::size_t MetadataSamplerAssoc = 10;
    static constexpr std::size_t PartitionLevelCount = 3;
    static constexpr uint64_t InvalidPartitionLevel = PartitionLevelCount;
    static constexpr uint64_t PartitionUpdateInterval = 1ULL << 15;
    static constexpr uint64_t DataHitScore = 16;
    static constexpr uint8_t MaxMetadataEtr = 7;
    static constexpr std::size_t StreamEntryTargetCount = 4;
    static constexpr std::size_t CompletedStreamLength =
        StreamEntryTargetCount + 1;
    static constexpr unsigned TriggerHashBits = 10;
    static constexpr unsigned PartialTagBits = 6;
    static constexpr unsigned ResidualTagBits =
        TriggerHashBits - PartialTagBits;

    struct StreamEntry
    {
        bool valid = false;
        uint8_t partialTag = 0;
        uint8_t residualTag = 0;
        std::array<Addr, StreamEntryTargetCount> targets = {};
    };
    struct TrainingUnit
    {
        std::vector<Addr> streamBuilder;
        std::vector<Addr> metadataBuffer;
        uint64_t metadataBufferInsertions = 0;
        uint64_t epochAccesses = 0;
        uint64_t degree = 0;
    };
    struct MetadataLine
    {
        std::vector<uint64_t> packed;
        std::array<uint8_t, MetadataBlockEntryCount> etrs = {};
    };
    struct MetadataSamplerEntry
    {
        bool valid = false;
        uint16_t triggerHash = 0;
        uint32_t firstTargetHash = 0;
        uint16_t pcHash = 0;
        uint8_t etr = MaxMetadataEtr;
        uint64_t timestamp = 0;
    };
    struct QueuedPrefetch
    {
        Tick ready = MaxTick;
        PacketPtr pkt = nullptr;
    };
    struct MetadataSenderState : public Packet::SenderState
    {
        enum class Type
        {
            Read,
            Write,
        };

        MetadataSenderState(Type type, Addr pc, Addr access_addr, bool secure,
                            Addr trigger_block, Addr line_addr)
            : type(type),
              pc(pc),
              accessAddr(access_addr),
              secure(secure),
              triggerBlock(trigger_block),
              lineAddr(line_addr)
        {}

        Type type;
        Addr pc;
        Addr accessAddr;
        bool secure;
        Addr triggerBlock;
        Addr lineAddr;
    };
    class MetadataPort : public RequestPort
    {
      public:
        MetadataPort(const std::string &name, Streamline &owner);

      protected:
        void recvReqRetry() override;
        bool recvTimingResp(PacketPtr pkt) override;
        void
        recvTimingSnoopReq(PacketPtr pkt) override
        {}
        void
        recvFunctionalSnoop(PacketPtr pkt) override
        {}
        Tick
        recvAtomicSnoop(PacketPtr pkt) override
        {
            return 0;
        }

      private:
        Streamline &owner;
    };
    struct StreamlineStatGroup : public statistics::Group
    {
        StreamlineStatGroup(statistics::Group *parent);

        statistics::Scalar metadataBufferHits;
        statistics::Scalar metadataBufferMisses;
        statistics::Scalar metadataBufferInsertions;
        statistics::Scalar metadataReadIssued;
        statistics::Scalar metadataReadResponses;
        statistics::Scalar metadataReadHits;
        statistics::Scalar metadataReadMisses;
        statistics::Formula metadataReadHitRate;
        statistics::Scalar metadataWritesIssued;
        statistics::Scalar metadataWriteResponses;
        statistics::Scalar metadataReqRetries;
        statistics::Scalar currentPartitionLevel;
        statistics::Scalar partitionTransitions;
        statistics::Scalar partitionSampledAccesses;
        statistics::Scalar partitionScoreLevel0;
        statistics::Scalar partitionScoreLevel1;
        statistics::Scalar partitionScoreLevel2;
    };

    const Addr metadataBaseAddr;
    const uint64_t metadataStoreAssoc;
    const uint64_t metadataStoreEntries;
    const uint64_t metadataLineStride;
    const uint64_t metadataBufferEntries;
    const uint64_t maxDegree;
    const uint64_t epochSize;
    const uint64_t lowInsertionThreshold;
    const uint64_t midInsertionThreshold;
    const uint64_t highInsertionThreshold;
    const uint64_t maxMetadataSets;
    const RequestorID metadataRequestorId;
    MetadataPort metadataPort;

    std::unordered_map<Addr, TrainingUnit> trainingUnits;
    std::unordered_map<Addr, MetadataLine> metadataLines;
    std::array<
        std::array<std::array<MetadataSamplerEntry, MetadataSamplerAssoc>,
                   MetadataSamplerBucketCount>,
        MetadataSamplerSetCount>
        metadataSampler = {};
    StreamlineStatGroup streamlineStats;
    std::array<uint64_t, PartitionLevelCount> partitionScores = {};
    uint64_t partitionSampledAccesses = 0;
    uint64_t partitionTransitionCount = 0;
    uint64_t partitionTransitionStatBase = 0;
    uint64_t currentPartitionLevel = 2;
    uint64_t metadataSamplerTimestamp = 0;

    static std::pair<uint8_t, uint8_t> splitTriggerHash(uint64_t trigger_hash);
    static StreamEntry makeStreamEntry(uint64_t trigger_hash,
                                       const std::vector<Addr> &targets);
    static std::vector<Addr>
    advanceTrainingStream(const std::vector<Addr> &current_stream,
                          Addr address);
    static std::pair<std::vector<Addr>, std::vector<Addr>>
    alignStreams(const std::vector<Addr> &old_stream,
                 const std::vector<Addr> &new_stream);
    static uint64_t metadataSetCount(uint64_t metadata_entries,
                                     uint64_t metadata_assoc);
    static uint64_t activeMetadataSetCount(uint64_t partition_level,
                                           uint64_t max_metadata_sets,
                                           uint64_t sample_set_count);
    static bool isMetadataSetActive(uint64_t metadata_set,
                                    uint64_t partition_level,
                                    uint64_t max_metadata_sets,
                                    uint64_t sample_set_count);
    static Addr metadataLineAddress(Addr metadata_base,
                                    uint64_t metadata_line_stride,
                                    uint64_t max_metadata_sets,
                                    uint64_t metadata_set,
                                    uint64_t partial_tag);
    static Addr metadataTriggerBlock(Addr reference_block,
                                     uint64_t residual_tag);
    static bool canEncodeMetadataTarget(Addr trigger_block,
                                        Addr target_block);
    static uint64_t encodeMetadataTarget(Addr trigger_block,
                                         Addr target_block);
    static Addr decodeMetadataTarget(Addr trigger_block,
                                     uint64_t encoded_target);
    static std::vector<uint64_t>
    packMetadataBlock(const std::vector<uint64_t> &entries,
                      Addr reference_block);
    static std::vector<uint64_t>
    unpackMetadataBlock(const std::vector<uint64_t> &packed_block,
                        Addr reference_block);
    static std::size_t chooseMetadataVictim(const std::vector<uint64_t> &etrs,
                                            const std::vector<bool> &valids);
    static uint64_t metadataHitScore(double accuracy);
    static uint64_t selectPartitionLevel(const std::vector<uint64_t> &scores,
                                         uint64_t current_level);
    static uint64_t sampledPartitionLevel(uint64_t metadata_set,
                                          uint64_t max_metadata_sets,
                                          uint64_t sample_set_count);
    static std::vector<Addr>
    updateMetadataBuffer(const std::vector<Addr> &current_buffer,
                         const std::vector<Addr> &stream_entry,
                         uint64_t buffer_entries);
    static std::vector<Addr>
    planBufferedPrefetch(const std::vector<Addr> &current_buffer, Addr address,
                         uint64_t degree);
    static bool needsMetadataRead(const std::vector<Addr> &current_buffer,
                                  Addr address);
    static uint64_t updateDegree(uint64_t insertions, uint64_t max_degree,
                                 uint64_t low_insertion_threshold,
                                 uint64_t mid_insertion_threshold,
                                 uint64_t high_insertion_threshold);
    static std::pair<uint16_t, uint32_t>
    metadataSignature(const std::vector<Addr> &stream_entry);
    void syncPartitionStats();
    std::pair<uint64_t, uint64_t>
    metadataSamplerCoordinates(uint64_t metadata_set) const;
    uint64_t
    predictMetadataSamplerEtr(uint64_t metadata_set,
                              const std::vector<Addr> &stream_entry) const;
    uint64_t chooseMetadataVictimForEntries(
        uint64_t metadata_set,
        const std::vector<Addr> &flattened_entries) const;
    void trainMetadataSampler(uint64_t metadata_set,
                              const std::vector<Addr> &stream_entry, Addr pc);
    uint64_t computeMetadataSet(Addr address) const;
    uint64_t computeTriggerHash(Addr address) const;
    Addr runtimeMetadataLineAddress(Addr address) const;
    std::vector<Addr> lookupMetadataEntry(Addr address);
    void storeMetadataEntry(const std::vector<Addr> &stream_entry, Addr pc);
    double currentPrefetchAccuracy() const;
    void recordPartitionSample(uint64_t partition_level, uint64_t score);
    void recordDataHitSample(Addr address);
    void recordMetadataHitSample(Addr address);
    std::vector<Addr> observeStreamAccess(Addr pc, Addr address,
                                          bool allow_metadata_lookup);
    void enqueuePrefetches(Addr pc, Addr access_addr, bool secure,
                           const std::vector<Addr> &block_indices);
    void enqueueMetadataRead(Addr pc, Addr access_addr, bool secure,
                             Addr trigger_block);
    void enqueueMetadataWrite(Addr line_addr,
                              const std::vector<uint64_t> &packed);
    void sendNextMetadataRequest();
    void recvMetadataReqRetry();
    bool recvMetadataResp(PacketPtr pkt);
    bool metadataPortConnected() const;

    std::deque<QueuedPrefetch> prefetchQueue;
    std::deque<PacketPtr> metadataRequestQueue;
    PacketPtr activeMetadataRequest = nullptr;
    bool metadataRequestBlocked = false;

  public:
    explicit Streamline(const StreamlinePrefetcherParams &p);
    ~Streamline() override;

    Port &getPort(const std::string &if_name, PortID idx) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void resetStats() override;
    void preDumpStats() override;
    PacketPtr getPacket() override;
    bool hasPendingPacket() override;

    Tick nextPrefetchReadyTime() const override;
    void rxHint(BaseMMU::Translation *) override {}
    void pfHitNotify(float, PrefetchSourceType,
                     const PacketPtr &) override {}

    uint64_t debugMetadataEntryTargetCount() const;
    std::vector<uint64_t> debugTriggerFields(uint64_t trigger_hash) const;
    std::vector<Addr>
    debugDescribeStreamEntry(uint64_t trigger_hash,
                             const std::vector<Addr> &targets) const;
    std::vector<Addr>
    debugAppendTrainingAddress(const std::vector<Addr> &current_stream,
                               Addr address) const;
    std::pair<std::vector<Addr>, std::vector<Addr>>
    debugAlignStreams(const std::vector<Addr> &old_stream,
                      const std::vector<Addr> &new_stream) const;
    uint64_t debugDegreeForInsertions(uint64_t insertions, uint64_t max_degree,
                                      uint64_t low_insertion_threshold,
                                      uint64_t mid_insertion_threshold,
                                      uint64_t high_insertion_threshold) const;
    uint64_t debugMetadataSetCount(uint64_t metadata_entries,
                                   uint64_t metadata_assoc) const;
    uint64_t debugActiveMetadataSetCount(uint64_t partition_level,
                                         uint64_t max_metadata_sets,
                                         uint64_t sample_set_count) const;
    bool debugIsMetadataSetActive(uint64_t metadata_set,
                                  uint64_t partition_level,
                                  uint64_t max_metadata_sets,
                                  uint64_t sample_set_count) const;
    Addr debugMetadataLineAddress(Addr metadata_base,
                                  uint64_t metadata_line_stride,
                                  uint64_t max_metadata_sets,
                                  uint64_t metadata_set,
                                  uint64_t partial_tag) const;
    Addr debugRuntimeMetadataLineAddress(Addr address) const;
    uint64_t debugChooseMetadataVictim(const std::vector<uint64_t> &etrs,
                                       const std::vector<bool> &valids) const;
    std::vector<uint64_t>
    debugMetadataSamplerCoordinates(uint64_t metadata_set) const;
    void debugTrainMetadataSampler(uint64_t metadata_set,
                                   const std::vector<Addr> &stream_entry,
                                   Addr pc);
    uint64_t debugPredictMetadataSamplerEtr(
        uint64_t metadata_set, const std::vector<Addr> &stream_entry) const;
    uint64_t debugChooseMetadataVictimForEntries(
        uint64_t metadata_set,
        const std::vector<Addr> &flattened_entries) const;
    uint64_t debugMetadataHitScore(double accuracy) const;
    uint64_t debugSelectPartitionLevel(const std::vector<uint64_t> &scores,
                                       uint64_t current_level) const;
    uint64_t debugSampledPartitionLevel(uint64_t metadata_set,
                                        uint64_t max_metadata_sets,
                                        uint64_t sample_set_count) const;
    std::vector<uint64_t>
    debugPackMetadataBlock(const std::vector<uint64_t> &entries) const;
    std::vector<uint64_t>
    debugUnpackMetadataBlock(const std::vector<uint64_t> &packed_block) const;
    std::vector<Addr>
    debugUpdateMetadataBuffer(const std::vector<Addr> &current_buffer,
                              const std::vector<Addr> &stream_entry,
                              uint64_t buffer_entries) const;
    std::vector<Addr>
    debugPlanBufferedPrefetch(const std::vector<Addr> &current_buffer,
                              Addr address, uint64_t degree) const;
    bool debugNeedsMetadataRead(const std::vector<Addr> &current_buffer,
                                Addr address) const;
    void debugRecordPartitionSample(uint64_t partition_level, uint64_t score);
    uint64_t debugCurrentPartitionLevel() const;
    uint64_t debugCurrentPartitionLevelStat() const;
    uint64_t debugPartitionTransitionCount() const;
    uint64_t debugPartitionTransitionsStat() const;
    std::vector<uint64_t> debugPartitionScores() const;
    uint64_t debugPartitionSampledAccesses() const;
    uint64_t debugPartitionUpdateInterval() const;
    void debugResetRuntimeState();
    std::vector<Addr> debugObserveAccess(Addr pc, Addr address);

    void startup() override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_STREAMLINE_HH__
