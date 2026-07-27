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

#include "mem/cache/prefetch/ipop_multi.hh"

#include <algorithm>
#include <fstream>
#include <limits>

#include "base/logging.hh"
#include "cpu/base.hh"
#include "mem/cache/prefetch/queued.hh"
#include "params/IPOPMultiPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

IPOPMulti::IPOPMultiStats::IPOPMultiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(phaseCount, statistics::units::Count::get(),
               "Completed I-POP phases"),
      ADD_STAT(llcLatencyCycles, statistics::units::Cycle::get(),
               "Most recent average LLC miss latency in cycles"),
      ADD_STAT(dramLatencyCycles, statistics::units::Cycle::get(),
               "Most recent average DRAM miss latency in cycles"),
      ADD_STAT(pe, statistics::units::Unspecified::get(),
               "Most recent PE value per child prefetcher"),
      ADD_STAT(currentLevel, statistics::units::Count::get(),
               "Current ON level per child prefetcher"),
      ADD_STAT(
          currentState, statistics::units::Unspecified::get(),
          "Current control state per child prefetcher (ON=1 WA=0 OFF=-1)"),
      ADD_STAT(phasesOn, statistics::units::Count::get(),
               "Number of completed phases spent in ON per child prefetcher"),
      ADD_STAT(phasesWait, statistics::units::Count::get(),
               "Number of completed phases spent in WA per child prefetcher"),
      ADD_STAT(phasesOff, statistics::units::Count::get(),
               "Number of completed phases spent in OFF per child prefetcher"),
      ADD_STAT(transitionsToOn, statistics::units::Count::get(),
               "Number of transitions into ON per child prefetcher"),
      ADD_STAT(transitionsToWait, statistics::units::Count::get(),
               "Number of transitions into WA per child prefetcher"),
      ADD_STAT(transitionsToOff, statistics::units::Count::get(),
               "Number of transitions into OFF per child prefetcher"),
      ADD_STAT(firstTransitionToOffPhase, statistics::units::Count::get(),
               "First completed phase index that triggered OFF per child "
               "prefetcher"),
      ADD_STAT(usefulLlc, statistics::units::Count::get(),
               "Useful LLC prefetches in the most recent phase"),
      ADD_STAT(usefulDram, statistics::units::Count::get(),
               "Useful DRAM prefetches in the most recent phase"),
      ADD_STAT(pollutionLlc, statistics::units::Count::get(),
               "Pollution LLC misses in the most recent phase"),
      ADD_STAT(pollutionDram, statistics::units::Count::get(),
               "Pollution DRAM misses in the most recent phase"),
      ADD_STAT(delay, statistics::units::Count::get(),
               "Delay events in the most recent phase"),
      ADD_STAT(bus, statistics::units::Count::get(),
               "Bus contention events in the most recent phase"),
      ADD_STAT(bank, statistics::units::Count::get(),
               "Bank contention events in the most recent phase"),
      ADD_STAT(totalUsefulLlc, statistics::units::Count::get(),
               "Cumulative useful LLC prefetches per child prefetcher"),
      ADD_STAT(totalUsefulDram, statistics::units::Count::get(),
               "Cumulative useful DRAM prefetches per child prefetcher"),
      ADD_STAT(totalPollutionLlc, statistics::units::Count::get(),
               "Cumulative pollution LLC misses per child prefetcher"),
      ADD_STAT(totalPollutionDram, statistics::units::Count::get(),
               "Cumulative pollution DRAM misses per child prefetcher"),
      ADD_STAT(totalDelay, statistics::units::Count::get(),
               "Cumulative delay events per child prefetcher"),
      ADD_STAT(totalBus, statistics::units::Count::get(),
               "Cumulative bus contention events per child prefetcher"),
      ADD_STAT(totalBank, statistics::units::Count::get(),
               "Cumulative bank contention events per child prefetcher")
{}

IPOPMulti::IPOPMulti(const IPOPMultiPrefetcherParams &p)
    : Multi(p),
      phaseLength(p.phase_length),
      pfhtEntries(p.pfht_entries),
      pohtEntries(p.poht_entries),
      tableTagBits(p.table_tag_bits),
      onLevels(p.ipop_on_levels),
      offLevels(p.ipop_off_levels),
      idealDramLatency(p.ideal_dram_latency),
      phaseOnMiss(p.phase_on_miss),
      warmupPhases(p.warmup_phases),
      tNoc(p.t_noc),
      tBus(p.t_bus),
      tBank(p.t_bank),
      channelShift(p.channel_shift),
      channelBits(p.channel_bits),
      bankShift(p.bank_shift),
      bankBits(p.bank_bits),
      recordPhasePeIpcCsv(p.record_phase_pe_ipc_csv),
      phasePeIpcCsvPath(p.phase_pe_ipc_csv_path),
      pfht(pfhtEntries),
      poht(pohtEntries),
      phaseCounters(prefetchers.size()),
      totalCounters(prefetchers.size()),
      runtimeStates(prefetchers.size()),
      ipopStats(this),
      demandAccesses(0),
      completedPhases(0),
      llcMissLatencySum(0),
      dramMissLatencySum(0),
      llcMissLatencySamples(0),
      dramMissLatencySamples(0),
      lastPhaseInsts(0),
      lastPhaseCycles(0)
{
    fatal_if(prefetchers.size() > sizeof(uint64_t) * 8,
             "%s supports at most %u child prefetchers for one-hot I-POP IDs",
             name(), unsigned(sizeof(uint64_t) * 8));
    fatal_if(pfhtEntries == 0 || pohtEntries == 0,
             "%s requires non-zero PfHT and PoHT entry counts", name());
    fatal_if(tableTagBits == 0 || tableTagBits > 63,
             "%s requires table_tag_bits in the range [1, 63]", name());
    fatal_if(onLevels == 0 || offLevels == 0,
             "%s requires non-zero ON and OFF level counts", name());
    fatal_if(channelBits >= 64 || bankBits >= 64,
             "%s requires channel_bits and bank_bits to be less than 64",
             name());
    fatal_if((channelBits != 0 && channelShift >= 64) ||
                 (bankBits != 0 && bankShift >= 64),
             "%s requires decode shifts to be less than 64", name());
    fatal_if(channelBits != 0 && channelShift + channelBits > 64,
             "%s channel decode exceeds 64 address bits", name());
    fatal_if(bankBits != 0 && bankShift + bankBits > 64,
             "%s bank decode exceeds 64 address bits", name());
    fatal_if(recordPhasePeIpcCsv && phasePeIpcCsvPath.empty(),
             "%s requires phase_pe_ipc_csv_path when "
             "record_phase_pe_ipc_csv is enabled",
             name());

    ipopStats.pe.init(prefetchers.size());
    ipopStats.currentLevel.init(prefetchers.size());
    ipopStats.currentState.init(prefetchers.size());
    ipopStats.phasesOn.init(prefetchers.size());
    ipopStats.phasesWait.init(prefetchers.size());
    ipopStats.phasesOff.init(prefetchers.size());
    ipopStats.transitionsToOn.init(prefetchers.size());
    ipopStats.transitionsToWait.init(prefetchers.size());
    ipopStats.transitionsToOff.init(prefetchers.size());
    ipopStats.firstTransitionToOffPhase.init(prefetchers.size());
    ipopStats.usefulLlc.init(prefetchers.size());
    ipopStats.usefulDram.init(prefetchers.size());
    ipopStats.pollutionLlc.init(prefetchers.size());
    ipopStats.pollutionDram.init(prefetchers.size());
    ipopStats.delay.init(prefetchers.size());
    ipopStats.bus.init(prefetchers.size());
    ipopStats.bank.init(prefetchers.size());
    ipopStats.totalUsefulLlc.init(prefetchers.size());
    ipopStats.totalUsefulDram.init(prefetchers.size());
    ipopStats.totalPollutionLlc.init(prefetchers.size());
    ipopStats.totalPollutionDram.init(prefetchers.size());
    ipopStats.totalDelay.init(prefetchers.size());
    ipopStats.totalBus.init(prefetchers.size());
    ipopStats.totalBank.init(prefetchers.size());

    for (unsigned int i = 0; i < prefetchers.size(); ++i) {
        auto *queued = dynamic_cast<Queued *>(prefetchers[i]);
        fatal_if(!queued,
                 "%s only supports QueuedPrefetcher children, but child %u "
                 "(%s) is not queue-based",
                 name(), i, prefetchers[i]->name());

        auto &state = runtimeStates[i];
        state.name = prefetchers[i]->name();
        state.maxLevel = onLevels;
        state.childMaxDegree = prefetchers[i]->getIpopMaxAggressivenessLevel();
        fatal_if(state.childMaxDegree == 0,
                 "%s child %s exposes zero I-POP runtime degrees", name(),
                 state.name);
        state.state = ControlState::On;
        state.level = state.maxLevel;
        state.enabled = true;
        state.offCountdown = 0;
        applyRuntimeState(i);
    }

    for (unsigned int i = 0; i < prefetchers.size(); ++i) {
        const auto &state = runtimeStates[i];
        ipopStats.pe.subname(i, state.name);
        ipopStats.currentLevel.subname(i, state.name);
        ipopStats.currentState.subname(i, state.name);
        ipopStats.phasesOn.subname(i, state.name);
        ipopStats.phasesWait.subname(i, state.name);
        ipopStats.phasesOff.subname(i, state.name);
        ipopStats.transitionsToOn.subname(i, state.name);
        ipopStats.transitionsToWait.subname(i, state.name);
        ipopStats.transitionsToOff.subname(i, state.name);
        ipopStats.firstTransitionToOffPhase.subname(i, state.name);
        ipopStats.usefulLlc.subname(i, state.name);
        ipopStats.usefulDram.subname(i, state.name);
        ipopStats.pollutionLlc.subname(i, state.name);
        ipopStats.pollutionDram.subname(i, state.name);
        ipopStats.delay.subname(i, state.name);
        ipopStats.bus.subname(i, state.name);
        ipopStats.bank.subname(i, state.name);
        ipopStats.totalUsefulLlc.subname(i, state.name);
        ipopStats.totalUsefulDram.subname(i, state.name);
        ipopStats.totalPollutionLlc.subname(i, state.name);
        ipopStats.totalPollutionDram.subname(i, state.name);
        ipopStats.totalDelay.subname(i, state.name);
        ipopStats.totalBus.subname(i, state.name);
        ipopStats.totalBank.subname(i, state.name);
    }

    if (recordPhasePeIpcCsv) {
        initializeCsvLogging();
    }
}

uint64_t
IPOPMulti::getPrefetcherIdBits(uint8_t prefetcher_index) const
{ return uint64_t{1} << prefetcher_index; }

Addr
IPOPMulti::tableIndex(Addr addr, unsigned int entries) const
{ return blockIndex(addr) % entries; }

Addr
IPOPMulti::tableTag(Addr addr) const
{
    const Addr mask = (Addr{1} << tableTagBits) - 1;
    return blockIndex(addr) & mask;
}

void
IPOPMulti::forEachPrefetcherBit(
    uint64_t bits, const std::function<void(unsigned int)> &visitor) const
{
    fatal_if(bits == 0, "%s requires a non-zero I-POP prefetcher bitmask",
             name());
    fatal_if((bits >> prefetchers.size()) != 0,
             "%s received out-of-range I-POP prefetcher bitmask: %#llx",
             name(), static_cast<unsigned long long>(bits));

    while (bits != 0) {
        const unsigned int index = __builtin_ctzll(bits);
        visitor(index);
        bits &= (bits - 1);
    }
}

IPOPMulti::TableEntry *
IPOPMulti::lookupTable(std::vector<TableEntry> &table, unsigned int entries,
                       Addr addr, bool is_secure)
{
    TableEntry &entry = table[tableIndex(addr, entries)];
    if (!entry.valid || entry.tag != tableTag(addr) ||
        entry.isSecure != is_secure) {
        return nullptr;
    }

    return &entry;
}

void
IPOPMulti::collectReadyPackets()
{
    for (unsigned int pf_index = 0; pf_index < prefetchers.size();
         ++pf_index) {
        auto *prefetcher = prefetchers[pf_index];
        while (prefetcher->nextPrefetchReadyTime() <= curTick()) {
            PacketPtr pkt = prefetcher->getPacket();
            panic_if(!pkt, "Prefetcher is ready but didn't return a packet.");

            PendingPacket *pending =
                findPendingPacket(pkt->getBlockAddr(blkSize), pkt->isSecure());
            if (pending) {
                pending->prefetcherIdBits |= getPrefetcherIdBits(pf_index);
                delete pkt;
                continue;
            }

            pendingPackets.push_back(
                PendingPacket{pkt, getPrefetcherIdBits(pf_index)});
        }
    }
}

IPOPMulti::PendingPacket *
IPOPMulti::findPendingPacket(Addr addr, bool is_secure)
{
    for (auto &pending : pendingPackets) {
        if (pending.pkt->getBlockAddr(blkSize) == addr &&
            pending.pkt->isSecure() == is_secure) {
            return &pending;
        }
    }

    return nullptr;
}

void
IPOPMulti::updateLatencySums(bool access_dram, Tick latency)
{
    if (access_dram) {
        dramMissLatencySum += latency;
        dramMissLatencySamples++;
    } else {
        llcMissLatencySum += latency;
        llcMissLatencySamples++;
    }
}

unsigned int
IPOPMulti::clampLevel(unsigned int index, unsigned int desired) const
{ return std::max(1U, std::min(desired, runtimeStates[index].maxLevel)); }

double
IPOPMulti::averageMissLatencyCycles(bool access_dram) const
{
    const Tick sum = access_dram ? dramMissLatencySum : llcMissLatencySum;
    const uint64_t samples =
        access_dram ? dramMissLatencySamples : llcMissLatencySamples;

    if (samples == 0) {
        return access_dram ? static_cast<double>(idealDramLatency) : 0.0;
    }

    return static_cast<double>(ticksToCycles(sum)) /
           static_cast<double>(samples);
}

double
IPOPMulti::computePe(const PrefetcherCounters &counters, double llc_latency,
                     double dram_latency) const
{
    const double iupf =
        counters.usefulLlC * llc_latency + counters.usefulDram * dram_latency;
    const double ipoll = counters.pollutionLlC * llc_latency +
                         counters.pollutionDram * dram_latency;
    const double ilat = counters.delay * static_cast<double>(tNoc) +
                        counters.bus * static_cast<double>(tBus) +
                        counters.bank * static_cast<double>(tBank);

    return iupf - ipoll - ilat;
}

bool
IPOPMulti::bandwidthAvailable(double dram_latency) const
{ return dram_latency <= static_cast<double>(idealDramLatency) * 1.5; }

bool
IPOPMulti::bandwidthConstrained(double dram_latency) const
{ return dram_latency >= static_cast<double>(idealDramLatency) * 3.0; }

uint64_t
IPOPMulti::extractField(Addr addr, unsigned int shift,
                        unsigned int width) const
{
    if (width == 0) {
        return 0;
    }

    const uint64_t mask = width == 64 ? std::numeric_limits<uint64_t>::max()
                                      : ((uint64_t{1} << width) - 1);
    return (static_cast<uint64_t>(addr) >> shift) & mask;
}

bool
IPOPMulti::sameChannel(Addr lhs, Addr rhs) const
{
    if (channelBits == 0) {
        return true;
    }

    return extractField(lhs, channelShift, channelBits) ==
           extractField(rhs, channelShift, channelBits);
}

bool
IPOPMulti::sameBank(Addr lhs, Addr rhs) const
{
    if (bankBits == 0) {
        return false;
    }

    return extractField(lhs, bankShift, bankBits) ==
           extractField(rhs, bankShift, bankBits);
}

double
IPOPMulti::controlStateValue(ControlState state) const
{
    switch (state) {
        case ControlState::On:
            return 1.0;
        case ControlState::Wait:
            return 0.0;
        case ControlState::Off:
            return -1.0;
    }

    panic("%s reached invalid I-POP control state", name());
}

const char *
IPOPMulti::controlStateName(ControlState state) const
{
    switch (state) {
        case ControlState::On:
            return "ON";
        case ControlState::Wait:
            return "WA";
        case ControlState::Off:
            return "OFF";
    }

    panic("%s reached invalid I-POP control state", name());
}

Counter
IPOPMulti::totalCpuCycles() const
{
    return ticksToCycles(curTick());
}

double
IPOPMulti::currentPhaseIpc(Counter insts, Counter cycles) const
{
    const Counter delta_insts = insts - lastPhaseInsts;
    const Counter delta_cycles = cycles - lastPhaseCycles;
    if (delta_cycles == 0) {
        return 0.0;
    }

    return static_cast<double>(delta_insts) /
           static_cast<double>(delta_cycles);
}

void
IPOPMulti::initializeCsvLogging()
{
    std::ifstream input(phasePeIpcCsvPath);
    const bool needs_header =
        !input.good() || input.peek() == std::ifstream::traits_type::eof();
    input.close();

    std::ofstream output(phasePeIpcCsvPath, std::ios::app);
    fatal_if(!output.is_open(), "%s could not open CSV output path %s", name(),
             phasePeIpcCsvPath);

    if (!needs_header) {
        return;
    }

    output << "phase_index,next_phase_ipc";
    for (const auto &state : runtimeStates) {
        output << "," << state.name << "_pe";
    }
    output << "\n";
}

void
IPOPMulti::appendPendingPhaseCsvRecord(double next_phase_ipc)
{
    if (!recordPhasePeIpcCsv || !pendingPhaseCsvRecord.valid) {
        return;
    }

    std::ofstream output(phasePeIpcCsvPath, std::ios::app);
    fatal_if(!output.is_open(), "%s could not append CSV output path %s",
             name(), phasePeIpcCsvPath);

    output << pendingPhaseCsvRecord.phaseIndex << "," << next_phase_ipc;
    for (double pe : pendingPhaseCsvRecord.peValues) {
        output << "," << pe;
    }
    output << "\n";
}

void
IPOPMulti::updateExportedStats(double llc_latency, double dram_latency)
{
    ipopStats.phaseCount++;
    ipopStats.llcLatencyCycles = llc_latency;
    ipopStats.dramLatencyCycles = dram_latency;

    for (unsigned int i = 0; i < runtimeStates.size(); ++i) {
        const auto &state = runtimeStates[i];
        const auto &counters = phaseCounters[i];
        ipopStats.pe[i] = state.lastPe;
        ipopStats.currentLevel[i] = state.level;
        ipopStats.currentState[i] = controlStateValue(state.state);
        ipopStats.phasesOn[i] = state.phasesOn;
        ipopStats.phasesWait[i] = state.phasesWait;
        ipopStats.phasesOff[i] = state.phasesOff;
        ipopStats.transitionsToOn[i] = state.transitionsToOn;
        ipopStats.transitionsToWait[i] = state.transitionsToWait;
        ipopStats.transitionsToOff[i] = state.transitionsToOff;
        ipopStats.firstTransitionToOffPhase[i] =
            state.firstTransitionToOffPhase;
        ipopStats.usefulLlc[i] = counters.usefulLlC;
        ipopStats.usefulDram[i] = counters.usefulDram;
        ipopStats.pollutionLlc[i] = counters.pollutionLlC;
        ipopStats.pollutionDram[i] = counters.pollutionDram;
        ipopStats.delay[i] = counters.delay;
        ipopStats.bus[i] = counters.bus;
        ipopStats.bank[i] = counters.bank;
        ipopStats.totalUsefulLlc[i] = totalCounters[i].usefulLlC;
        ipopStats.totalUsefulDram[i] = totalCounters[i].usefulDram;
        ipopStats.totalPollutionLlc[i] = totalCounters[i].pollutionLlC;
        ipopStats.totalPollutionDram[i] = totalCounters[i].pollutionDram;
        ipopStats.totalDelay[i] = totalCounters[i].delay;
        ipopStats.totalBus[i] = totalCounters[i].bus;
        ipopStats.totalBank[i] = totalCounters[i].bank;
    }
}

void
IPOPMulti::applyRuntimeState(unsigned int index)
{
    auto *prefetcher = prefetchers[index];
    auto &state = runtimeStates[index];
    state.level = clampLevel(index, state.level);
    const unsigned int degree = std::max(
        1U, (state.childMaxDegree * state.level + state.maxLevel - 1) /
                state.maxLevel);

    if (state.state == ControlState::On) {
        state.enabled = true;
        prefetcher->setIpopEnabled(true);
        prefetcher->setIpopAggressivenessLevel(degree);
    } else {
        state.enabled = false;
        prefetcher->setIpopEnabled(false);
        prefetcher->setIpopAggressivenessLevel(degree);
    }
}

void
IPOPMulti::updateStateForNegativePe(unsigned int index)
{
    if (completedPhases < warmupPhases) {
        return;
    }

    auto &state = runtimeStates[index];
    if (state.lastPe > 0.0 || state.state != ControlState::On) {
        return;
    }

    state.state = ControlState::Off;
    state.offCountdown = offLevels;
    state.level = state.maxLevel;
    state.transitionsToOff++;
    if (state.firstTransitionToOffPhase == 0) {
        state.firstTransitionToOffPhase = completedPhases + 1;
    }
}

void
IPOPMulti::resetPhaseStats()
{
    demandAccesses = 0;
    llcMissLatencySum = 0;
    dramMissLatencySum = 0;
    llcMissLatencySamples = 0;
    dramMissLatencySamples = 0;
    for (auto &counters : phaseCounters) {
        counters = PrefetcherCounters{};
    }
}

void
IPOPMulti::evaluatePhase()
{
    const double llc_latency = averageMissLatencyCycles(false);
    const double dram_latency = averageMissLatencyCycles(true);
    const Counter total_insts = BaseCPU::numSimulatedInsts();
    const Counter total_cycles = totalCpuCycles();
    const double phase_ipc = currentPhaseIpc(total_insts, total_cycles);

    for (unsigned int i = 0; i < phaseCounters.size(); ++i) {
        runtimeStates[i].lastPe =
            computePe(phaseCounters[i], llc_latency, dram_latency);
    }

    for (auto &state : runtimeStates) {
        switch (state.state) {
            case ControlState::On:
                state.phasesOn++;
                break;
            case ControlState::Wait:
                state.phasesWait++;
                break;
            case ControlState::Off:
                state.phasesOff++;
                break;
        }

        if (state.state == ControlState::Off) {
            if (state.offCountdown > 1) {
                state.offCountdown--;
            } else {
                state.state = ControlState::Wait;
                state.offCountdown = 0;
                state.transitionsToWait++;
            }
        }
    }

    for (unsigned int i = 0; i < runtimeStates.size(); ++i) {
        updateStateForNegativePe(i);
    }

    if (!bandwidthConstrained(dram_latency)) {
        int wait_index = -1;
        double best_wait_pe = -std::numeric_limits<double>::infinity();
        for (unsigned int i = 0; i < runtimeStates.size(); ++i) {
            const auto &state = runtimeStates[i];
            if (state.state == ControlState::Wait &&
                state.lastPe > best_wait_pe) {
                best_wait_pe = state.lastPe;
                wait_index = i;
            }
        }

        if (wait_index >= 0) {
            auto &state = runtimeStates[wait_index];
            state.state = ControlState::On;
            state.level = 1;
            state.transitionsToOn++;
        }
    }

    if (bandwidthConstrained(dram_latency)) {
        int worst_index = -1;
        double worst_pe = std::numeric_limits<double>::infinity();
        for (unsigned int i = 0; i < runtimeStates.size(); ++i) {
            const auto &state = runtimeStates[i];
            if (state.state == ControlState::On && state.level > 1 &&
                state.lastPe < worst_pe) {
                worst_pe = state.lastPe;
                worst_index = i;
            }
        }

        if (worst_index >= 0) {
            runtimeStates[worst_index].level--;
        }
    } else if (bandwidthAvailable(dram_latency)) {
        int best_index = -1;
        double best_pe = -std::numeric_limits<double>::infinity();
        for (unsigned int i = 0; i < runtimeStates.size(); ++i) {
            const auto &state = runtimeStates[i];
            if (state.state == ControlState::On &&
                state.level < state.maxLevel && state.lastPe > best_pe) {
                best_pe = state.lastPe;
                best_index = i;
            }
        }

        if (best_index >= 0) {
            runtimeStates[best_index].level++;
        }
    }

    for (unsigned int i = 0; i < runtimeStates.size(); ++i) {
        applyRuntimeState(i);
    }

    if (recordPhasePeIpcCsv) {
        appendPendingPhaseCsvRecord(phase_ipc);
        pendingPhaseCsvRecord.valid = true;
        pendingPhaseCsvRecord.phaseIndex = completedPhases;
        pendingPhaseCsvRecord.peValues.clear();
        pendingPhaseCsvRecord.peValues.reserve(runtimeStates.size());
        for (const auto &state : runtimeStates) {
            pendingPhaseCsvRecord.peValues.push_back(state.lastPe);
        }
    }

    lastPhaseInsts = total_insts;
    lastPhaseCycles = total_cycles;
    updateExportedStats(llc_latency, dram_latency);
    completedPhases++;
    resetPhaseStats();
}

void
IPOPMulti::maybeAdvancePhase()
{
    if (phaseLength == 0 || demandAccesses < phaseLength) {
        return;
    }

    evaluatePhase();
}

void
IPOPMulti::calculatePrefetch(const PrefetchInfo &pfi,
                             std::vector<AddrPriority> &addresses)
{
    calculatePrefetch(pfi, addresses, false, PrefetchSourceType::PF_NONE,
                      false);
}

void
IPOPMulti::calculatePrefetch(const PrefetchInfo &pfi,
                             std::vector<AddrPriority> &addresses, bool late,
                             PrefetchSourceType source, bool miss_repeat)
{
    if (phaseOnMiss) {
        if (pfi.isCacheMiss()) {
            demandAccesses++;
            maybeAdvancePhase();
        }
    } else {
        demandAccesses++;
        maybeAdvancePhase();
    }

    for (unsigned int i = 0; i < prefetchers.size(); ++i) {
        auto &state = runtimeStates[i];
        if (!state.enabled || state.state == ControlState::Off) {
            continue;
        }
        auto *queued = dynamic_cast<Queued *>(prefetchers[i]);
        fatal_if(!queued, "%s: child %s is not a Queued prefetcher",
                 name(), prefetchers[i]->name());
        queued->setIpopEnabled(true);
        queued->setIpopAggressivenessLevel(
            std::max(1u, std::min(state.level, state.childMaxDegree)));
        queued->calculatePrefetch(pfi, addresses, late, source, miss_repeat);
    }
}

PacketPtr
IPOPMulti::getPacket()
{
    collectReadyPackets();

    if (pendingPackets.empty()) {
        return nullptr;
    }

    PendingPacket pending = pendingPackets.front();
    pendingPackets.pop_front();
    static_cast<void>(pending.prefetcherIdBits);
    prefetchStats.pfIssued++;
    issuedPrefetches++;
    return pending.pkt;
}

bool
IPOPMulti::hasPendingPacket()
{
    collectReadyPackets();
    return !pendingPackets.empty();
}

Tick
IPOPMulti::nextPrefetchReadyTime() const
{
    if (!pendingPackets.empty()) {
        return curTick();
    }

    return Multi::nextPrefetchReadyTime();
}

void
IPOPMulti::notifyIpopPrefetchFill(const IPOPEventInfo &info)
{
    TableEntry &entry = pfht[tableIndex(info.addr, pfhtEntries)];
    entry.valid = true;
    entry.tag = tableTag(info.addr);
    entry.isSecure = info.isSecure;
    entry.prefetcherIdBits = info.prefetcherIdBits;
    entry.accessDram = info.accessDram;

    const bool bus_contention =
        info.hasContentionAddr ? sameChannel(info.addr, info.contentionAddr)
                               : info.busContention;
    const bool bank_contention = info.hasContentionAddr
                                     ? sameBank(info.addr, info.contentionAddr)
                                     : info.bankContention;
    forEachPrefetcherBit(info.prefetcherIdBits, [&](unsigned int pf_index) {
        PrefetcherCounters &counters = phaseCounters[pf_index];
        if (info.delayedDemand) {
            counters.delay++;
            totalCounters[pf_index].delay++;
        }
        if (bus_contention) {
            counters.bus++;
            totalCounters[pf_index].bus++;
        }
        if (bank_contention) {
            counters.bank++;
            totalCounters[pf_index].bank++;
        }
    });
}

void
IPOPMulti::notifyIpopPrefetchEviction(const IPOPEventInfo &info)
{
    TableEntry &entry = poht[tableIndex(info.addr, pohtEntries)];
    entry.valid = true;
    entry.tag = tableTag(info.addr);
    entry.isSecure = info.isSecure;
    entry.prefetcherIdBits = info.prefetcherIdBits;
    entry.accessDram = info.accessDram;
}

void
IPOPMulti::notifyIpopDemandHit(const IPOPEventInfo &info)
{
    if (!phaseOnMiss) {
        demandAccesses++;
    }

    TableEntry *entry =
        lookupTable(pfht, pfhtEntries, info.addr, info.isSecure);
    if (!entry) {
        maybeAdvancePhase();
        return;
    }

    forEachPrefetcherBit(entry->prefetcherIdBits, [&](unsigned int pf_index) {
        PrefetcherCounters &counters = phaseCounters[pf_index];
        if (entry->accessDram) {
            counters.usefulDram++;
            totalCounters[pf_index].usefulDram++;
        } else {
            counters.usefulLlC++;
            totalCounters[pf_index].usefulLlC++;
        }
    });

    entry->valid = false;
    maybeAdvancePhase();
}

void
IPOPMulti::notifyIpopDemandMissComplete(const IPOPEventInfo &info)
{
    demandAccesses++;
    updateLatencySums(info.accessDram, info.latency);

    TableEntry *entry =
        lookupTable(poht, pohtEntries, info.addr, info.isSecure);
    if (entry) {
        forEachPrefetcherBit(
            entry->prefetcherIdBits, [&](unsigned int pf_index) {
                PrefetcherCounters &counters = phaseCounters[pf_index];
                if (info.accessDram) {
                    counters.pollutionDram++;
                    totalCounters[pf_index].pollutionDram++;
                } else {
                    counters.pollutionLlC++;
                    totalCounters[pf_index].pollutionLlC++;
                }
            });

        entry->valid = false;
    }

    maybeAdvancePhase();
}

} // namespace prefetch
} // namespace gem5
