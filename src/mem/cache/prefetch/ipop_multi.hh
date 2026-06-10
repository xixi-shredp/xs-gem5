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

#ifndef __MEM_CACHE_PREFETCH_IPOP_MULTI_HH__
#define __MEM_CACHE_PREFETCH_IPOP_MULTI_HH__

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "base/types.hh"
#include "mem/cache/prefetch/ipop_info.hh"
#include "mem/cache/prefetch/multi.hh"

namespace gem5
{

struct IPOPMultiPrefetcherParams;

namespace prefetch
{

class IPOPMulti : public Multi
{
  private:
    struct TableEntry
    {
        bool valid = false;
        Addr tag = 0;
        bool isSecure = false;
        uint64_t prefetcherIdBits = 0;
        bool accessDram = false;
    };

    struct PrefetcherCounters
    {
        uint64_t usefulLlC = 0;
        uint64_t usefulDram = 0;
        uint64_t pollutionLlC = 0;
        uint64_t pollutionDram = 0;
        uint64_t delay = 0;
        uint64_t bus = 0;
        uint64_t bank = 0;
    };

    enum class ControlState
    {
        On,
        Wait,
        Off,
    };

    struct PrefetcherRuntimeState
    {
        std::string name;
        ControlState state = ControlState::On;
        unsigned int level = 1;
        unsigned int maxLevel = 1;
        unsigned int childMaxDegree = 1;
        unsigned int offCountdown = 0;
        double lastPe = 0.0;
        bool enabled = true;
        uint64_t phasesOn = 0;
        uint64_t phasesWait = 0;
        uint64_t phasesOff = 0;
        uint64_t transitionsToOn = 0;
        uint64_t transitionsToWait = 0;
        uint64_t transitionsToOff = 0;
        uint64_t firstTransitionToOffPhase = 0;
    };

    struct IPOPMultiStats : public statistics::Group
    {
        explicit IPOPMultiStats(statistics::Group *parent);

        statistics::Scalar phaseCount;
        statistics::Scalar llcLatencyCycles;
        statistics::Scalar dramLatencyCycles;
        statistics::Vector pe;
        statistics::Vector currentLevel;
        statistics::Vector currentState;
        statistics::Vector phasesOn;
        statistics::Vector phasesWait;
        statistics::Vector phasesOff;
        statistics::Vector transitionsToOn;
        statistics::Vector transitionsToWait;
        statistics::Vector transitionsToOff;
        statistics::Vector firstTransitionToOffPhase;
        statistics::Vector usefulLlc;
        statistics::Vector usefulDram;
        statistics::Vector pollutionLlc;
        statistics::Vector pollutionDram;
        statistics::Vector delay;
        statistics::Vector bus;
        statistics::Vector bank;
        statistics::Vector totalUsefulLlc;
        statistics::Vector totalUsefulDram;
        statistics::Vector totalPollutionLlc;
        statistics::Vector totalPollutionDram;
        statistics::Vector totalDelay;
        statistics::Vector totalBus;
        statistics::Vector totalBank;
    };

    struct PendingPhaseCsvRecord
    {
        bool valid = false;
        uint64_t phaseIndex = 0;
        std::vector<double> peValues;
    };

    struct PendingPacket
    {
        PacketPtr pkt = nullptr;
        uint64_t prefetcherIdBits = 0;
    };

  public:
    explicit IPOPMulti(const IPOPMultiPrefetcherParams &p);

    PacketPtr getPacket() override;
    bool hasPendingPacket() override;
    Tick nextPrefetchReadyTime() const override;
    void notifyIpopPrefetchFill(const IPOPEventInfo &info) override;
    void notifyIpopPrefetchEviction(const IPOPEventInfo &info) override;
    void notifyIpopDemandHit(const IPOPEventInfo &info) override;
    void notifyIpopDemandMissComplete(const IPOPEventInfo &info) override;

  private:
    const unsigned int phaseLength;
    const unsigned int pfhtEntries;
    const unsigned int pohtEntries;
    const unsigned int tableTagBits;
    const unsigned int onLevels;
    const unsigned int offLevels;
    const Cycles idealDramLatency;
    const bool phaseOnMiss;
    const unsigned int warmupPhases;
    const Cycles tNoc;
    const Cycles tBus;
    const Cycles tBank;
    const unsigned int channelShift;
    const unsigned int channelBits;
    const unsigned int bankShift;
    const unsigned int bankBits;
    const bool recordPhasePeIpcCsv;
    const std::string phasePeIpcCsvPath;

    std::vector<TableEntry> pfht;
    std::vector<TableEntry> poht;
    std::vector<PrefetcherCounters> phaseCounters;
    std::vector<PrefetcherCounters> totalCounters;
    std::vector<PrefetcherRuntimeState> runtimeStates;
    IPOPMultiStats ipopStats;

    uint64_t demandAccesses;
    uint64_t completedPhases;
    Tick llcMissLatencySum;
    Tick dramMissLatencySum;
    uint64_t llcMissLatencySamples;
    uint64_t dramMissLatencySamples;
    Counter lastPhaseInsts;
    Counter lastPhaseCycles;
    PendingPhaseCsvRecord pendingPhaseCsvRecord;
    std::deque<PendingPacket> pendingPackets;

    uint64_t getPrefetcherIdBits(uint8_t prefetcher_index) const;
    Addr tableIndex(Addr addr, unsigned int entries) const;
    Addr tableTag(Addr addr) const;
    void forEachPrefetcherBit(
        uint64_t bits, const std::function<void(unsigned int)> &visitor) const;
    TableEntry *lookupTable(std::vector<TableEntry> &table,
                            unsigned int entries, Addr addr, bool is_secure);
    void collectReadyPackets();
    PendingPacket *findPendingPacket(Addr addr, bool is_secure);
    void updateLatencySums(bool access_dram, Tick latency);
    void maybeAdvancePhase();
    void evaluatePhase();
    void resetPhaseStats();
    void applyRuntimeState(unsigned int index);
    void updateStateForNegativePe(unsigned int index);
    unsigned int clampLevel(unsigned int index, unsigned int desired) const;
    double averageMissLatencyCycles(bool access_dram) const;
    double computePe(const PrefetcherCounters &counters, double llc_latency,
                     double dram_latency) const;
    bool bandwidthAvailable(double dram_latency) const;
    bool bandwidthConstrained(double dram_latency) const;
    uint64_t extractField(Addr addr, unsigned int shift,
                          unsigned int width) const;
    bool sameChannel(Addr lhs, Addr rhs) const;
    bool sameBank(Addr lhs, Addr rhs) const;
    double controlStateValue(ControlState state) const;
    const char *controlStateName(ControlState state) const;
    Counter totalCpuCycles() const;
    double currentPhaseIpc(Counter insts, Counter cycles) const;
    void initializeCsvLogging();
    void appendPendingPhaseCsvRecord(double next_phase_ipc);
    void updateExportedStats(double llc_latency, double dram_latency);
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_IPOP_MULTI_HH__
