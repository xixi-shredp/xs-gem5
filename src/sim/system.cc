/*
 * Copyright (c) 2011-2014,2017-2019 ARM Limited
 * All rights reserved
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
 * Copyright (c) 2003-2006 The Regents of The University of Michigan
 * Copyright (c) 2011 Regents of the University of California
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

#include "sim/system.hh"

#include <algorithm>
#include <cstring>
#include <functional>

#include "base/compiler.hh"
#include "base/cprintf.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "base/str.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#if !IS_NULL_ISA
#include "cpu/base.hh"
#endif
#include "cpu/thread_context.hh"
#include "debug/Loader.hh"
#include "debug/Quiesce.hh"
#include "debug/WorkItems.hh"
#include "mem/abstract_mem.hh"
#include "mem/mem_util.hh"
#include "mem/physical.hh"
#include "params/System.hh"
#include "sim/byteswap.hh"
#include "sim/debug.hh"
#include "sim/redirect_path.hh"
#include "sim/serialize_handlers.hh"

namespace gem5
{

std::vector<System *> System::systemList;

void
System::Threads::Thread::resume()
{
#   if !IS_NULL_ISA
    DPRINTFS(Quiesce, context->getCpuPtr(), "activating\n");
    context->activate();
#   endif
}

std::string
System::Threads::Thread::name() const
{
    assert(context);
    return csprintf("%s.threads[%d]", context->getSystemPtr()->name(),
            context->contextId());
}

void
System::Threads::Thread::quiesce() const
{
    context->suspend();
    context->getSystemPtr()->workload->recordQuiesce();
}

void
System::Threads::insert(ThreadContext *tc)
{
    ContextID id = size();
    tc->setContextId(id);

    auto &t = threads.emplace_back();
    t.context = tc;
    // Look up this thread again on resume, in case the threads vector has
    // been reallocated.
    t.resumeEvent = new EventFunctionWrapper(
            [this, id](){ thread(id).resume(); },
            tc->getSystemPtr()->name());
}

void
System::Threads::replace(ThreadContext *tc, ContextID id)
{
    auto &t = thread(id);
    panic_if(!t.context, "Can't replace a context which doesn't exist.");
#   if !IS_NULL_ISA
    if (t.resumeEvent->scheduled()) {
        Tick when = t.resumeEvent->when();
        t.context->getCpuPtr()->deschedule(t.resumeEvent);
        tc->getCpuPtr()->schedule(t.resumeEvent, when);
    }
#   endif
    t.context = tc;
}

ThreadContext *
System::Threads::findFree()
{
    for (auto &thread: threads) {
        if (thread.context->status() == ThreadContext::Halted)
            return thread.context;
    }
    return nullptr;
}

int
System::Threads::numRunning() const
{
    int count = 0;
    for (auto &thread: threads) {
        auto status = thread.context->status();
        if (status != ThreadContext::Halted &&
                status != ThreadContext::Halting) {
            count++;
        }
    }
    return count;
}

void
System::Threads::quiesce(ContextID id)
{
    auto &t = thread(id);
#   if !IS_NULL_ISA
    [[maybe_unused]] BaseCPU *cpu = t.context->getCpuPtr();
    DPRINTFS(Quiesce, cpu, "quiesce()\n");
#   endif
    t.quiesce();
}

void
System::Threads::quiesceTick(ContextID id, Tick when)
{
#   if !IS_NULL_ISA
    auto &t = thread(id);
    BaseCPU *cpu = t.context->getCpuPtr();

    DPRINTFS(Quiesce, cpu, "quiesceTick until %u\n", when);
    t.quiesce();

    cpu->reschedule(t.resumeEvent, when, true);
#   endif
}

int System::numSystemsRunning = 0;

System::IdealDCacheOracleStats::IdealDCacheOracleStats(System *system)
    : statistics::Group(system, "idealDCacheOracle"),
      ADD_STAT(linesInitialized, statistics::units::Count::get(),
               "Canonical cache lines initialized"),
      ADD_STAT(logicalCommits, statistics::units::Count::get(),
               "Logical writes committed to canonical cache lines"),
      ADD_STAT(backingSyncs, statistics::units::Count::get(),
               "Canonical cache lines synchronized to physical backing"),
      ADD_STAT(cacheCommits, statistics::units::Count::get(),
               "Logical commits originating from non-ideal caches"),
      ADD_STAT(idealCommits, statistics::units::Count::get(),
               "Logical commits originating from ideal L1D accesses"),
      ADD_STAT(memoryCommits, statistics::units::Count::get(),
               "Logical commits observed after memory writes"),
      ADD_STAT(functionalCommits, statistics::units::Count::get(),
               "Logical commits originating from functional writes"),
      ADD_STAT(rebases, statistics::units::Count::get(),
               "Tracked external lines rebased from canonical data"),
      ADD_STAT(stateCarrierRebases, statistics::units::Count::get(),
               "Cache writeback carriers rebased from canonical data"),
      ADD_STAT(reservationSets, statistics::units::Count::get(),
               "Ideal-DCache LL reservations set"),
      ADD_STAT(reservationChecks, statistics::units::Count::get(),
               "Ideal-DCache SC reservations checked"),
      ADD_STAT(reservationVersionFailures, statistics::units::Count::get(),
               "Ideal-DCache SC failures caused by a line version change"),
      ADD_STAT(reservationInvalidations, statistics::units::Count::get(),
               "Reservations broken by external coherence acquisitions"),
      ADD_STAT(reservationInvalidationFailures,
               statistics::units::Count::get(),
               "Ideal-DCache SC failures caused by coherence invalidation")
{
}

System::System(const Params &p)
    : SimObject(p), _systemPort("system_port", this),
      multiThread(p.multi_thread),
      init_param(p.init_param),
      physProxy(_systemPort, p.cache_line_size),
      workload(p.workload),
      numCPUs(p.num_cpus),
      enableDifftest(p.enable_difftest),
      enableMemDedup(p.enable_mem_dedup),
      physmem(name() + ".physmem", p.memories, p.mmap_using_noreserve,
              p.shared_backstore, p.enable_h_gcpt, p.restore_from_gcpt, p.gcpt_restorer_file,
              p.gcpt_file, p.map_to_raw_cpt, p.auto_unlink_shared_backstore, p.gcpt_restorer_size_limit,
              &dedupMemManager, p.enable_mem_dedup),
      ShadowRomRanges(p.shadow_rom_ranges.begin(),
                      p.shadow_rom_ranges.end()),
      memoryMode(p.mem_mode),
      _cacheLineSize(p.cache_line_size),
      idealDCacheOracleStats(this),
      numWorkIds(p.num_work_ids),
      thermalModel(p.thermal_model),
      _m5opRange(p.m5ops_base ?
                 RangeSize(p.m5ops_base, 0x10000) :
                 AddrRange(1, 0)), // Create an empty range if disabled
      redirectPaths(p.redirect_paths),
      xiangshanSystem(p.xiangshan_system)
{
    panic_if(!workload, "No workload set for system %s "
            "(could use StubWorkload?).", name());
    workload->setSystem(this);

    // add self to global system list
    systemList.push_back(this);

    // check if the cache line size is a value known to work
    if (_cacheLineSize != 16 && _cacheLineSize != 32 &&
        _cacheLineSize != 64 && _cacheLineSize != 128) {
        warn_once("Cache line size is neither 16, 32, 64 nor 128 bytes.\n");
    }

    // Get the generic system requestor IDs
    [[maybe_unused]] RequestorID tmp_id;
    tmp_id = getRequestorId(this, "writebacks");
    assert(tmp_id == Request::wbRequestorId);
    tmp_id = getRequestorId(this, "functional");
    assert(tmp_id == Request::funcRequestorId);
    tmp_id = getRequestorId(this, "interrupt");
    assert(tmp_id == Request::intRequestorId);

    // increment the number of running systems
    numSystemsRunning++;

    // Set back pointers to the system in all memories
    for (int x = 0; x < params().memories.size(); x++)
        params().memories[x]->system(this);
}

System::~System()
{
    for (uint32_t j = 0; j < numWorkIds; j++)
        delete workItemStats[j];
}

Port &
System::getPort(const std::string &if_name, PortID idx)
{
    // no need to distinguish at the moment (besides checking)
    return _systemPort;
}

void
System::setMemoryMode(enums::MemoryMode mode)
{
    assert(drainState() == DrainState::Drained);
    memoryMode = mode;
}

void
System::registerThreadContext(ThreadContext *tc)
{
    threads.insert(tc);

    workload->registerThreadContext(tc);

    for (auto *e: liveEvents)
        tc->schedule(e);
}

bool
System::schedule(PCEvent *event)
{
    bool all = true;
    liveEvents.push_back(event);
    for (auto *tc: threads)
        all = tc->schedule(event) && all;
    return all;
}

bool
System::remove(PCEvent *event)
{
    bool all = true;
    liveEvents.remove(event);
    for (auto *tc: threads)
        all = tc->remove(event) && all;
    return all;
}

void
System::replaceThreadContext(ThreadContext *tc, ContextID context_id)
{
    auto *otc = threads[context_id];
    threads.replace(tc, context_id);

    workload->replaceThreadContext(tc);

    for (auto *e: liveEvents) {
        otc->remove(e);
        tc->schedule(e);
    }
}

Addr
System::memSize() const
{
    return physmem.totalSize();
}

bool
System::isMemAddr(Addr addr) const
{
    return physmem.isMemAddr(addr);
}

bool
System::idealDCacheOracleOwns(PacketPtr pkt) const
{
    return idealDCacheOracleOwns(pkt, nullptr);
}

bool
System::idealDCacheOracleOwns(
    Addr start, Addr size, RequestorID requestor_id) const
{
    if (GEM5_LIKELY(!idealDCacheEnabled) || size == 0 ||
        size > MaxAddr - start) {
        return false;
    }

    const AddrRange range = RangeSize(start, size);
    const auto device_memories = deviceMemMap.find(requestor_id);
    if (device_memories != deviceMemMap.end()) {
        for (const auto *memory : device_memories->second) {
            if (range.isSubset(memory->getAddrRange())) {
                return false;
            }
        }
    }

    return physmem.isMemRange(range);
}

bool
System::idealDCacheOracleOwns(
    PacketPtr pkt, const memory::AbstractMemory *owner) const
{
    if (!pkt || !pkt->req || pkt->getSize() == 0) {
        return false;
    }

    const Addr start = pkt->getAddr();
    const Addr size = static_cast<Addr>(pkt->getSize());
    if (!idealDCacheOracleOwns(start, size, pkt->requestorId())) {
        return false;
    }

    return !owner || physmem.isMemRange(RangeSize(start, size), owner);
}

size_t
System::IdealDCacheLineKeyHash::operator()(
    const IdealDCacheLineKey &key) const
{
    const size_t addr_hash = std::hash<Addr>{}(key.addr);
    return (addr_hash << 1) ^ static_cast<size_t>(key.secure);
}

Addr
System::idealDCacheLineAddr(Addr addr) const
{
    return addr & ~static_cast<Addr>(_cacheLineSize - 1);
}

void
System::registerIdealDCache()
{
    fatal_if(compressedCacheRegistered,
             "The ideal-DCache oracle is incompatible with compressed caches "
             "in system %s", name());
    if (!idealDCacheEnabled) {
        physmem.forbidWritableRawBacking();
        idealDCacheEnabled = true;
    }
}

void
System::registerCompressedCache()
{
    fatal_if(idealDCacheEnabled,
             "Compressed caches are incompatible with the ideal-DCache "
             "oracle in system %s", name());
    compressedCacheRegistered = true;
}

bool
System::readIdealDCacheBackingLine(Addr addr, bool secure, uint8_t *data)
{
    if (!data) {
        return false;
    }

    const Addr line_addr = idealDCacheLineAddr(addr);
    const Addr line_size = static_cast<Addr>(_cacheLineSize);
    if (line_size > MaxAddr - line_addr ||
        !physmem.isMemRange(RangeSize(line_addr, line_size))) {
        return false;
    }

    std::memset(data, 0, _cacheLineSize);
    Request::Flags flags;
    flags.set(Request::IDEAL_DCACHE_INTERNAL);
    if (secure) {
        flags.set(Request::SECURE);
    }
    RequestPtr req = std::make_shared<Request>(
        line_addr, _cacheLineSize, flags, Request::funcRequestorId);
    Packet pkt(req, MemCmd::ReadReq);
    pkt.dataStatic(data);
    physmem.functionalAccess(&pkt);
    return pkt.isResponse() && !pkt.isError() && pkt.hasData();
}

bool
System::syncIdealDCacheBackingLine(
    Addr addr, bool secure, const uint8_t *data)
{
    if (!data) {
        return false;
    }

    const Addr line_addr = idealDCacheLineAddr(addr);
    const Addr line_size = static_cast<Addr>(_cacheLineSize);
    if (line_size > MaxAddr - line_addr ||
        !physmem.isMemRange(RangeSize(line_addr, line_size))) {
        return false;
    }

    Request::Flags flags;
    flags.set(Request::IDEAL_DCACHE_INTERNAL);
    if (secure) {
        flags.set(Request::SECURE);
    }
    RequestPtr req = std::make_shared<Request>(
        line_addr, _cacheLineSize, flags, Request::funcRequestorId);
    Packet pkt(req, MemCmd::WriteReq);
    pkt.dataStaticConst(data);
    physmem.functionalAccess(&pkt);
    if (!pkt.isResponse() || pkt.isError()) {
        return false;
    }

    idealDCacheOracleStats.backingSyncs++;
    return true;
}

bool
System::readIdealDCacheLine(Addr addr, bool secure, uint8_t *data)
{
    if (GEM5_UNLIKELY(!idealDCacheEnabled) || !data) {
        return false;
    }

    const IdealDCacheLineKey key{idealDCacheLineAddr(addr), secure};
    auto line = idealDCacheLines.find(key);
    if (line == idealDCacheLines.end()) {
        IdealDCacheLine new_line;
        new_line.data.resize(_cacheLineSize);
        if (!readIdealDCacheBackingLine(key.addr, secure,
                                        new_line.data.data())) {
            return false;
        }
        line = idealDCacheLines.emplace(key, std::move(new_line)).first;
        idealDCacheOracleStats.linesInitialized++;
    }

    std::memcpy(data, line->second.data.data(), _cacheLineSize);
    return true;
}

bool
System::rebaseIdealDCacheLine(Addr addr, bool secure, uint8_t *data)
{
    if (GEM5_UNLIKELY(!idealDCacheEnabled) || !data) {
        return false;
    }

    const IdealDCacheLineKey key{idealDCacheLineAddr(addr), secure};
    const auto line = idealDCacheLines.find(key);
    if (line == idealDCacheLines.end()) {
        return false;
    }

    std::memcpy(data, line->second.data.data(), _cacheLineSize);
    idealDCacheOracleStats.rebases++;
    return true;
}

void
System::commitIdealDCacheLine(
    Addr addr, bool secure, const uint8_t *data,
    IdealDCacheCommitSource source, bool sync_backing)
{
    if (GEM5_UNLIKELY(!idealDCacheEnabled)) {
        return;
    }
    fatal_if(!data, "Cannot commit a null ideal-DCache oracle line");

    const IdealDCacheLineKey key{idealDCacheLineAddr(addr), secure};
    auto [line, inserted] = idealDCacheLines.try_emplace(key);
    if (inserted) {
        line->second.data.resize(_cacheLineSize);
        idealDCacheOracleStats.linesInitialized++;
    }
    std::memcpy(line->second.data.data(), data, _cacheLineSize);
    ++line->second.version;

    idealDCacheOracleStats.logicalCommits++;
    switch (source) {
      case IdealDCacheCommitSource::Cache:
        idealDCacheOracleStats.cacheCommits++;
        break;
      case IdealDCacheCommitSource::Ideal:
        idealDCacheOracleStats.idealCommits++;
        break;
      case IdealDCacheCommitSource::Memory:
        idealDCacheOracleStats.memoryCommits++;
        break;
      case IdealDCacheCommitSource::Functional:
        idealDCacheOracleStats.functionalCommits++;
        break;
    }

    fatal_if(sync_backing &&
             !syncIdealDCacheBackingLine(key.addr, secure,
                                         line->second.data.data()),
             "Unable to synchronize ideal-DCache oracle line %#x", key.addr);
}

void
System::commitIdealDCacheCacheLine(
    Addr addr, bool secure, const uint8_t *data, bool from_ideal)
{
    commitIdealDCacheLine(
        addr, secure, data,
        from_ideal ? IdealDCacheCommitSource::Ideal :
                     IdealDCacheCommitSource::Cache,
        true);
}

void
System::normalizeIdealDCachePacket(PacketPtr pkt)
{
    if (!idealDCacheOracleOwns(pkt) ||
        pkt->req->isIdealDCacheInternal()) {
        return;
    }

    const bool state_carrier =
        pkt->cmd == MemCmd::WritebackDirty ||
        pkt->cmd == MemCmd::WritebackClean ||
        pkt->cmd == MemCmd::WriteClean;
    if (!state_carrier) {
        return;
    }

    fatal_if(!pkt->hasData() || pkt->getSize() != _cacheLineSize ||
             pkt->getAddr() != idealDCacheLineAddr(pkt->getAddr()),
             "Malformed ideal-DCache state carrier: %s", pkt->print());
    if (rebaseIdealDCacheLine(pkt->getAddr(), pkt->isSecure(),
                              pkt->getPtr<uint8_t>())) {
        idealDCacheOracleStats.stateCarrierRebases++;
    }
}

void
System::trackIdealDCacheLoadLocked(
    PacketPtr pkt, const uint8_t *line_data)
{
    if (GEM5_UNLIKELY(!idealDCacheEnabled)) {
        return;
    }
    fatal_if(!pkt || !pkt->req || !pkt->isLLSC() || !pkt->isRead() ||
             !pkt->req->hasContextId(),
             "Malformed ideal-DCache load-locked packet");

    const IdealDCacheLineKey key{
        idealDCacheLineAddr(pkt->getAddr()), pkt->isSecure()};
    auto line = idealDCacheLines.find(key);
    if (line == idealDCacheLines.end()) {
        if (line_data) {
            IdealDCacheLine new_line;
            new_line.data.assign(line_data, line_data + _cacheLineSize);
            line = idealDCacheLines.emplace(key, std::move(new_line)).first;
            idealDCacheOracleStats.linesInitialized++;
        } else {
            std::vector<uint8_t> data(_cacheLineSize);
            fatal_if(!readIdealDCacheLine(key.addr, key.secure, data.data()),
                     "Unable to initialize ideal-DCache LL line %#x",
                     key.addr);
            line = idealDCacheLines.find(key);
            assert(line != idealDCacheLines.end());
        }
    }

    const Addr low_addr = pkt->getAddr();
    idealDCacheReservations[pkt->req->contextId()] = {
        key, low_addr, low_addr + pkt->getSize() - 1,
        line->second.version
    };
    idealDCacheOracleStats.reservationSets++;
}

void
System::invalidateIdealDCacheReservations(PacketPtr pkt)
{
    if (!idealDCacheOracleOwns(pkt) ||
        pkt->req->isIdealDCacheInternal() || pkt->getSize() == 0 ||
        !(pkt->needsWritable() || pkt->isInvalidate() ||
          pkt->req->isCacheMaintenance())) {
        return;
    }

    const bool state_carrier =
        pkt->cmd == MemCmd::WritebackDirty ||
        pkt->cmd == MemCmd::WritebackClean ||
        pkt->cmd == MemCmd::WriteClean;
    if (state_carrier) {
        return;
    }

    const Addr pkt_end = pkt->getAddr() + pkt->getSize() - 1;
    fatal_if(pkt_end < pkt->getAddr(),
             "Ideal-DCache reservation invalidation overflow: %s",
             pkt->print());
    const Addr first_line = idealDCacheLineAddr(pkt->getAddr());
    const Addr last_line = idealDCacheLineAddr(pkt_end);
    const bool preserve_requester =
        pkt->isLLSC() && pkt->req->hasContextId();

    for (auto &[context_id, reservation] : idealDCacheReservations) {
        if (reservation.key.secure != pkt->isSecure() ||
            reservation.key.addr < first_line ||
            reservation.key.addr > last_line ||
            (preserve_requester &&
             context_id == pkt->req->contextId()) ||
            reservation.invalidated) {
            continue;
        }
        reservation.invalidated = true;
        idealDCacheOracleStats.reservationInvalidations++;
    }
}

bool
System::checkIdealDCacheStoreConditional(PacketPtr pkt)
{
    if (GEM5_UNLIKELY(!idealDCacheEnabled)) {
        return true;
    }
    fatal_if(!pkt || !pkt->req || !pkt->isLLSC() || !pkt->isWrite() ||
             !pkt->req->hasContextId(),
             "Malformed ideal-DCache store-conditional packet");

    idealDCacheOracleStats.reservationChecks++;
    const ContextID context_id = pkt->req->contextId();
    const auto reservation = idealDCacheReservations.find(context_id);
    const IdealDCacheLineKey key{
        idealDCacheLineAddr(pkt->getAddr()), pkt->isSecure()};
    const Addr req_low = pkt->getAddr();
    const Addr req_high = req_low + pkt->getSize() - 1;
    const bool address_match =
        reservation != idealDCacheReservations.end() &&
        reservation->second.key == key &&
        req_low >= reservation->second.lowAddr &&
        req_high <= reservation->second.highAddr;

    bool version_match = false;
    bool invalidation_match = false;
    if (address_match) {
        invalidation_match = !reservation->second.invalidated;
        if (!invalidation_match) {
            idealDCacheOracleStats.reservationInvalidationFailures++;
        }
        const auto line = idealDCacheLines.find(key);
        version_match = line != idealDCacheLines.end() &&
            line->second.version == reservation->second.version;
        if (!version_match) {
            idealDCacheOracleStats.reservationVersionFailures++;
        }
    }

    if (reservation != idealDCacheReservations.end()) {
        idealDCacheReservations.erase(reservation);
    }
    const bool success =
        address_match && version_match && invalidation_match;
    pkt->req->setExtraData(success ? 1 : 0);
    return success;
}

void
System::observeIdealDCacheMemoryWrite(PacketPtr pkt)
{
    if (!idealDCacheOracleOwns(pkt) ||
        pkt->req->isIdealDCacheInternal() || !pkt->isWrite() ||
        pkt->getSize() == 0) {
        return;
    }

    const bool state_carrier =
        pkt->cmd == MemCmd::WritebackDirty ||
        pkt->cmd == MemCmd::WritebackClean ||
        pkt->cmd == MemCmd::WriteClean;
    if (state_carrier) {
        return;
    }

    const Addr pkt_end = pkt->getAddr() + pkt->getSize() - 1;
    fatal_if(pkt_end < pkt->getAddr(),
             "Ideal-DCache memory write address overflow: %s",
             pkt->print());
    const Addr first_line = idealDCacheLineAddr(pkt->getAddr());
    const Addr last_line = idealDCacheLineAddr(pkt_end);
    std::vector<uint8_t> data(_cacheLineSize);
    for (Addr line = first_line;; line += _cacheLineSize) {
        fatal_if(!readIdealDCacheBackingLine(
                     line, pkt->isSecure(), data.data()),
                 "Unable to observe memory write to ideal-DCache line %#x",
                 line);
        commitIdealDCacheLine(line, pkt->isSecure(), data.data(),
                              IdealDCacheCommitSource::Memory, false);
        if (line == last_line) {
            break;
        }
    }
}

void
System::commitIdealDCacheFunctionalWrite(PacketPtr pkt)
{
    if (!idealDCacheOracleOwns(pkt) ||
        pkt->req->isIdealDCacheInternal() ||
        pkt->req->isIdealDCacheFunctionalObserved() || !pkt->isWrite() ||
        pkt->isRead()) {
        return;
    }

    const bool state_carrier =
        pkt->cmd == MemCmd::WritebackDirty ||
        pkt->cmd == MemCmd::WritebackClean ||
        pkt->cmd == MemCmd::WriteClean;
    if (state_carrier) {
        return;
    }

    fatal_if(!pkt->hasData(),
             "Ideal-DCache functional write has no data: %s", pkt->print());
    const Addr pkt_end = pkt->getAddr() + pkt->getSize() - 1;
    assert(pkt_end >= pkt->getAddr());

    const auto &byte_enable = pkt->req->getByteEnable();
    fatal_if(!byte_enable.empty() &&
             byte_enable.size() != pkt->getSize(),
             "Ideal-DCache functional write has malformed byte enable: %s",
             pkt->print());

    pkt->req->setFlags(Request::IDEAL_DCACHE_FUNCTIONAL_OBSERVED);
    const Addr first_line = idealDCacheLineAddr(pkt->getAddr());
    const Addr last_line = idealDCacheLineAddr(pkt_end);
    const uint8_t *pkt_data = pkt->getConstPtr<uint8_t>();
    std::vector<uint8_t> line_data(_cacheLineSize);
    for (Addr line = first_line;; line += _cacheLineSize) {
        fatal_if(!readIdealDCacheLine(
                     line, pkt->isSecure(), line_data.data()),
                 "Unable to initialize functional ideal-DCache line %#x",
                 line);
        const Addr overlap_start = std::max(line, pkt->getAddr());
        const Addr overlap_end = std::min(
            line + static_cast<Addr>(_cacheLineSize) - 1, pkt_end);
        for (Addr addr = overlap_start;; ++addr) {
            const size_t pkt_offset = addr - pkt->getAddr();
            if (byte_enable.empty() || byte_enable[pkt_offset]) {
                line_data[addr - line] = pkt_data[pkt_offset];
            }
            if (addr == overlap_end) {
                break;
            }
        }
        commitIdealDCacheLine(line, pkt->isSecure(), line_data.data(),
                              IdealDCacheCommitSource::Functional, true);
        if (line == last_line) {
            break;
        }
    }
}

void
System::addDeviceMemory(RequestorID requestor_id,
    memory::AbstractMemory *deviceMemory)
{
    deviceMemMap[requestor_id].push_back(deviceMemory);
}

bool
System::isDeviceMemAddr(const PacketPtr& pkt) const
{
    if (!deviceMemMap.count(pkt->requestorId())) {
        return false;
    }

    return (getDeviceMemory(pkt) != nullptr);
}

memory::AbstractMemory *
System::getDeviceMemory(const PacketPtr& pkt) const
{
    const RequestorID& rid = pkt->requestorId();

    panic_if(!deviceMemMap.count(rid),
             "No device memory found for Requestor %d\n", rid);

    for (auto& mem : deviceMemMap.at(rid)) {
        if (pkt->getAddrRange().isSubset(mem->getAddrRange())) {
            return mem;
        }
    }

    return nullptr;
}

void
System::serialize(CheckpointOut &cp) const
{
    for (auto &t: threads.threads) {
        Tick when = 0;
        if (t.resumeEvent && t.resumeEvent->scheduled())
            when = t.resumeEvent->when();
        ContextID id = t.context->contextId();
        paramOut(cp, csprintf("quiesceEndTick_%d", id), when);
    }

    // also serialize the memories in the system
    physmem.serializeSection(cp, "physmem");
}


void
System::unserialize(CheckpointIn &cp)
{
    for (auto &t: threads.threads) {
        Tick when = 0;
        ContextID id = t.context->contextId();
        if (!optParamIn(cp, csprintf("quiesceEndTick_%d", id), when) ||
                !when || !t.resumeEvent) {
            continue;
        }
#       if !IS_NULL_ISA
        t.context->getCpuPtr()->schedule(t.resumeEvent, when);
#       endif
    }

    // also unserialize the memories in the system
    physmem.unserializeSection(cp, "physmem");
}

void
System::regStats()
{
    SimObject::regStats();

    for (uint32_t j = 0; j < numWorkIds ; j++) {
        workItemStats[j] = new statistics::Histogram(this);
        std::stringstream namestr;
        ccprintf(namestr, "work_item_type%d", j);
        workItemStats[j]->init(20)
                         .name(namestr.str())
                         .desc("Run time stat for" + namestr.str())
                         .prereq(*workItemStats[j]);
    }
}

void
System::workItemEnd(uint32_t tid, uint32_t workid)
{
    std::pair<uint32_t,uint32_t> p(tid, workid);
    if (!lastWorkItemStarted.count(p))
        return;

    Tick samp = curTick() - lastWorkItemStarted[p];
    DPRINTF(WorkItems, "Work item end: %d\t%d\t%lld\n", tid, workid, samp);

    if (workid >= numWorkIds)
        fatal("Got workid greater than specified in system configuration\n");

    workItemStats[workid]->sample(samp);
    lastWorkItemStarted.erase(p);
}

bool
System::trapToGdb(int signal, ContextID ctx_id) const
{
    return workload->trapToGdb(signal, ctx_id);
}

void
System::printSystems()
{
    std::ios::fmtflags flags(std::cerr.flags());

    std::vector<System *>::iterator i = systemList.begin();
    std::vector<System *>::iterator end = systemList.end();
    for (; i != end; ++i) {
        System *sys = *i;
        std::cerr << "System " << sys->name() << ": " << std::hex << sys
                  << std::endl;
    }

    std::cerr.flags(flags);
}

void
printSystems()
{
    System::printSystems();
}

std::string
System::stripSystemName(const std::string& requestor_name) const
{
    if (startswith(requestor_name, name())) {
        return requestor_name.substr(name().size() + 1);
    } else {
        return requestor_name;
    }
}

RequestorID
System::lookupRequestorId(const SimObject* obj) const
{
    RequestorID id = Request::invldRequestorId;

    // number of occurrences of the SimObject pointer
    // in the requestor list.
    auto obj_number = 0;

    for (int i = 0; i < requestors.size(); i++) {
        if (requestors[i].obj == obj) {
            id = i;
            obj_number++;
        }
    }

    fatal_if(obj_number > 1,
        "Cannot lookup RequestorID by SimObject pointer: "
        "More than one requestor is sharing the same SimObject\n");

    return id;
}

RequestorID
System::lookupRequestorId(const std::string& requestor_name) const
{
    std::string name = stripSystemName(requestor_name);

    for (int i = 0; i < requestors.size(); i++) {
        if (requestors[i].req_name == name) {
            return i;
        }
    }

    return Request::invldRequestorId;
}

RequestorID
System::getGlobalRequestorId(const std::string& requestor_name)
{
    return _getRequestorId(nullptr, requestor_name);
}

RequestorID
System::getRequestorId(const SimObject* requestor, std::string subrequestor)
{
    auto requestor_name = leafRequestorName(requestor, subrequestor);
    return _getRequestorId(requestor, requestor_name);
}

RequestorID
System::_getRequestorId(const SimObject* requestor,
                     const std::string& requestor_name)
{
    std::string name = stripSystemName(requestor_name);

    // CPUs in switch_cpus ask for ids again after switching
    for (int i = 0; i < requestors.size(); i++) {
        if (requestors[i].req_name == name) {
            return i;
        }
    }

    // Verify that the statistics haven't been enabled yet
    // Otherwise objects will have sized their stat buckets and
    // they will be too small

    if (statistics::enabled()) {
        fatal("Can't request a requestorId after regStats(). "
                "You must do so in init().\n");
    }

    // Generate a new RequestorID incrementally
    RequestorID requestor_id = requestors.size();

    // Append the new Requestor metadata to the group of system Requestors.
    requestors.emplace_back(requestor, name, requestor_id);

    return requestors.back().id;
}

std::string
System::leafRequestorName(const SimObject* requestor,
                       const std::string& subrequestor)
{
    if (subrequestor.empty()) {
        return requestor->name();
    } else {
        // Get the full requestor name by appending the subrequestor name to
        // the root SimObject requestor name
        return requestor->name() + "." + subrequestor;
    }
}

std::string
System::getRequestorName(RequestorID requestor_id)
{
    if (requestor_id >= requestors.size())
        fatal("Invalid requestor_id passed to getRequestorName()\n");

    const auto& requestor_info = requestors[requestor_id];
    return requestor_info.req_name;
}

void System::initState()
{
    // it does nothing
    SimObject::initState();

    if (physmem.tryRestoreFromXSCpt()) {
        inform("Restored from Xiangshan RISC-V Checkpoint\n");
    }

    // have to initiate golden memory after checkpoint restored
    if (multiContextDifftest()) {
        warn("Creating golden memory for multi-context difftest\n");
        assert(enableMemDedup);
        goldenMem = dedupMemManager.createCopyOnWriteBranch();
        goldenMemManager.initGoldenMem(physmem.getStartaddr(), memSize(), goldenMem);
    }

}

} // namespace gem5
