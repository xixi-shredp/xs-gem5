/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/o3/cpu.hh"

#include <cassert>
#include <cstring>
#include <limits>

#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pmp.hh"
#include "base/output.hh"
#include "config/the_isa.hh"
#include "cpu/activity.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/checker/thread_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/thread_context.hh"
#include "cpu/o3/trace/TraceReader.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "cpu/reg_class.hh"
#include "cpu/simple_thread.hh"
#include "cpu/pred/btb/hint_bundle.hh"
#include "cpu/thread_context.hh"
#include "debug/Activity.hh"
#include "debug/Commit.hh"
#include "debug/Drain.hh"
#include "debug/O3CPU.hh"
#include "debug/Quiesce.hh"
#include "debug/ValueCommit.hh"
#include "enums/MemoryMode.hh"
#include "sim/async.hh"
#include "sim/cur_tick.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/sim_exit.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"

namespace gem5
{

struct BaseCPUParams;

namespace o3
{

CPU::CPU(const BaseO3CPUParams &params)
    : BaseCPU(params),
      mmu(params.mmu),
      hintDmaPort(*this),
      hintDmaRequestorId(params.system->getRequestorId(this, "hint_dma")),
      tickEvent([this]{ tick(); }, "O3CPU tick",
                false, Event::CPU_Tick_Pri),
      threadExitEvent([this]{ exitThreads(); }, "O3CPU exit threads",
                false, Event::CPU_Exit_Pri),
#ifndef NDEBUG
      instcount(0),
#endif
      removeInstsThisCycle(false),
      fetch(this, params),
      decode(this, params),
      rename(this, params),
      iew(this, params),
      commit(this, fetch.getBp(), params),

      regFile(params.numPhysIntRegs,
              params.numPhysFloatRegs,
              params.numPhysVecRegs,
              params.numPhysVecPredRegs,
              params.numPhysCCRegs,
              params.numPhysRMiscRegs,
              params.isa[0]->regClasses()),

      freeList(name() + ".freelist", &regFile, params),

      rob(this, params),

      scoreboard(name() + ".scoreboard", regFile.totalNumPhysRegs()),

      isa(numThreads, NULL),

      timeBuffer(params.backComSize, params.forwardComSize),
      fetchTimebuffer(params.backComSize, params.forwardComSize),
      decodeTimebuffer(params.backComSize, params.forwardComSize),
      renameTimebuffer(params.backComSize, params.forwardComSize),
      iewTimebuffer(params.backComSize, params.forwardComSize),
      activityRec(name(), NumStages,
                  params.backComSize + params.forwardComSize,
                  params.activity),
      globalSeqNum(1),
      system(params.system),
      lastRunningCycle(curCycle()),
      archDBer(params.arch_db),
      perfCCT(new PerfCCT(params.arch_db && params.arch_db->dumpLifetime, params.arch_db)),
      ipc_r("ipc", "", 1000, archDBer),
      cpi_r("cpi", "", 1000, archDBer),
      issueWidth(params.decodeWidth),
      enableMoveElimination(params.enableMoveElimination),
      enableConstantFolding(params.enableConstantFolding),
      enableMovImmElimination(params.enableMovImmElimination),
      cpuStats(this),
      valuePred(params.valuePred)
{
    registerExitCallback([this] {
        if (switchedOut())
            return;
        const auto &a = hintDmaAudit;
        const auto c = hintDmaCounts();
        HintController::BufferStats buffers;
        bool controller_present = false;
        if (auto *bpu = dynamic_cast<
                branch_prediction::btb_pred::DecoupledBPUWithBTB *>(fetch.getBp());
                bpu && bpu->hasHintBundle()) {
            controller_present = true;
            buffers = bpu->getHintBundleController().bufferStats();
        }
        const std::string filename = cpuId() == 0 ? "bundle-controller-buffers.json" :
            name() + ".bundle-controller-buffers.json";
        auto output = simout.create(filename, false, true);
        *output->stream() << "{\"schema\":\"hint-bundle-controller-buffers/v2\""
            << ",\"controller_present\":" << (controller_present ? "true" : "false")
            << ",\"measure_only\":false"
            << ",\"boundary_owner_peak\":" << buffers.boundaryOwnerPeak
            << ",\"read_buffer_peak_bytes\":" << buffers.readBufferPeakBytes
            << ",\"write_buffer_peak_bytes\":" << buffers.writeBufferPeakBytes
            << ",\"shared_slot_peak\":" << buffers.sharedSlotPeak
            << ",\"read_slot_peak\":" << buffers.readSlotPeak
            << ",\"write_slot_peak\":" << buffers.writeSlotPeak
            << ",\"shared_payload_peak_bytes\":" << buffers.sharedPayloadPeakBytes
            << ",\"completion_descriptor_peak\":" << buffers.completionDescriptorPeak
            << ",\"max_row_bytes_per_tick\":" << buffers.maxRowBytesPerTick
            << ",\"max_pack_bytes_per_tick\":" << buffers.maxPackBytesPerTick
            << ",\"max_write_retire_slots_per_tick\":" << buffers.maxWriteRetireSlotsPerTick
            << ",\"max_write_retire_records_per_tick\":" << buffers.maxWriteRetireRecordsPerTick
            << ",\"max_submit_per_tick\":" << buffers.maxSubmitPerTick << "}\n";
        simout.close(output);
        if (!a.acquired)
            return;
        std::cout << "HINT_BUNDLE_DMA_AUDIT {\"schema\":\"hint-bundle-dma-audit/v3\""
            << ",\"transport\":\"independent-membus-master\""
            << ",\"direct_membus\":true,\"max_request_bytes\":64"
            << ",\"configured_max_outstanding\":64,\"shared_slots\":64"
            << ",\"shared_payload_bytes\":4096,\"max_send_attempts_per_cycle\":1"
            << ",\"core_fixed_priority\":false,\"payload_static_alias\":true"
            << ",\"port\":\"" << hintDmaPort.name() << "\""
            << ",\"requestor_id\":" << hintDmaRequestorId
            << ",\"lease_acquired\":" << a.acquired << ",\"lease_released\":" << a.released
            << ",\"acquire_refused\":" << a.acquireRefused
            << ",\"canceled_reserved\":" << a.canceledReserved
            << ",\"released_reads\":" << a.releasedReads
            << ",\"released_writes\":" << a.releasedWrites
            << ",\"submitted\":" << a.submitted
            << ",\"send_attempts\":" << a.attempts
            << ",\"accepted\":" << a.accepted
            << ",\"responses\":" << a.responses
            << ",\"reads\":" << a.reads << ",\"writes\":" << a.writes
            << ",\"response_errors\":" << a.responseErrors
            << ",\"access_faults\":" << a.accessFaults
            << ",\"retry_notifications\":" << a.retries
            << ",\"retry_quota_deferred\":" << a.retryDeferred
            << ",\"send_refused\":" << a.refused
            << ",\"out_of_order_responses\":" << a.outOfOrderResponses
            << ",\"out_of_order_reads\":" << a.outOfOrderReads
            << ",\"out_of_order_writes\":" << a.outOfOrderWrites
            << ",\"accepted_bytes\":" << a.bytes
            << ",\"read_bytes\":" << a.readBytes << ",\"write_bytes\":" << a.writeBytes
            << ",\"reads64\":" << a.reads64 << ",\"writes64\":" << a.writes64
            << ",\"partial_reads\":" << a.partialReads << ",\"partial_writes\":" << a.partialWrites
            << ",\"max_beat_bytes\":" << a.maxBeat
            << ",\"max_attempts_per_cycle\":" << a.maxAttemptsPerCycle
            << ",\"line_crossings\":" << a.lineCrossings
            << ",\"leased\":" << c.leased << ",\"reserved\":" << c.reserved
            << ",\"queued\":" << c.queued << ",\"sent\":" << c.sent
            << ",\"response_ready\":" << c.ready
            << ",\"read_leased\":" << c.readLeased << ",\"write_leased\":" << c.writeLeased
            << ",\"read_sent\":" << c.readSent << ",\"write_sent\":" << c.writeSent
            << ",\"read_ready\":" << c.readReady << ",\"write_ready\":" << c.writeReady
            << ",\"resident_read_bytes\":" << c.readBytes
            << ",\"resident_write_bytes\":" << c.writeBytes
            << ",\"leased_payload_bytes\":" << c.leased * 64
            << ",\"peak_leased\":" << a.maxLeased << ",\"peak_reserved\":" << a.maxReserved
            << ",\"peak_queued\":" << a.maxQueued << ",\"max_outstanding\":" << a.maxOutstanding
            << ",\"peak_response_ready\":" << a.maxReady
            << ",\"peak_read_leased\":" << a.maxReadLeased
            << ",\"peak_write_leased\":" << a.maxWriteLeased
            << ",\"peak_read_sent\":" << a.maxReadSent
            << ",\"peak_write_sent\":" << a.maxWriteSent
            << ",\"peak_read_ready\":" << a.maxReadReady
            << ",\"peak_write_ready\":" << a.maxWriteReady
            << ",\"peak_mixed_sent\":" << a.maxMixedSent
            << ",\"peak_ready_while_sent\":" << a.maxReadyWhileSent
            << ",\"peak_resident_read_bytes\":" << a.maxReadBytes
            << ",\"peak_resident_write_bytes\":" << a.maxWriteBytes
            << ",\"peak_leased_payload_bytes\":" << a.maxLeased * 64
            << ",\"blocked_send_owner\":" << unsigned(hintDmaBlocked.has_value())
            << ",\"retry_grant_pending\":" << unsigned(hintDmaRetryGrant)
            << ",\"pending\":" << c.leased << "}\n";
    });
    if (!params.switched_out) {
        _status = Running;
    } else {
        _status = SwitchedOut;
    }

    if (params.checker) {
        BaseCPU *temp_checker = params.checker;
        checker = dynamic_cast<Checker<DynInstPtr> *>(temp_checker);
        checker->setIcachePort(&fetch.getInstPort());
        checker->setSystem(params.system);
    } else {
        checker = NULL;
    }

    if (!FullSystem) {
        thread.resize(numThreads);
        tids.resize(numThreads);
    }

    // The stages also need their CPU pointer setup.  However this
    // must be done at the upper level CPU because they have pointers
    // to the upper level CPU, and not this CPU.

    // Set up Pointers to the activeThreads list for each stage
    fetch.setActiveThreads(&activeThreads);
    decode.setActiveThreads(&activeThreads);
    rename.setActiveThreads(&activeThreads);
    iew.setActiveThreads(&activeThreads);
    commit.setActiveThreads(&activeThreads);
    freeList.setActiveThreads(&activeThreads);

    // Give each of the stages the time buffer they will use.
    fetch.setTimeBuffer(&timeBuffer);
    decode.setTimeBuffer(&timeBuffer);
    rename.setTimeBuffer(&timeBuffer);
    iew.setTimeBuffer(&timeBuffer);
    commit.setTimeBuffer(&timeBuffer);

    // Also setup each of the stages' queues.
    fetch.setFetchQueue(&fetchTimebuffer);
    decode.setFetchQueue(&fetchTimebuffer);
    commit.setFetchQueue(&fetchTimebuffer);
    decode.setDecodeQueue(&decodeTimebuffer);
    rename.setDecodeQueue(&decodeTimebuffer);
    rename.setRenameQueue(&renameTimebuffer);
    iew.setRenameQueue(&renameTimebuffer);
    iew.setIEWQueue(&iewTimebuffer);
    commit.setIEWQueue(&iewTimebuffer);
    commit.setRenameQueue(&renameTimebuffer);

    decode.setFetchStage(&fetch);
    commit.setIEWStage(&iew);
    commit.setDecodeStage(&decode);
    rename.setIEWStage(&iew);
    rename.setCommitStage(&commit);

    fetch.setStallSignals(&stallSignals);
    decode.setStallSignals(&stallSignals);
    rename.setStallSignals(&stallSignals);
    iew.setStallSignals(&stallSignals);
    commit.setStallSignals(&stallSignals);

    ThreadID active_threads;
    if (FullSystem) {
        // FS-SMT still uses one shared workload/system image, but the O3 core
        // must provision per-thread architectural state for every hardware
        // thread context exposed by the CPU.
        active_threads = numThreads;
    } else {
        active_threads = params.workload.size();

        if (active_threads > MaxThreads) {
            panic("Workload Size too large. Increase the 'MaxThreads' "
                  "constant in cpu/o3/limits.hh or edit your workload size.");
        }
    }

    // Make Sure That this a Valid Architeture
    assert(numThreads);
    const auto &regClasses = params.isa[0]->regClasses();

    assert(params.numPhysIntRegs >=
            numThreads * regClasses.at(IntRegClass).numRegs());
    assert(params.numPhysFloatRegs >=
            numThreads * regClasses.at(FloatRegClass).numRegs());
    assert(params.numPhysVecRegs >=
            numThreads * regClasses.at(VecRegClass).numRegs());
    assert(params.numPhysVecPredRegs >=
            numThreads * regClasses.at(VecPredRegClass).numRegs());
    assert(params.numPhysCCRegs >=
            numThreads * regClasses.at(CCRegClass).numRegs());

    // Just make this a warning and go ahead anyway, to keep from having to
    // add checks everywhere.
    warn_if(regClasses.at(CCRegClass).numRegs() == 0 &&
            params.numPhysCCRegs != 0,
            "Non-zero number of physical CC regs specified, even though\n"
            "    ISA does not use them.");

    rename.setScoreboard(&scoreboard);
    iew.setScoreboard(&scoreboard);
    iew.setRob(&rob);

    // Setup the rename map for whichever stages need it.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        isa[tid] = dynamic_cast<TheISA::ISA *>(params.isa[tid]);
        warn("Setting isa ptr of cpu to %p", isa[tid]);
        commitRenameMap[tid].init(regClasses, &regFile, &freeList, tid);
        renameMap[tid].init(regClasses, &regFile, &freeList, tid);
    }

    // Initialize rename map to assign physical registers to the
    // architectural registers for active threads only.
    for (ThreadID tid = 0; tid < active_threads; tid++) {
        for (auto type = (RegClassType)0; type <= RMiscRegClass;
                type = (RegClassType)(type + 1)) {
            for (RegIndex ridx = 0; ridx < regClasses.at(type).numRegs();
                    ++ridx) {
                // Note that we can't use the rename() method because we don't
                // want special treatment for the zero register at this point
                RegId rid = RegId(type, ridx);
                PhysRegIdPtr phys_reg = freeList.getReg(type, tid);
                renameMap[tid].setEntry(rid, VirtRegId(phys_reg));
                commitRenameMap[tid].setEntry(rid, VirtRegId(phys_reg));
            }
        }
    }

    rename.setRenameMap(renameMap);
    commit.setRenameMap(commitRenameMap);
    rename.setFreeList(&freeList);

    // Setup the ROB for whichever stages need it.
    commit.setROB(&rob);

    lastActivatedCycle = 0;

    DPRINTF(O3CPU, "Creating O3CPU object.\n");

    // Setup any thread state.
    thread.resize(numThreads);

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (FullSystem) {
            thread[tid] = new ThreadState(this, tid, NULL);
        } else {
            if (tid < params.workload.size()) {
                DPRINTF(O3CPU, "Workload[%i] process is %#x", tid,
                        thread[tid]);
                thread[tid] = new ThreadState(this, tid, params.workload[tid]);
            } else {
                //Allocate Empty thread so M5 can use later
                //when scheduling threads to CPU
                Process* dummy_proc = NULL;

                thread[tid] = new ThreadState(this, tid, dummy_proc);
            }
        }

        gem5::ThreadContext *tc;

        // Setup the TC that will serve as the interface to the threads/CPU.
        auto *o3_tc = new ThreadContext;

        tc = o3_tc;

        // If we're using a checker, then the TC should be the
        // CheckerThreadContext.
        if (params.checker) {
            tc = new CheckerThreadContext<ThreadContext>(o3_tc, checker);
        }

        o3_tc->cpu = this;
        o3_tc->thread = thread[tid];

        // Give the thread the TC.
        thread[tid]->tc = tc;

        // Add the TC to the CPU's list of TC's.
        threadContexts.push_back(tc);
    }

    // O3CPU always requires an interrupt controller.
    if (!params.switched_out && interrupts.empty()) {
        fatal("O3CPU %s has no interrupt controller.\n"
              "Ensure createInterruptController() is called.\n", name());
    }
}

void
CPU::regProbePoints()
{
    BaseCPU::regProbePoints();

    ppInstAccessComplete = new ProbePointArg<PacketPtr>(
            getProbeManager(), "InstAccessComplete");
    ppDataAccessComplete = new ProbePointArg<
        std::pair<DynInstPtr, PacketPtr>>(
                getProbeManager(), "DataAccessComplete");

    fetch.regProbePoints();
    rename.regProbePoints();
    iew.regProbePoints();
    commit.regProbePoints();
}

CPU::CPUStats::CPUStats(CPU *cpu)
    : statistics::Group(cpu),
      ADD_STAT(timesIdled, statistics::units::Count::get(),
               "Number of times that the entire CPU went into an idle state "
               "and unscheduled itself"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Total number of cycles that the CPU has spent unscheduled due "
               "to idling"),
      ADD_STAT(quiesceCycles, statistics::units::Cycle::get(),
               "Total number of cycles that CPU has spent quiesced or waiting "
               "for an interrupt"),
      ADD_STAT(committedInsts, statistics::units::Count::get(),
               "Number of Instructions Simulated"),
      ADD_STAT(committedOps, statistics::units::Count::get(),
               "Number of Ops (including micro ops) Simulated"),
      ADD_STAT(cpi, statistics::units::Rate<
                    statistics::units::Cycle, statistics::units::Count>::get(),
               "CPI: Cycles Per Instruction"),
      ADD_STAT(totalCpi, statistics::units::Rate<
                    statistics::units::Cycle, statistics::units::Count>::get(),
               "CPI: Total CPI of All Threads"),
      ADD_STAT(ipc, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "IPC: Instructions Per Cycle"),
      ADD_STAT(totalIpc, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "IPC: Total IPC of All Threads"),
      ADD_STAT(baseRetiring, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Retiring"),
      ADD_STAT(frontendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Frontend Bound"),
      ADD_STAT(frontendLatencyBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Frontend Latency Bound"),
      ADD_STAT(frontendBandwidthBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Frontend Bandwidth Bound"),
      ADD_STAT(badSpecBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Bad Speculation"),
      ADD_STAT(branchMissPrediction, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Branch Missprediction"),
      ADD_STAT(machineClears, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Machine Clears"),
      ADD_STAT(backendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Backend Bound"),
      ADD_STAT(coreBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Core Bound"),
      ADD_STAT(memoryBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Memory Bound"),
      ADD_STAT(l1Bound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--L1 Bound"),
      ADD_STAT(l2Bound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--L2 Bound"),
      ADD_STAT(l3Bound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--L3 Bound"),
      ADD_STAT(memBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--Memory Bound"),
      ADD_STAT(storeBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--Store Bound"),
      ADD_STAT(intRegfileReads, statistics::units::Count::get(),
               "Number of integer regfile reads"),
      ADD_STAT(intRegfileWrites, statistics::units::Count::get(),
               "Number of integer regfile writes"),
      ADD_STAT(fpRegfileReads, statistics::units::Count::get(),
               "Number of floating regfile reads"),
      ADD_STAT(fpRegfileWrites, statistics::units::Count::get(),
               "Number of floating regfile writes"),
      ADD_STAT(vecRegfileReads, statistics::units::Count::get(),
               "number of vector regfile reads"),
      ADD_STAT(vecRegfileWrites, statistics::units::Count::get(),
               "number of vector regfile writes"),
      ADD_STAT(vecPredRegfileReads, statistics::units::Count::get(),
               "number of predicate regfile reads"),
      ADD_STAT(vecPredRegfileWrites, statistics::units::Count::get(),
               "number of predicate regfile writes"),
      ADD_STAT(ccRegfileReads, statistics::units::Count::get(),
               "number of cc regfile reads"),
      ADD_STAT(ccRegfileWrites, statistics::units::Count::get(),
               "number of cc regfile writes"),
      ADD_STAT(miscRegfileReads, statistics::units::Count::get(),
               "number of misc regfile reads"),
      ADD_STAT(miscRegfileWrites, statistics::units::Count::get(),
               "number of misc regfile writes"),
      ADD_STAT(lastCommitTick, statistics::units::Count::get(),
               "The last tick to commit an instruction")
{
    // Register any of the O3CPU's stats here.
    timesIdled
        .prereq(timesIdled);

    idleCycles
        .prereq(idleCycles);

    quiesceCycles
        .prereq(quiesceCycles);

    // Number of Instructions simulated
    // --------------------------------
    // Should probably be in Base CPU but need templated
    // MaxThreads so put in here instead
    committedInsts
        .init(cpu->numThreads)
        .flags(statistics::total);

    committedOps
        .init(cpu->numThreads)
        .flags(statistics::total);

    cpi
        .precision(6);
    cpi = cpu->baseStats.numCycles / committedInsts;

    totalCpi
        .precision(6);
    totalCpi = cpu->baseStats.numCycles / sum(committedInsts);

    ipc
        .precision(6);
    ipc = committedInsts / cpu->baseStats.numCycles;

    totalIpc
        .precision(6);
    totalIpc = sum(committedInsts) / cpu->baseStats.numCycles;

    baseRetiring = committedInsts / (cpu->issueWidth * cpu->baseStats.numCycles);

    frontendBound = cpu->fetch.getFetchStats().fetchBubbles /
        (cpu->issueWidth * cpu->baseStats.numCycles);

    frontendLatencyBound = cpu->fetch.getFetchStats().fetchBubbles_max / cpu->baseStats.numCycles;

    frontendBandwidthBound = frontendBound - frontendLatencyBound;

    // badSpecBound = (INST_SPEC - INST_RETIRED + RECOVERY_BUBBLE)/(IssueBW * CPU_CYCLES)
    badSpecBound = (cpu->iew.getIEWStats().dispatchedInsts - committedInsts + cpu->commit.getCommitStats().recovery_bubble) /
         (cpu->issueWidth * cpu->baseStats.numCycles);

    // branchMissPrediction = Bad Speculation * BR_MIS_PRED/TOTAL_FLUSH
    branchMissPrediction = badSpecBound * cpu->commit.getCommitStats().branchMispredicts / cpu->commit.getCommitStats().totalSquash;

    machineClears = badSpecBound - branchMissPrediction;

    backendBound = 1 - (frontendBound + badSpecBound + baseRetiring);

    Scheduler* scheduler = cpu->iew.getScheduler();
    const auto &stats = scheduler->getStats();

    // Calculate raw proportions first
    auto rawCore = stats.exec_stall_cycle - stats.memstall_any_load - stats.memstall_any_store;
    auto rawMemory = stats.memstall_any_load + stats.memstall_any_store;
    auto rawTotal = rawCore + rawMemory;

    // Scale Level 2: ensure Core + Memory = Backend
    coreBound = backendBound * rawCore / rawTotal;
    memoryBound = backendBound * rawMemory / rawTotal;

    // Scale Level 3: ensure sub-components sum to Memory
    auto rawL1 = stats.memstall_any_load - stats.memstall_l1miss;
    auto rawL2 = stats.memstall_l1miss - stats.memstall_l2miss;
    auto rawL3 = stats.memstall_l2miss - stats.memstall_l3miss;
    auto rawL3Total = rawL1 + rawL2 + rawL3 + stats.memstall_l3miss + stats.memstall_any_store;

    l1Bound = memoryBound * rawL1 / rawL3Total;
    l2Bound = memoryBound * rawL2 / rawL3Total;
    l3Bound = memoryBound * rawL3 / rawL3Total;
    memBound = memoryBound * stats.memstall_l3miss / rawL3Total;
    storeBound = memoryBound * stats.memstall_any_store / rawL3Total;

    intRegfileReads
        .prereq(intRegfileReads);

    intRegfileWrites
        .prereq(intRegfileWrites);

    fpRegfileReads
        .prereq(fpRegfileReads);

    fpRegfileWrites
        .prereq(fpRegfileWrites);

    vecRegfileReads
        .prereq(vecRegfileReads);

    vecRegfileWrites
        .prereq(vecRegfileWrites);

    vecPredRegfileReads
        .prereq(vecPredRegfileReads);

    vecPredRegfileWrites
        .prereq(vecPredRegfileWrites);

    ccRegfileReads
        .prereq(ccRegfileReads);

    ccRegfileWrites
        .prereq(ccRegfileWrites);

    miscRegfileReads
        .prereq(miscRegfileReads);

    miscRegfileWrites
        .prereq(miscRegfileWrites);
}

void
CPU::tick()
{
    DPRINTF(O3CPU, "\n\nO3CPU: Ticking main, O3CPU.\n");
    assert(!switchedOut());
    assert(drainState() != DrainState::Drained);

    ++baseStats.numCycles;
    ipc_r.roll(1);
    cpi_r++;
    updateCycleCounters(BaseCPU::CPU_STATE_ON);

//    activity = false;

    //Tick each of the stages

    auto *hint_bpu = dynamic_cast<
        branch_prediction::btb_pred::DecoupledBPUWithBTB *>(fetch.getBp());
    if (hint_bpu) {
        hint_bpu->hintCycleTick();
    }

    commit.tick();
    iew.tick();
    rename.tick();
    decode.tick();
    fetch.tick();

    if (hint_bpu && hint_bpu->hasHintBundle()) {
        hint_bpu->getHintBundleController().tick(curCycle());
        serviceHintBundleDma();
    }
    const bool bundle_active = hint_bpu && hint_bpu->hasHintBundle() &&
        hint_bpu->getHintBundleController().activeOperation();

    fetchTimebuffer.advance();
    decodeTimebuffer.advance();
    renameTimebuffer.advance();
    iewTimebuffer.advance();
    timeBuffer.advance();

    activityRec.advance();

    if (removeInstsThisCycle) {
        cleanUpRemovedInsts();
    }

    if (!tickEvent.scheduled()) {
        if (_status == SwitchedOut) {
            DPRINTF(O3CPU, "Switched out!\n");
            // increment stat
            lastRunningCycle = curCycle();
        } else if ((!activityRec.active() || _status == Idle) &&
                   !bundle_active &&
                   !hintDmaCounts().leased) {
            DPRINTF(O3CPU, "Idle!\n");
            lastRunningCycle = curCycle();
            cpuStats.timesIdled++;
        } else {
            lastRunningCycle = curCycle();
            schedule(tickEvent, clockEdge(Cycles(1)));
            DPRINTF(O3CPU, "Scheduling next tick!\n");
        }
    }

    if (!FullSystem)
        updateThreadPriority();

    tryDrain();
}

branch_prediction::btb_pred::HintBundleController &
CPU::getHintBundleController()
{
    auto *bpu = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
        fetch.getBp());
    fatal_if(!bpu || !bpu->hasHintBundle(),
             "Hint Bundle CSR/DMA requires a Bundle-capable decoupled BPU");
    return bpu->getHintBundleController();
}

uint64_t
CPU::readHintBundleCsr(unsigned csr)
{
    return getHintBundleController().readCsr(csr);
}

uint64_t
CPU::hintBundleTraceWritten()
{
    return getHintBundleController().writtenRecords();
}

std::vector<uint8_t>
CPU::readHintBundleTrace(Addr address, unsigned size)
{
    // Post-run observation only. Data must already have reached physical
    // memory through successful timing WriteResp; never flush or synthesize
    // an unfinished transfer for the host reader.
    auto &controller = getHintBundleController();
    const uint64_t status = controller.readCsr(0x5c0);
    fatal_if(status & ((7ULL << 59) | (1ULL << 50)),
             "Cannot observe Bundle Trace before profiling/DMA drain");
    fatal_if(hintDmaCounts().leased,
             "Cannot observe Bundle Trace with outstanding DMA");
    const Addr base = controller.readCsr(0x5c1) << 20;
    const uint64_t bytes = controller.writtenRecords() * 22;
    fatal_if(!size || size > 1024 * 1024 || address < base ||
             address - base > bytes || size > bytes - (address - base),
             "Bundle Trace observer is outside completed records");
    fatal_if(address > MaxAddr - (size - 1) ||
             !system->isMemAddr(address) ||
             !system->isMemAddr(address + size - 1),
             "Bundle Trace observer is outside physical RAM");
    std::vector<uint8_t> result(size);
    system->physProxy.readBlob(address, result.data(), size);
    return result;
}

void
CPU::writeHintBundleCsr(unsigned csr, uint64_t value, ThreadID tid)
{
    auto &controller = getHintBundleController();
    // The write is delivered by DynInst::updateMiscRegs at retirement. Keep
    // the launch hart and privilege fixed across asynchronous guest changes.
    const bool idle = !controller.activeOperation();
    const unsigned launch_privilege =
        tcBase(tid)->readMiscRegNoEffect(RiscvISA::MISCREG_PRV);
    // Capture before writeCsr: even a first request made by that call must
    // observe the launching hart/privilege rather than the preceding run.
    if (idle) {
        hintBundleThread = tid;
        hintBundlePrivilege = launch_privilege;
    }
    controller.writeCsr(csr, value);
    wakeCPU();
}

std::optional<CPU::HintDmaToken>
CPU::acquireHintBundleDma()
{
    for (unsigned i = 0; i < HintDmaSlots; ++i) {
        auto &slot = hintDmaSlots[i];
        if (slot.state != HintDmaSlotState::Free)
            continue;
        // Reuse follows release only: no Packet, callback or controller token
        // may survive release. Thus modulo generation wrap has no live ABA
        // peer, including wrap to zero; it is not an unbounded identity store.
        panic_if(slot.packet || slot.done, "Free Hint lease still has an owner");
        ++slot.generation;
        slot.state = HintDmaSlotState::Reserved;
        slot.bytes = 0;
        ++hintDmaAudit.acquired;
        sampleHintDmaCounts();
        wakeCPU();
        return HintDmaToken{uint8_t(i), slot.generation};
    }
    ++hintDmaAudit.acquireRefused;
    return std::nullopt;
}

CPU::HintDmaSlot &
CPU::checkedHintDmaSlot(HintDmaToken token)
{
    panic_if(token.slot >= HintDmaSlots, "Invalid Hint lease slot");
    auto &slot = hintDmaSlots[token.slot];
    panic_if(slot.state == HintDmaSlotState::Free ||
             slot.generation != token.generation, "Stale Hint lease token");
    return slot;
}

CPU::HintDmaPayload &
CPU::hintBundleDmaPayload(HintDmaToken token)
{
    auto &slot = checkedHintDmaSlot(token);
    panic_if(slot.state != HintDmaSlotState::Reserved &&
             slot.state != HintDmaSlotState::Ready,
             "Hint payload accessed while owned by transport");
    return slot.payload;
}

bool
CPU::submitHintBundleDma(HintDmaToken token, Addr address, bool write,
                         unsigned bytes, HintDmaDone done)
{
    auto &slot = checkedHintDmaSlot(token);
    if (slot.state != HintDmaSlotState::Reserved)
        return false;
    fatal_if(!bytes || bytes > 64 || address > MaxAddr - (bytes - 1) ||
             (address & 63) + bytes > 64 || !done, "Invalid Hint DMA beat");
    fatal_if(!hintDmaPort.isConnected(), "Hint DMA master is not connected");
    panic_if(hintDmaQueueCount >= HintDmaSlots, "Hint submission queue overflow");
    auto request = std::make_shared<Request>(address, bytes,
        Request::PHYSICAL | Request::UNCACHEABLE | Request::STRICT_ORDER,
        hintDmaRequestorId);
    request->setContext(tcBase(hintBundleThread)->contextId());
    slot.packet = new Packet(request, write ? MemCmd::WriteReq : MemCmd::ReadReq);
    slot.packet->dataStatic(slot.payload.data());
    slot.packet->senderState = new HintDmaSenderState(token);
    slot.thread = hintBundleThread;
    slot.privilege = hintBundlePrivilege;
    slot.bytes = bytes;
    slot.write = write;
    slot.done = std::move(done);
    slot.state = HintDmaSlotState::Queued;
    hintDmaQueue[(hintDmaQueueHead + hintDmaQueueCount) % HintDmaSlots] = token.slot;
    ++hintDmaQueueCount;
    ++hintDmaAudit.submitted;
    sampleHintDmaCounts();
    wakeCPU();
    return true;
}

void
CPU::releaseHintBundleDma(HintDmaToken token)
{
    auto &slot = checkedHintDmaSlot(token);
    panic_if(slot.state != HintDmaSlotState::Reserved &&
             slot.state != HintDmaSlotState::Ready,
             "Cannot release an unreturned Hint transaction");
    panic_if(slot.packet || slot.done, "Released Hint lease has packet/callback owner");
    if (slot.state == HintDmaSlotState::Reserved)
        ++hintDmaAudit.canceledReserved;
    else if (slot.write)
        ++hintDmaAudit.releasedWrites;
    else
        ++hintDmaAudit.releasedReads;
    slot.state = HintDmaSlotState::Free;
    slot.bytes = 0;
    ++hintDmaAudit.released;
    sampleHintDmaCounts();
}

CPU::HintDmaCounts
CPU::hintDmaCounts() const
{
    HintDmaCounts c;
    for (const auto &slot : hintDmaSlots) {
        if (slot.state == HintDmaSlotState::Free)
            continue;
        ++c.leased;
        if (slot.state == HintDmaSlotState::Reserved) {
            ++c.reserved;
            continue;
        }
        if (slot.write) { ++c.writeLeased; c.writeBytes += slot.bytes; }
        else { ++c.readLeased; c.readBytes += slot.bytes; }
        if (slot.state == HintDmaSlotState::Queued) ++c.queued;
        if (slot.state == HintDmaSlotState::Sent) {
            ++c.sent;
            if (slot.write) ++c.writeSent; else ++c.readSent;
        }
        if (slot.state == HintDmaSlotState::Ready) {
            ++c.ready;
            if (slot.write) ++c.writeReady; else ++c.readReady;
        }
    }
    return c;
}

void
CPU::sampleHintDmaCounts()
{
    const auto c = hintDmaCounts();
    auto &a = hintDmaAudit;
    panic_if(c.leased != c.reserved + c.queued + c.sent + c.ready ||
             c.queued != hintDmaQueueCount || c.sent != hintDmaAcceptedCount ||
             a.acquired - a.released != c.leased ||
             a.accepted - a.responses != c.sent ||
             (hintDmaRetryGrant && !hintDmaBlocked),
             "Hint shared-pool ownership conservation failed");
    a.maxLeased = std::max<uint64_t>(a.maxLeased, c.leased);
    a.maxReserved = std::max<uint64_t>(a.maxReserved, c.reserved);
    a.maxQueued = std::max<uint64_t>(a.maxQueued, c.queued);
    a.maxOutstanding = std::max<uint64_t>(a.maxOutstanding, c.sent);
    a.maxReady = std::max<uint64_t>(a.maxReady, c.ready);
    a.maxReadLeased = std::max<uint64_t>(a.maxReadLeased, c.readLeased);
    a.maxWriteLeased = std::max<uint64_t>(a.maxWriteLeased, c.writeLeased);
    a.maxReadSent = std::max<uint64_t>(a.maxReadSent, c.readSent);
    a.maxWriteSent = std::max<uint64_t>(a.maxWriteSent, c.writeSent);
    a.maxReadReady = std::max<uint64_t>(a.maxReadReady, c.readReady);
    a.maxWriteReady = std::max<uint64_t>(a.maxWriteReady, c.writeReady);
    a.maxReadBytes = std::max<uint64_t>(a.maxReadBytes, c.readBytes);
    a.maxWriteBytes = std::max<uint64_t>(a.maxWriteBytes, c.writeBytes);
    if (c.readSent && c.writeSent)
        a.maxMixedSent = std::max<uint64_t>(a.maxMixedSent, c.sent);
    if (c.sent)
        a.maxReadyWhileSent = std::max<uint64_t>(a.maxReadyWhileSent, c.ready);
}

void
CPU::bindHintDmaPool(HintController &controller)
{
    controller.setDmaPool(
        [this] { return acquireHintBundleDma(); },
        [this](HintDmaToken t) -> HintDmaPayload & { return hintBundleDmaPayload(t); },
        [this](HintDmaToken t, Addr a, bool w, unsigned n, HintDmaDone done) {
            return submitHintBundleDma(t, a, w, n, std::move(done));
        },
        [this](HintDmaToken t) { releaseHintBundleDma(t); });
}

Port &
CPU::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "hint_dma_port")
        return hintDmaPort;
    return BaseCPU::getPort(if_name, idx);
}

void
CPU::serviceHintBundleDma()
{
    if (!hintDmaQueueCount || (hintDmaBlocked && !hintDmaRetryGrant))
        return;
    // Normal service and retry callbacks share the same single-cycle quota.
    // A granted retry postponed here remains granted, not waiting for another
    // notification that the downstream will never send.
    if (hintDmaLastAttempt && *hintDmaLastAttempt == curCycle())
        return;
    const unsigned index = hintDmaQueue[hintDmaQueueHead];
    auto &slot = hintDmaSlots[index];
    panic_if(slot.state != HintDmaSlotState::Queued ||
             (hintDmaBlocked && *hintDmaBlocked != index),
             "Hint submission/retry owner mismatch");
    auto *packet = slot.packet;
    bool valid = true;
    for (unsigned i = 0; valid && i < slot.bytes; ++i)
        valid = system->isMemAddr(packet->getAddr() + i);
    auto *tc = tcBase(slot.thread);
    auto *riscv_mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
    fatal_if(!riscv_mmu, "Hint DMA requires RISC-V protection checks");
    if (valid) {
        riscv_mmu->pma->check(packet->req);
        valid = riscv_mmu->getPMP()->pmpCheckPhysical(packet->req,
            slot.write ? BaseMMU::Write : BaseMMU::Read,
            static_cast<RiscvISA::PrivilegeMode>(slot.privilege)) == NoFault;
    }
    if (!valid) {
        ++hintDmaAudit.accessFaults;
        auto done = std::move(slot.done);
        slot.done = {};
        const HintDmaToken token{uint8_t(index), slot.generation};
        delete packet->senderState;
        packet->senderState = nullptr;
        delete packet;
        slot.packet = nullptr;
        slot.state = HintDmaSlotState::Ready;
        hintDmaQueueHead = (hintDmaQueueHead + 1) % HintDmaSlots;
        --hintDmaQueueCount;
        hintDmaBlocked.reset();
        hintDmaRetryGrant = false;
        sampleHintDmaCounts();
        done(token, false, uint64_t(curCycle()));
        return;
    }
    hintDmaLastAttempt = curCycle();
    hintDmaAttemptsThisCycle = 0;
    ++hintDmaAttemptsThisCycle;
    ++hintDmaAudit.attempts;
    hintDmaAudit.maxAttemptsPerCycle = std::max<uint64_t>(
        hintDmaAudit.maxAttemptsPerCycle, hintDmaAttemptsThisCycle);
    if (hintDmaPort.sendTimingReq(packet)) {
        slot.state = HintDmaSlotState::Sent;
        panic_if(hintDmaAcceptedCount >= HintDmaSlots, "Hint accepted-order observer overflow");
        hintDmaAcceptedOrder[hintDmaAcceptedCount++] = index;
        hintDmaQueueHead = (hintDmaQueueHead + 1) % HintDmaSlots;
        --hintDmaQueueCount;
        hintDmaBlocked.reset();
        hintDmaRetryGrant = false;
        ++hintDmaAudit.accepted;
        hintDmaAudit.bytes += slot.bytes;
        hintDmaAudit.maxBeat = std::max<uint64_t>(hintDmaAudit.maxBeat, slot.bytes);
        hintDmaAudit.lineCrossings += (packet->getAddr() & 63) + slot.bytes > 64;
        if (slot.write) {
            ++hintDmaAudit.writes; hintDmaAudit.writeBytes += slot.bytes;
            if (slot.bytes == 64) ++hintDmaAudit.writes64;
            else ++hintDmaAudit.partialWrites;
        } else {
            ++hintDmaAudit.reads; hintDmaAudit.readBytes += slot.bytes;
            if (slot.bytes == 64) ++hintDmaAudit.reads64;
            else ++hintDmaAudit.partialReads;
        }
    } else {
        ++hintDmaAudit.refused;
        hintDmaBlocked = index;
        hintDmaRetryGrant = false;
    }
    sampleHintDmaCounts();
}

void
CPU::HintDmaPort::recvReqRetry()
{
    panic_if(!cpu.hintDmaBlocked || cpu.hintDmaRetryGrant,
             "Unexpected Hint DMA retry");
    ++cpu.hintDmaAudit.retries;
    cpu.hintDmaRetryGrant = true;
    if (cpu.hintDmaLastAttempt && *cpu.hintDmaLastAttempt == cpu.curCycle())
        ++cpu.hintDmaAudit.retryDeferred;
    cpu.serviceHintBundleDma();
    cpu.wakeCPU();
}

bool
CPU::HintDmaPort::recvTimingResp(PacketPtr packet)
{
    auto *state = dynamic_cast<HintDmaSenderState *>(packet->senderState);
    panic_if(!state, "Hint DMA response has no lease sender state");
    const auto token = state->token;
    auto &slot = cpu.checkedHintDmaSlot(token);
    panic_if(slot.packet != packet || slot.state != HintDmaSlotState::Sent ||
             packet->getConstPtr<uint8_t>() != slot.payload.data(),
             "Hint DMA response owner/payload mismatch");
    const bool success = !packet->isError();
    unsigned position = 0;
    while (position < cpu.hintDmaAcceptedCount &&
           cpu.hintDmaAcceptedOrder[position] != token.slot) ++position;
    panic_if(position == cpu.hintDmaAcceptedCount, "Response absent from acceptance observer");
    cpu.hintDmaAudit.outOfOrderResponses += position != 0;
    if (position) {
        if (slot.write) ++cpu.hintDmaAudit.outOfOrderWrites;
        else ++cpu.hintDmaAudit.outOfOrderReads;
    }
    for (unsigned i = position + 1; i < cpu.hintDmaAcceptedCount; ++i)
        cpu.hintDmaAcceptedOrder[i - 1] = cpu.hintDmaAcceptedOrder[i];
    --cpu.hintDmaAcceptedCount;
    ++cpu.hintDmaAudit.responses;
    cpu.hintDmaAudit.responseErrors += !success;
    if (success && slot.write) {
        // Difftest-only mirror, not a second functional DMA payload buffer.
        std::vector<uint8_t> data(slot.payload.begin(), slot.payload.begin() + slot.bytes);
        cpu.syncHintBundleDma(packet->getAddr(), data);
    }
    auto done = std::move(slot.done);
    slot.done = {};
    slot.packet = nullptr;
    slot.state = HintDmaSlotState::Ready;
    packet->senderState = nullptr;
    delete state;
    delete packet;
    cpu.sampleHintDmaCounts();
    done(token, success, uint64_t(cpu.curCycle()));
    cpu.wakeCPU();
    return true;
}

bool
CPU::hintBundleNeedsTick() const
{
    // These existing accessors lack const overloads; observing status here
    // changes neither the predictor nor the controller.
    auto *bpu = dynamic_cast<
        branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
            const_cast<CPU *>(this)->fetch.getBp());
    return hintDmaCounts().leased || (bpu && bpu->hasHintBundle() &&
        bpu->getHintBundleController().activeOperation());
}

bool
CPU::hintBundleNeedsDrain() const
{
    auto *bpu = dynamic_cast<
        branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
            const_cast<CPU *>(this)->fetch.getBp());
    return hintDmaCounts().leased || (bpu && bpu->hasHintBundle() &&
        bpu->getHintBundleController().busy());
}

void
CPU::init()
{
    BaseCPU::init();
    if (auto *bpu = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
            fetch.getBp()); bpu && bpu->hasHintBundle()) {
      fatal_if(!switchedOut() && !hintDmaPort.isConnected(),
               "Bundle requires hint_dma_port");
      bindHintDmaPool(bpu->getHintBundleController());
    }

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        // Set noSquashFromTC so that the CPU doesn't squash when initially
        // setting up registers.
        thread[tid]->noSquashFromTC = true;
    }

    // Clear noSquashFromTC.
    for (int tid = 0; tid < numThreads; ++tid)
        thread[tid]->noSquashFromTC = false;

    commit.setThreads(thread);
}

void
CPU::startup()
{
    BaseCPU::startup();
    initInstructionPhases();

    fetch.startupStage();
    decode.startupStage();
    iew.startupStage();
    rename.startupStage();
    commit.startupStage();
    freeList.resetEntries();
}

void
CPU::activateThread(ThreadID tid)
{
    std::list<ThreadID>::iterator isActive =
        std::find(activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling activate thread.\n", tid);
    assert(!switchedOut());

    if (isActive == activeThreads.end()) {
        DPRINTF(O3CPU, "[tid:%i] Adding to active threads list\n", tid);

        activeThreads.push_back(tid);
    }
}

void
CPU::deactivateThread(ThreadID tid)
{
    // hardware transactional memory
    // shouldn't deactivate thread in the middle of a transaction
    assert(!commit.executingHtmTransaction(tid));

    //Remove From Active List, if Active
    std::list<ThreadID>::iterator thread_it =
        std::find(activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling deactivate thread.\n", tid);
    assert(!switchedOut());

    if (thread_it != activeThreads.end()) {
        DPRINTF(O3CPU,"[tid:%i] Removing from active threads list\n",
                tid);
        activeThreads.erase(thread_it);
    }

    fetch.deactivateThread(tid);
    commit.deactivateThread(tid);
}

Counter
CPU::totalInsts() const
{
    if (params().instructionPhaseControl)
        return restoreCommitted + warmupCommitted + measureCommitted;
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numInst;

    return total;
}

Counter
CPU::totalOps() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numOp;

    return total;
}

void
CPU::activateContext(ThreadID tid)
{
    assert(!switchedOut());

    // Needs to set each stage to running as well.
    activateThread(tid);

    // We don't want to wake the CPU if it is drained. In that case,
    // we just want to flag the thread as active and schedule the tick
    // event from drainResume() instead.
    if (drainState() == DrainState::Drained)
        return;

    // If we are time 0 or if the last activation time is in the past,
    // schedule the next tick and wake up the fetch unit
    if (lastActivatedCycle == 0 || lastActivatedCycle < curTick()) {
        scheduleTickEvent(Cycles(0));

        // Be sure to signal that there's some activity so the CPU doesn't
        // deschedule itself.
        activityRec.activity();
        fetch.wakeFromQuiesce();

        Cycles cycles(curCycle() - lastRunningCycle);
        // @todo: This is an oddity that is only here to match the stats
        if (cycles != 0)
            --cycles;
        cpuStats.quiesceCycles += cycles;

        lastActivatedCycle = curTick();

        _status = Running;

        BaseCPU::activateContext(tid);
    }
}

void
CPU::suspendContext(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Suspending Thread Context.\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        if (!hintBundleNeedsTick())
            unscheduleTickEvent();
        lastRunningCycle = curCycle();
        _status = Idle;
    }

    DPRINTF(Quiesce, "Suspending Context\n");

    BaseCPU::suspendContext(tid);
}

void
CPU::haltContext(ThreadID tid)
{
    //For now, this is the same as deallocate
    DPRINTF(O3CPU,"[tid:%i] Halt Context called. Deallocating\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);
    removeThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        if (tickEvent.scheduled() && !hintBundleNeedsTick())
        {
            unscheduleTickEvent();
        }
        lastRunningCycle = curCycle();
        _status = Idle;
    }
    updateCycleCounters(BaseCPU::CPU_STATE_SLEEP);
}

void
CPU::insertThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Initializing thread into CPU");
    // Will change now that the PC and thread state is internal to the CPU
    // and not in the ThreadContext.
    gem5::ThreadContext *src_tc;
    if (FullSystem)
        src_tc = system->threads[tid];
    else
        src_tc = tcBase(tid);

    //Bind Int Regs to Rename Map
    const auto &regClasses = isa[tid]->regClasses();

    for (auto type = (RegClassType)0; type <= RMiscRegClass;
            type = (RegClassType)(type + 1)) {
        for (RegIndex idx = 0; idx < regClasses.at(type).numRegs(); idx++) {
            PhysRegIdPtr phys_reg = freeList.getReg(type, tid);
            renameMap[tid].setEntry(RegId(type, idx), VirtRegId(phys_reg));
            scoreboard.setReg(phys_reg);
        }
    }

    //Copy Thread Data Into RegFile
    //copyFromTC(tid);

    //Set PC/NPC/NNPC
    pcState(src_tc->pcState(), tid);

    src_tc->setStatus(gem5::ThreadContext::Active);

    activateContext(tid);

    //Reset ROB/IQ/LSQ Entries
    commit.rob->resetEntries();
    freeList.resetEntries();
}

void
CPU::removeThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Removing thread context from CPU.\n", tid);

    // Copy Thread Data From RegFile
    // If thread is suspended, it might be re-allocated
    // copyToTC(tid);


    // @todo: 2-27-2008: Fix how we free up rename mappings
    // here to alleviate the case for double-freeing registers
    // in SMT workloads.

    // clear all thread-specific states in each stage of the pipeline
    // since this thread is going to be completely removed from the CPU
    commit.clearStates(tid);
    fetch.clearStates(tid);
    decode.clearStates(tid);
    rename.clearStates(tid);
    iew.clearStates(tid);

    // Flush out any old data from the time buffers.
    for (int i = 0; i < timeBuffer.getSize(); ++i) {
        timeBuffer.advance();
        fetchTimebuffer.advance();
        decodeTimebuffer.advance();
        renameTimebuffer.advance();
        iewTimebuffer.advance();
    }

    assert(iew.ldstQueue.getCount(tid) == 0);
    assert(commit.rob->isEmpty(tid));

    // Reset per-thread Preg occupancy accounting so a reinserted tid
    // does not carry stale quota usage.
    freeList.resetThreadUsed(tid);

    // Reset ROB/IQ/LSQ Entries

    // Commented out for now.  This should be possible to do by
    // telling all the pipeline stages to drain first, and then
    // checking until the drain completes.  Once the pipeline is
    // drained, call resetEntries(). - 10-09-06 ktlim
/*
    if (activeThreads.size() >= 1) {
        commit.rob->resetEntries();
        iew.resetEntries();
    }
*/
}

Fault
CPU::getInterrupts()
{
    // Check if there are any outstanding interrupts
    return interrupts[0]->getInterrupt();
}

int
CPU::getInterruptsNO()
{
    // Check if there are any outstanding interrupts
    return interrupts[0]->getInterruptNO();
}

void
CPU::processInterrupts(const Fault &interrupt)
{
    // Check for interrupts here.  For now can copy the code that
    // exists within isa_fullsys_traits.hh.  Also assume that thread 0
    // is the one that handles the interrupts.
    // @todo: Possibly consolidate the interrupt checking code.
    // @todo: Allow other threads to handle interrupts.

    assert(interrupt != NoFault);
    interrupts[0]->updateIntrInfo();

    DPRINTF(O3CPU, "Interrupt %s being handled\n", interrupt->name());
    trap(interrupt, 0, nullptr);
}

void
CPU::trap(const Fault &fault, ThreadID tid, const StaticInstPtr &inst)
{
    // Pass the thread's TC into the invoke method.
    fault->invoke(threadContexts[tid], inst);
}

void
CPU::serializeThread(CheckpointOut &cp, ThreadID tid) const
{
    thread[tid]->serialize(cp);
}

void
CPU::unserializeThread(CheckpointIn &cp, ThreadID tid)
{
    thread[tid]->unserialize(cp);
}

DrainState
CPU::drain()
{
    // Deschedule any power gating event (if any)
    deschedulePowerGatingEvent();

    // If the CPU isn't doing anything, then return immediately.
    if (switchedOut())
        return DrainState::Drained;

    DPRINTF(Drain, "Draining...\n");

    // We only need to signal a drain to the commit stage as this
    // initiates squashing controls the draining. Once the commit
    // stage commits an instruction where it is safe to stop, it'll
    // squash the rest of the instructions in the pipeline and force
    // the fetch stage to stall. The pipeline will be drained once all
    // in-flight instructions have retired.
    commit.drain();

    // Wake the CPU and record activity so everything can drain out if
    // the CPU was not able to immediately drain.
    if (!isCpuDrained())  {
        // If a thread is suspended, wake it up so it can be drained
        for (auto t : threadContexts) {
            if (t->status() == gem5::ThreadContext::Suspended){
                DPRINTF(Drain, "Currently suspended so activate %i \n",
                        t->threadId());
                t->activate();
                // As the thread is now active, change the power state as well
                activateContext(t->threadId());
            }
        }

        wakeCPU();
        activityRec.activity();

        DPRINTF(Drain, "CPU not drained\n");

        return DrainState::Draining;
    } else {
        DPRINTF(Drain, "CPU is already drained\n");
        if (tickEvent.scheduled())
            deschedule(tickEvent);

        // Flush out any old data from the time buffers.  In
        // particular, there might be some data in flight from the
        // fetch stage that isn't visible in any of the CPU buffers we
        // test in isCpuDrained().
        for (int i = 0; i < timeBuffer.getSize(); ++i) {
            timeBuffer.advance();
            fetchTimebuffer.advance();
            decodeTimebuffer.advance();
            renameTimebuffer.advance();
            iewTimebuffer.advance();
        }

        drainSanityCheck();
        return DrainState::Drained;
    }
}

bool
CPU::tryDrain()
{
    if (drainState() != DrainState::Draining || !isCpuDrained())
        return false;

    if (tickEvent.scheduled())
        deschedule(tickEvent);

    DPRINTF(Drain, "CPU done draining, processing drain event\n");
    signalDrainDone();

    return true;
}

void
CPU::drainSanityCheck() const
{
    assert(isCpuDrained());
    fetch.drainSanityCheck();
    decode.drainSanityCheck();
    rename.drainSanityCheck();
    iew.drainSanityCheck();
    commit.drainSanityCheck();
}

bool
CPU::isCpuDrained() const
{
    bool drained(true);
    if (hintBundleNeedsDrain()) {
        DPRINTF(Drain, "Hint controller/DMA not drained.\n");
        drained = false;
    }

    if (!instList.empty() || !removeList.empty()) {
        DPRINTF(Drain, "Main CPU structures not drained.\n");
        drained = false;
    }

    if (!fetch.isDrained()) {
        DPRINTF(Drain, "Fetch not drained.\n");
        drained = false;
    }

    if (!decode.isDrained()) {
        DPRINTF(Drain, "Decode not drained.\n");
        drained = false;
    }

    if (!rename.isDrained()) {
        DPRINTF(Drain, "Rename not drained.\n");
        drained = false;
    }

    if (!iew.isDrained()) {
        DPRINTF(Drain, "IEW not drained.\n");
        drained = false;
    }

    if (!commit.isDrained()) {
        DPRINTF(Drain, "Commit not drained.\n");
        drained = false;
    }

    return drained;
}

void CPU::commitDrained(ThreadID tid) { fetch.drainStall(tid); }

void
CPU::drainResume()
{
    if (switchedOut())
        return;

    DPRINTF(Drain, "Resuming...\n");
    verifyMemoryMode();

    fetch.drainResume();
    commit.drainResume();

    _status = Idle;
    for (ThreadID i = 0; i < thread.size(); i++) {
        if (thread[i]->status() == gem5::ThreadContext::Active) {
            DPRINTF(Drain, "Activating thread: %i\n", i);
            activateThread(i);
            _status = Running;
        }
    }

    assert(!tickEvent.scheduled());
    if (_status == Running || hintBundleNeedsTick())
        schedule(tickEvent, nextCycle());

    // Reschedule any power gating event (if any)
    schedulePowerGatingEvent();
}

void
CPU::switchOut()
{
    panic_if(hintBundleNeedsDrain(), "Cannot switch out outstanding Hint DMA");
    DPRINTF(O3CPU, "Switching out\n");
    BaseCPU::switchOut();

    activityRec.reset();

    _status = SwitchedOut;

    if (checker)
        checker->switchOut();
}

void
CPU::takeOverFrom(BaseCPU *oldCPU)
{
    panic_if(hintBundleNeedsDrain(), "New CPU has outstanding Hint DMA");
    if (auto *old_o3 = dynamic_cast<CPU *>(oldCPU)) {
        panic_if(old_o3->hintBundleNeedsDrain(),
                 "Old CPU has outstanding Hint DMA at takeover");
        // Normal configuration gives every CPU its own membus endpoint.
        // Only transfer the old endpoint for legacy callers lacking one.
        if (!hintDmaPort.isConnected() && old_o3->hintDmaPort.isConnected())
            hintDmaPort.takeOverFrom(&old_o3->hintDmaPort);
    }
    BaseCPU::takeOverFrom(oldCPU);

    if (auto *bpu = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
            fetch.getBp()); bpu && bpu->hasHintBundle()) {
        fatal_if(!hintDmaPort.isConnected(), "Takeover requires Hint DMA master");
        bindHintDmaPool(bpu->getHintBundleController());
    }

    fetch.takeOverFrom();
    decode.takeOverFrom();
    rename.takeOverFrom();
    iew.takeOverFrom();
    commit.takeOverFrom();

    assert(!tickEvent.scheduled());

    auto *oldO3CPU = dynamic_cast<CPU *>(oldCPU);
    if (oldO3CPU)
        globalSeqNum = oldO3CPU->globalSeqNum;

    lastRunningCycle = curCycle();
    _status = Idle;
}

void
CPU::verifyMemoryMode() const
{
    if (!system->isTimingMode()) {
        fatal("The O3 CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

RegVal
CPU::readMiscRegNoEffect(int misc_reg, ThreadID tid) const
{
    return isa[tid]->readMiscRegNoEffect(misc_reg);
}

RegVal
CPU::readMiscReg(int misc_reg, ThreadID tid)
{
    cpuStats.miscRegfileReads++;
    return isa[tid]->readMiscReg(misc_reg);
}

void
CPU::setMiscRegNoEffect(int misc_reg, RegVal val, ThreadID tid)
{
    isa[tid]->setMiscRegNoEffect(misc_reg, val);
}

void
CPU::setMiscReg(int misc_reg, RegVal val, ThreadID tid)
{
    cpuStats.miscRegfileWrites++;
    isa[tid]->setMiscReg(misc_reg, val);
}

RegVal
CPU::getReg(PhysRegIdPtr phys_reg)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getReg(phys_reg);
}

RegVal
CPU::getReg(VirtRegId virt_reg)
{
    switch (virt_reg.PhyReg()->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getReg(virt_reg);
}

void
CPU::getReg(PhysRegIdPtr phys_reg, void *val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    regFile.getReg(phys_reg, val);
}

void *
CPU::getWritableReg(PhysRegIdPtr phys_reg)
{
    switch (phys_reg->classValue()) {
      case VecRegClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, RegVal val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileWrites++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileWrites++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileWrites++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, const void *val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileWrites++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileWrites++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileWrites++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

RegVal
CPU::getArchReg(const RegId &reg, ThreadID tid)
{
    VirtRegId virt_reg = commitRenameMap[tid].lookup(reg);
    return regFile.getReg(virt_reg);
}

void
CPU::getArchReg(const RegId &reg, void *val, ThreadID tid)
{
    VirtRegId virt_reg = commitRenameMap[tid].lookup(reg);
    assert(!virt_reg.IEOper());
    regFile.getReg(virt_reg.PhyReg(), val);
}

void *
CPU::getWritableArchReg(const RegId &reg, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg).PhyReg();
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setArchReg(const RegId &reg, RegVal val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg).PhyReg();
    regFile.setReg(phys_reg, val);
}

void
CPU::setArchReg(const RegId &reg, const void *val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg).PhyReg();
    regFile.setReg(phys_reg, val);
}

const PCStateBase &
CPU::pcState(ThreadID tid)
{
    return commit.pcState(tid);
}

void
CPU::pcState(const PCStateBase &val, ThreadID tid)
{
    commit.pcState(val, tid);
}

void
CPU::squashFromTC(ThreadID tid)
{
    thread[tid]->noSquashFromTC = true;
    commit.generateTCEvent(tid);
}

CPU::ListIt
CPU::addInst(const DynInstPtr &inst)
{
    instList.push_back(inst);

    return --(instList.end());
}

void
CPU::logInstructionPhase(const char *phase) const
{
    inform("H2P_PHASE phase=%s tick=%llu committed=%llu warmup=%llu "
           "measure=%llu retired_post_restore=%llu terminal_fusion_excess=%u "
           "warmup_fusion_split=%u core_cycle=%llu reset_retired=%llu",
           phase, curTick(), thread[0]->numInst,
           warmupCommitted, measureCommitted, retiredPostRestore,
           terminalFusionExcess, warmupFusionSplit,
           static_cast<uint64_t>(curCycle()), measureResetRetired);
}

void
CPU::initInstructionPhases()
{
    if (!params().instructionPhaseControl)
        return;
    fatal_if(numThreads != 1, "Instruction phase control requires one thread");
    fatal_if(params().max_insts_any_thread &&
             params().max_insts_any_thread <= warmupInstCount,
             "maxinsts must exceed warmup instructions");
    auto *bpu = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
        fetch.getBp());
    const Addr base = params().gcptRestoreBase;
    if (base) {
        fatal_if(!FullSystem, "GCPT restore detection requires full-system mode");
        const auto size = system->physProxy.read<uint32_t>(base + 4);
        fatal_if(size < 132 || size > 0x100000,
                 "Unsupported GCPT restorer header size: %#x", size);
        fatal_if(system->physProxy.read<uint64_t>(base + 0xecdb0) != 0xbeef,
                 "GCPT boot flag does not identify a checkpoint restore");
        restoreTargetPc = system->physProxy.read<uint64_t>(base + 0xecdb8);
        fatal_if(!restoreTargetPc || (restoreTargetPc & 1),
                 "Invalid GCPT saved target PC: %#x", restoreTargetPc);
        // LibCheckpointAlpha uses .option norvc. Match all integer restores,
        // ending with ld sp,16(sp); mret, rather than choosing the first mret.
        unsigned candidates = 0;
        for (Addr offset = 124; offset + 4 <= size; offset += 4) {
            if (system->physProxy.read<uint32_t>(base + offset) != 0x30200073)
                continue;
            bool matches = true;
            for (unsigned i = 0; i < 31; ++i) {
                const unsigned reg = i == 0 ? 1 : (i == 30 ? 2 : i + 2);
                const uint32_t encoding = (reg * 8 << 20) | (2 << 15) |
                                          (3 << 12) | (reg << 7) | 3;
                matches &= system->physProxy.read<uint32_t>(
                    base + offset - 124 + i * 4) == encoding;
            }
            if (matches) {
                restoreMretPc = base + offset;
                ++candidates;
            }
        }
        fatal_if(candidates != 1,
                 "Unsupported GCPT integer restore tail: found %u matches",
                 candidates);
        instructionPhase = InstructionPhase::Restore;
        inform("GCPT_RESTORE base=%#x size=%#x mret=%#x target=%#x",
               base, size, restoreMretPc, restoreTargetPc);
        registerExitCallback([this] {
            fatal_if(instructionPhase == InstructionPhase::Restore,
                     "Simulation ended before verified GCPT restore completion");
        });
    } else {
        instructionPhase = warmupInstCount ? InstructionPhase::Warmup :
                                            InstructionPhase::Measure;
    }
    if (bpu)
        bpu->h2pSetMeasurementActive(instructionPhase == InstructionPhase::Measure);
}

void
CPU::notifyHintBranchResolve(uint64_t ftq_id, Addr pc, Addr target,
                             uint64_t sequence, bool taken, ThreadID tid,
                             bool indirect, bool is_return, unsigned size)
{
    auto *bpu = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
        fetch.getBp());
    if (bpu) {
        bpu->h2pResolveAtExecute(ftq_id, pc, target, sequence, taken,
                                 instructionMeasurementActive(), tid, indirect, is_return, size);
    }
}

void
CPU::finishInstructionCommit(const DynInstPtr &inst)
{
    const auto &phaseParams = static_cast<const BaseO3CPUParams &>(params());
    auto *bpu = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB *>(
        fetch.getBp());
    if (inst->isNop() || inst->isInstPrefetch() ||
        (inst->isMicroop() && !inst->isLastMicroop()))
        return;
    const unsigned units = inst->staticInst->isFusion() ? 2 : 1;
    if (!params().instructionPhaseControl) {
        if (bpu)
            bpu->h2pNotifyCommit(units);
        return;
    }
    if (instructionPhase == InstructionPhase::Restore) {
        restoreCommitted += units;
        fatal_if(params().gcptRestoreMaxInsts &&
                 restoreCommitted > params().gcptRestoreMaxInsts,
                 "GCPT restore exceeded instruction watchdog");
        const Addr pc = inst->pcState().instAddr();
        if (pc == restoreMretPc - 124 + restoreTailCommitted * 4)
            ++restoreTailCommitted;
        else
            restoreTailCommitted = pc == restoreMretPc - 124 ? 1 : 0;
        if (pc == restoreMretPc) {
            fatal_if(restoreTailCommitted != 32 ||
                     inst->pcState().as<RiscvISA::PCState>().npc() != restoreTargetPc,
                     "GCPT restore tail or saved target mismatch at mret");
            logInstructionPhase("restore_complete");
            if (phaseParams.dumpWarmupStats && warmupInstCount)
                statistics::reset();
            instructionPhase = warmupInstCount ? InstructionPhase::Warmup :
                                                InstructionPhase::Measure;
            if (!warmupInstCount) {
                statistics::reset();
                if (bpu)
                    bpu->h2pSetMeasurementActive(true);
                logInstructionPhase("warmup_complete");
            }
        }
        return;
    }
    retiredPostRestore += units;
    // Account fused retirement as the same two architectural units used by
    // instDone, while allowing a phase/window boundary between those units.
    for (unsigned i = 0; i < units; ++i) {
        if (instructionPhase == InstructionPhase::Finished)
            break;
        thread[inst->threadNumber]->threadStats.numInsts++;
        cpuStats.committedInsts[inst->threadNumber]++;
        ipc_r++;
        cpi_r.roll(1);
        if (instructionPhase == InstructionPhase::Warmup) {
            ++warmupCommitted;
            if (warmupCommitted == warmupInstCount) {
                warmupFusionSplit = i + 1 < units;
                measureResetRetired = retiredPostRestore;
                logInstructionPhase("warmup_complete");
                if (phaseParams.dumpWarmupStats)
                    statistics::dump();
                statistics::reset();
                instructionPhase = InstructionPhase::Measure;
                if (bpu)
                    bpu->h2pSetMeasurementActive(true);
            }
        } else {
            ++measureCommitted;
            if (bpu)
                bpu->h2pNotifyCommit(1);
            const uint64_t measureLimit = phaseParams.measureInstCount ?
                phaseParams.measureInstCount :
                (params().max_insts_any_thread ?
                 params().max_insts_any_thread - warmupCommitted : 0);
            if (measureLimit && measureCommitted == measureLimit) {
                terminalFusionExcess = units - i - 1;
                instructionPhase = InstructionPhase::Finished;
                if (bpu)
                    bpu->h2pSetMeasurementActive(false);
                logInstructionPhase("measure_complete");
                exitSimLoop("a thread reached the max instruction count");
            }
        }
    }
}

void
CPU::instDone(ThreadID tid, const DynInstPtr &inst)
{
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        thread[tid]->numInst++;
        if (inst->staticInst->isFusion())
            thread[tid]->numInst++;
        if (!params().instructionPhaseControl) {
            const unsigned units = inst->staticInst->isFusion() ? 2 : 1;
            thread[tid]->threadStats.numInsts += units;
            cpuStats.committedInsts[tid] += units;
            ipc_r += units;
            for (unsigned i = 0; i < units; ++i)
                cpi_r.roll(1);
        }

        const uint64_t committedThreadInsts = thread[tid]->numInst;

        if (this->nextDumpInstCount && !dump_done
                && committedThreadInsts >= this->nextDumpInstCount) {
            fprintf(stderr, "Will trigger stat dump and reset\n");
            statistics::schedStatEvent(true, true, curTick(), 0);
            scheduleInstStop(tid,0,"Will trigger stat dump and reset");
            dump_done = true;

            /*if (this->repeatDumpInstCount) {
                this->nextDumpInstCount += this->repeatDumpInstCount;
            };*/
        }

        // Check for instruction-count-based events.
        thread[tid]->comInstEventQueue.serviceEvents(thread[tid]->numInst);

        if (!params().instructionPhaseControl &&
                this->warmupInstCount && !warmup_done &&
                committedThreadInsts >= this->warmupInstCount) {
            fprintf(stderr, "Will trigger stat dump and reset\n");
            statistics::schedStatEvent(true, true, curTick(), 0);
            scheduleInstStop(tid,0,"Will trigger stat dump and reset");
            warmup_done = true;
        }
    }

    thread[tid]->numOp++;
    thread[tid]->threadStats.numOps++;
    cpuStats.committedOps[tid]++;

    probeInstCommit(inst->staticInst, inst->pcState().instAddr());
    cpuStats.lastCommitTick = curTick();
}

void
CPU::removeFrontInst(const DynInstPtr &inst)
{
    DPRINTF(O3CPU, "Removing committed instruction [tid:%i] PC %s "
            "[sn:%lli]\n",
            inst->threadNumber, inst->pcState(), inst->seqNum);

    removeInstsThisCycle = true;

    instList.erase(inst->getInstListIt());
}

void
CPU::removeInstsNotInROB(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %i: Deleting instructions from instruction"
            " list.\n", tid);

    ListIt end_it;

    bool rob_empty = false;

    if (instList.empty()) {
        return;
    } else if (rob.isEmpty(tid)) {
        DPRINTF(O3CPU, "ROB is empty, squashing all insts.\n");
        end_it = instList.begin();
        rob_empty = true;
    } else {
        end_it = (rob.readTailInst(tid))->getInstListIt();
        DPRINTF(O3CPU, "ROB is not empty, squashing insts not in ROB.\n");
    }

    removeInstsThisCycle = true;

    ListIt inst_it = instList.end();

    inst_it--;

    // Walk through the instruction list, removing any instructions
    // that were inserted after the given instruction iterator, end_it.
    while (inst_it != end_it) {
        assert(!instList.empty());

        inst_it = squashInstIt(inst_it, tid);
    }

    // If the ROB was empty, then we actually need to remove the first
    // instruction as well.
    if (rob_empty) {
        squashInstIt(inst_it, tid);
    }
}

void
CPU::removeInstsUntil(const InstSeqNum &seq_num, ThreadID tid)
{
    assert(!instList.empty());

    removeInstsThisCycle = true;

    ListIt inst_iter = instList.end();

    inst_iter--;

    DPRINTF(O3CPU, "Deleting instructions from instruction "
            "list that are from [tid:%i] and above [sn:%lli] (end=%lli).\n",
            tid, seq_num, (*inst_iter)->seqNum);

    while ((*inst_iter)->seqNum > seq_num) {

        bool break_loop = (inst_iter == instList.begin());

        inst_iter = squashInstIt(inst_iter, tid);

        if (break_loop)
            break;
    }
}

CPU::ListIt
CPU::squashInstIt(ListIt &instIt, ThreadID tid)
{
    if ((*instIt)->threadNumber == tid) {
        DPRINTF(O3CPU, "Squashing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*instIt)->threadNumber,
                (*instIt)->seqNum,
                (*instIt)->pcState());

        // Mark it as squashed.
        (*instIt)->setSquashed();

        // @todo: Formulate a consistent method for deleting
        // instructions from the instruction list
        // Remove the instruction from the list.
        instIt = instList.erase(instIt);
    }
    return --instIt;
}

void
CPU::flushTLBs()
{
    BaseCPU::flushTLBs();
    fetch.flushFetchBuffer();
}

void
CPU::cleanUpRemovedInsts()
{
    while (!removeList.empty()) {
        DPRINTF(O3CPU, "Removing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*removeList.front())->threadNumber,
                (*removeList.front())->seqNum,
                (*removeList.front())->pcState());

        instList.erase(removeList.front());

        removeList.pop_front();
    }

    removeInstsThisCycle = false;
}
/*
void
CPU::removeAllInsts()
{
    instList.clear();
}
*/
void
CPU::dumpInsts()
{
    int num = 0;

    ListIt inst_list_it = instList.begin();

    cprintf("Dumping Instruction List\n");

    while (inst_list_it != instList.end()) {
        cprintf("Instruction:%i\nPC:%#x\n[tid:%i]\n[sn:%lli]\nIssued:%i\n"
                "Squashed:%i\n\n",
                num, (*inst_list_it)->pcState().instAddr(),
                (*inst_list_it)->threadNumber,
                (*inst_list_it)->seqNum, (*inst_list_it)->isIssued(),
                (*inst_list_it)->isSquashed());
        inst_list_it++;
        ++num;
    }
}
/*
void
CPU::wakeDependents(const DynInstPtr &inst)
{
    iew.wakeDependents(inst);
}
*/
void
CPU::wakeCPU()
{
    if (activityRec.active() || tickEvent.scheduled()) {
        DPRINTF(Activity, "CPU already running.\n");
        return;
    }

    DPRINTF(Activity, "Waking up CPU\n");

    Cycles cycles(curCycle() - lastRunningCycle);
    // @todo: This is an oddity that is only here to match the stats
    if (cycles > 1) {
        --cycles;
        cpuStats.idleCycles += cycles;
        baseStats.numCycles += cycles;
    }

    schedule(tickEvent, clockEdge());
}

void
CPU::wakeup(ThreadID tid)
{
    if (thread[tid]->status() != gem5::ThreadContext::Suspended)
        return;

    wakeCPU();

    DPRINTF(Quiesce, "Suspended Processor woken\n");
    threadContexts[tid]->activate();
}

ThreadID
CPU::getFreeTid()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!tids[tid]) {
            tids[tid] = true;
            return tid;
        }
    }

    return InvalidThreadID;
}

void
CPU::updateThreadPriority()
{
    if (activeThreads.size() > 1) {
        //DEFAULT TO ROUND ROBIN SCHEME
        //e.g. Move highest priority to end of thread list
        std::list<ThreadID>::iterator list_begin = activeThreads.begin();

        unsigned high_thread = *list_begin;

        activeThreads.erase(list_begin);

        activeThreads.push_back(high_thread);
    }
}

void
CPU::addThreadToExitingList(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %d is inserted to exitingThreads list\n", tid);

    // the thread trying to exit can't be already halted
    assert(tcBase(tid)->status() != gem5::ThreadContext::Halted);

    // make sure the thread has not been added to the list yet
    assert(exitingThreads.count(tid) == 0);

    // add the thread to exitingThreads list to mark that this thread is
    // trying to exit. The boolean value in the pair denotes if a thread is
    // ready to exit. The thread is not ready to exit until the corresponding
    // exit trap event is processed in the future. Until then, it'll be still
    // an active thread that is trying to exit.
    exitingThreads.emplace(std::make_pair(tid, false));
}

bool
CPU::isThreadExiting(ThreadID tid) const
{
    return exitingThreads.count(tid) == 1;
}

void
CPU::scheduleThreadExitEvent(ThreadID tid)
{
    assert(exitingThreads.count(tid) == 1);

    // exit trap event has been processed. Now, the thread is ready to exit
    // and be removed from the CPU.
    exitingThreads[tid] = true;

    // we schedule a threadExitEvent in the next cycle to properly clean
    // up the thread's states in the pipeline. threadExitEvent has lower
    // priority than tickEvent, so the cleanup will happen at the very end
    // of the next cycle after all pipeline stages complete their operations.
    // We want all stages to complete squashing instructions before doing
    // the cleanup.
    if (!threadExitEvent.scheduled()) {
        schedule(threadExitEvent, nextCycle());
    }
}

void
CPU::exitThreads()
{
    // there must be at least one thread trying to exit
    assert(exitingThreads.size() > 0);

    // terminate all threads that are ready to exit
    auto it = exitingThreads.begin();
    while (it != exitingThreads.end()) {
        ThreadID thread_id = it->first;
        bool readyToExit = it->second;

        if (readyToExit) {
            DPRINTF(O3CPU, "Exiting thread %d\n", thread_id);
            haltContext(thread_id);
            tcBase(thread_id)->setStatus(gem5::ThreadContext::Halted);
            it = exitingThreads.erase(it);
        } else {
            it++;
        }
    }
}

void
CPU::htmSendAbortSignal(ThreadID tid, uint64_t htm_uid,
        HtmFailureFaultCause cause)
{
    const Addr addr = 0x0ul;
    const int size = 8;
    const Request::Flags flags =
      Request::PHYSICAL|Request::STRICT_ORDER|Request::HTM_ABORT;

    // O3-specific actions
    iew.ldstQueue.resetHtmStartsStops(tid);
    commit.resetHtmStartsStops(tid);

    // notify l1 d-cache (ruby) that core has aborted transaction
    RequestPtr req =
        std::make_shared<Request>(addr, size, flags, _dataRequestorId);

    req->taskId(taskId());
    req->setContext(thread[tid]->contextId());
    req->setHtmAbortCause(cause);

    assert(req->isHTMAbort());

    PacketPtr abort_pkt = Packet::createRead(req);
    uint8_t *memData = new uint8_t[8];
    assert(memData);
    abort_pkt->dataStatic(memData);
    abort_pkt->setHtmTransactional(htm_uid);

    // TODO include correct error handling here
    if (!iew.ldstQueue.sendCoreTimingReq(abort_pkt)) {
        panic("HTM abort signal was not sent to the memory subsystem.");
    }
}

void
CPU::readGem5Regs(ThreadID tid)
{
    auto diffAllStates = this->diffAllStates[tid];
    for (int i = 0; i < 32; i++) {
        diffAllStates->gem5RegFile[i] = readArchIntReg(i, tid);
        diffAllStates->gem5RegFile[i + 32] = readArchFloatReg(i, tid);
        readArchVecReg(i, (uint64_t*)&diffAllStates->gem5RegFile.vr[i], tid);
    }
}

RegVal
CPU::readArchIntReg(int reg_idx, ThreadID tid)
{

    cpuStats.intRegfileReads++;
    PhysRegIdPtr phys_reg =
        commitRenameMap[tid].lookup(RegId(IntRegClass, reg_idx)).PhyReg();

    DPRINTF(Scoreboard, "Get map: x%i -> p%i\n", reg_idx, phys_reg->flatIndex());

    return regFile.getReg(phys_reg);
}

RegVal
CPU::readArchFloatReg(int reg_idx, ThreadID tid)
{
    cpuStats.fpRegfileReads++;
    PhysRegIdPtr phys_reg =
        commitRenameMap[tid].lookup(RegId(FloatRegClass, reg_idx)).PhyReg();
    DPRINTF(Scoreboard, "Get map: f%i -> p%i\n", reg_idx, phys_reg->flatIndex());

    return regFile.getReg(phys_reg);
}

void
CPU::readArchVecReg(int reg_idx, uint64_t *val,ThreadID tid)
{
    cpuStats.vecRegfileReads++;
    PhysRegIdPtr phys_reg =
        commitRenameMap[tid].lookup(RegId(VecRegClass, reg_idx)).PhyReg();
    DPRINTF(Scoreboard, "Get map: v%i -> p%i\n", reg_idx, phys_reg->flatIndex());

    regFile.getReg(phys_reg, val);
}

bool
CPU::isTraceInstruction(InstSeqNum seqNum) const
{
    return fetch.isTraceInstruction(seqNum);
}

const o3::TraceInstruction*
CPU::getTraceInstMetadata(InstSeqNum seqNum) const
{
    return fetch.getTraceInstMetadata(seqNum);
}

uint64_t
CPU::getTraceIndexForSeqNum(InstSeqNum seqNum) const
{
    return fetch.findTraceIndexForSeqNum(seqNum);
}

Addr
CPU::getTracePCByIndex(uint64_t index)
{
    return fetch.getTracePCByIndex(index);
}

void
CPU::cleanupTraceMetadataOnCommit(InstSeqNum seqNum)
{
    if (isTraceMode()) {
        fetch.cleanupTraceMetadataOnCommit(seqNum);
    }
}

InstSeqNum
CPU::getOldestInFlightSeqNum() const
{
    if (!instList.empty()) {
        return instList.front()->seqNum;
    }
    // No in-flight instructions: return a large value so cleanup can proceed.
    return std::numeric_limits<InstSeqNum>::max();
}

} // namespace o3
} // namespace gem5
