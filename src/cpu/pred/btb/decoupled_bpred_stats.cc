#include <algorithm>
#include <sstream>
#include <tuple>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "debug/BTB.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace
{

constexpr int NumS1SourceBuckets = 3;
constexpr int NumOverrideReasonBuckets = 4;
constexpr unsigned FetchBlockBytes = 32;

constexpr const char *S1SourceLabels[NumS1SourceBuckets] = {
    "fallthrough",
    "ubtb",
    "abtb"
};

constexpr const char *OverrideReasonLabels[NumOverrideReasonBuckets] = {
    "no_override",
    "fall_thru",
    "control_addr",
    "target"
};

int
overrideReasonBucket(OverrideReason reason)
{
    switch (reason) {
      case OverrideReason::NO_OVERRIDE:
        return 0;
      case OverrideReason::FALL_THRU:
        return 1;
      case OverrideReason::CONTROL_ADDR:
        return 2;
      case OverrideReason::TARGET:
        return 3;
    }

    return 0;
}

template <class Map>
void
mergeHistogram(Map &destination, const Map &source)
{
    for (const auto &[key, count] : source) {
        destination[key] += count;
    }
}

void
writeHistogram(std::ostream &stream, const char *name,
               const std::map<uint64_t, uint64_t> &histogram)
{
    stream << "[" << name << "]\n";
    stream << "key count\n";
    for (const auto &[key, count] : histogram) {
        stream << key << " " << count << "\n";
    }
    stream << "\n";
}

} // namespace

void
DecoupledBPUWithBTB::mergeBpStatSequence(
    BpStatSequence &destination, const BpStatSequence &source)
{
    mergeHistogram(destination.allBranchDistance, source.allBranchDistance);
    mergeHistogram(destination.takenBranchDistance, source.takenBranchDistance);
    mergeHistogram(destination.notTakenBranchDistance,
                   source.notTakenBranchDistance);
    mergeHistogram(destination.notTakenBetweenTaken,
                   source.notTakenBetweenTaken);
}

void
DecoupledBPUWithBTB::BpStatSequence::recordNonBranch()
{
    if (seenAny) {
        ++nonBranchesSinceAny;
    }
    if (seenTaken) {
        ++nonBranchesSinceTaken;
    }
    if (seenNotTaken) {
        ++nonBranchesSinceNotTaken;
    }
}

void
DecoupledBPUWithBTB::BpStatSequence::recordBranch(bool taken)
{
    if (seenAny) {
        ++allBranchDistance[nonBranchesSinceAny];
    }
    seenAny = true;
    nonBranchesSinceAny = 0;

    if (taken) {
        if (seenTaken) {
            ++takenBranchDistance[nonBranchesSinceTaken];
            ++notTakenBetweenTaken[notTakenSinceTaken];
        }
        seenTaken = true;
        nonBranchesSinceTaken = 0;
        notTakenSinceTaken = 0;
    } else {
        if (seenNotTaken) {
            ++notTakenBranchDistance[nonBranchesSinceNotTaken];
        }
        seenNotTaken = true;
        nonBranchesSinceNotTaken = 0;
        if (seenTaken) {
            ++notTakenSinceTaken;
        }
    }
}

void
DecoupledBPUWithBTB::finalizeBpStatPredictionBlock(
    FetchTargetId target_id, ThreadID tid)
{
    auto &prediction_blocks = bpStat->predictBlocks[tid];
    if (const auto it = prediction_blocks.find(target_id);
        it != prediction_blocks.end()) {
        const auto &block = it->second;
        ++bpStat->predictBlockBranchCount[block.branches];
        ++bpStat->predictBlockTakenBranchCount[block.takenBranches];
        ++bpStat->predictBlockNotTakenBranchCount[block.notTakenBranches];
        ++bpStat->predictBlockFirstTakenPresence[
            block.hasFirstTaken ? 1 : 0];
        if (block.hasFirstTaken) {
            ++bpStat->predictBlockFirstTakenInstructionDistance[
                block.firstTakenInstructionDistance];
            ++bpStat->predictBlockFirstTakenByteDistance[
                block.firstTakenByteDistance];
        }
        mergeBpStatSequence(bpStat->predictionBlockSequences, block.sequence);
        prediction_blocks.erase(it);
    }

    auto &fetch_blocks = bpStat->fetchBlocks[tid];
    if (const auto targets_it = fetch_blocks.find(target_id);
        targets_it != fetch_blocks.end()) {
        for (const auto &entry : targets_it->second) {
            const auto &block = entry.second;
            ++bpStat->fetchBlockBranchCount[block.branches];
            ++bpStat->fetchBlockTakenBranchCount[block.takenBranches];
            ++bpStat->fetchBlockNotTakenBranchCount[block.notTakenBranches];
            mergeBpStatSequence(bpStat->fetchBlockSequences, block.sequence);
        }
        fetch_blocks.erase(targets_it);
    }
}

void
DecoupledBPUWithBTB::flushBpStatBlocks()
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        auto &prediction_blocks = bpStat->predictBlocks[tid];
        while (!prediction_blocks.empty()) {
            finalizeBpStatPredictionBlock(prediction_blocks.begin()->first, tid);
        }

        auto &fetch_blocks = bpStat->fetchBlocks[tid];
        while (!fetch_blocks.empty()) {
            const auto target_id = fetch_blocks.begin()->first;
            finalizeBpStatPredictionBlock(target_id, tid);
        }
    }
}

void
DecoupledBPUWithBTB::recordBpStatCommittedInst(const DynInstPtr &inst)
{
    const ThreadID tid = inst->threadNumber;
    const FetchTargetId target_id = inst->ftqId;
    auto &prediction_block = bpStat->predictBlocks[tid][target_id];
    const Addr inst_pc = inst->pcState().instAddr();
    const Addr fetch_block_addr =
        inst_pc & ~mask(floorLog2(FetchBlockBytes));
    auto &fetch_block = bpStat->fetchBlocks[tid][target_id][fetch_block_addr];

    const bool is_branch = !inst->isNonSpeculative() && inst->isControl();
    if (is_branch) {
        const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
        const bool taken = rv_pc.branching() || inst->isUncondCtrl();
        auto &branch_pairs = bpStat->globalBranchPairs[tid];
        const bool adjacent_tt = branch_pairs.seenPrevious &&
                                 branch_pairs.previousTaken && taken;
        if (branch_pairs.seenPrevious) {
            const uint64_t pair_key =
                (branch_pairs.previousTaken ? 2 : 0) | (taken ? 1 : 0);
            ++branch_pairs.directionPairs[pair_key];
        }

        if (taken && inst->isIndirectCtrl()) {
            auto &target = branch_pairs.indirectTargets[inst_pc];
            const Addr actual_target = rv_pc.npc();
            if (!target.seenTarget) {
                target.seenTarget = true;
                target.firstTarget = actual_target;
            } else if (target.firstTarget != actual_target) {
                target.multipleTargets = true;
            }
            if (adjacent_tt) {
                ++target.adjacentTTPairs;
            }
        } else if (adjacent_tt && inst->isDirectCtrl()) {
            ++branch_pairs.directStableSecondTarget;
        }

        branch_pairs.seenPrevious = true;
        branch_pairs.previousTaken = taken;
        bpStat->global[tid].recordBranch(taken);
        prediction_block.sequence.recordBranch(taken);
        fetch_block.sequence.recordBranch(taken);
        ++prediction_block.branches;
        ++fetch_block.branches;
        if (taken) {
            if (!prediction_block.hasFirstTaken) {
                const Addr start_pc = ftq.get(target_id, tid).startPC;
                panic_if(inst_pc < start_pc,
                         "Committed taken branch PC %#x precedes predict "
                         "window start PC %#x for FTQ entry %u",
                         inst_pc, start_pc, target_id);
                prediction_block.hasFirstTaken = true;
                prediction_block.firstTakenInstructionDistance =
                    prediction_block.architecturalInstructions;
                prediction_block.firstTakenByteDistance = inst_pc - start_pc;
            }
            ++prediction_block.takenBranches;
            ++fetch_block.takenBranches;
        } else {
            ++prediction_block.notTakenBranches;
            ++fetch_block.notTakenBranches;
        }
    } else {
        bpStat->global[tid].recordNonBranch();
        prediction_block.sequence.recordNonBranch();
        fetch_block.sequence.recordNonBranch();
    }
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        ++prediction_block.architecturalInstructions;
        ++fetch_block.architecturalInstructions;
    }
    ++prediction_block.instructions;
    ++fetch_block.instructions;
}

void
DecoupledBPUWithBTB::sampleBpStatFtqOccupancy()
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        ++bpStat->ftqOccupancy[tid][ftq.size(tid)];
    }
}

void
DecoupledBPUWithBTB::dumpBpStat()
{
    flushBpStatBlocks();

    BpStatSequence global;
    for (const auto &sequence : bpStat->global) {
        mergeBpStatSequence(global, sequence);
    }

    std::map<uint64_t, uint64_t> global_direction_pairs = {
        {0, 0}, {1, 0}, {2, 0}, {3, 0}
    };
    uint64_t stable_second_target = 0;
    for (const auto &branch_pairs : bpStat->globalBranchPairs) {
        for (size_t key = 0;
             key < branch_pairs.directionPairs.size(); ++key) {
            global_direction_pairs[key] += branch_pairs.directionPairs[key];
        }
        stable_second_target += branch_pairs.directStableSecondTarget;
        for (const auto &entry : branch_pairs.indirectTargets) {
            const auto &target = entry.second;
            if (target.seenTarget && !target.multipleTargets) {
                stable_second_target += target.adjacentTTPairs;
            }
        }
    }
    const std::map<uint64_t, uint64_t> global_stable_second_target = {
        {1, stable_second_target}
    };
    auto handle = simout.create("bp-stat.txt", false, true);
    auto &stream = *handle->stream();
    stream << "# bp-stat: committed dynamic instruction distributions\n"
           << "# predict_block: one committed DecoupledBPU FetchTarget "
              "prediction window\n"
           << "# fetch_block: one committed 32B-aligned BTB FBlock within "
              "a prediction window\n"
           << "# branch_distance: committed non-branch instructions between "
              "adjacent branches in the selected class\n"
           << "# not_taken_between_taken: not-taken branches strictly between "
              "adjacent taken branches\n"
           << "# first_taken_instruction_distance: committed architectural "
              "instructions strictly before the first actual taken branch\n"
           << "# first_taken_byte_distance: first actual taken branch PC "
              "minus the prediction-window start PC\n"
           << "# first_taken_presence: 0 means absent, 1 means present\n"
           << "# adjacent_branch_direction_pair: consecutive committed "
              "branches in one thread; 0=NN, 1=NT, 2=TN, 3=TT\n"
           << "# adjacent_tt_stable_second_target: key 1 counts TT pairs "
              "whose second branch is direct or an observed single-target "
              "indirect branch\n\n";
    writeHistogram(stream, "predict_block_branch_count",
                   bpStat->predictBlockBranchCount);
    writeHistogram(stream, "predict_block_taken_branch_count",
                   bpStat->predictBlockTakenBranchCount);
    writeHistogram(stream, "predict_block_not_taken_branch_count",
                   bpStat->predictBlockNotTakenBranchCount);
    writeHistogram(stream, "predict_block_first_taken_presence",
                   bpStat->predictBlockFirstTakenPresence);
    writeHistogram(stream, "predict_block_first_taken_instruction_distance",
                   bpStat->predictBlockFirstTakenInstructionDistance);
    writeHistogram(stream, "predict_block_first_taken_byte_distance",
                   bpStat->predictBlockFirstTakenByteDistance);
    writeHistogram(stream, "fetch_block_branch_count",
                   bpStat->fetchBlockBranchCount);
    writeHistogram(stream, "fetch_block_taken_branch_count",
                   bpStat->fetchBlockTakenBranchCount);
    writeHistogram(stream, "fetch_block_not_taken_branch_count",
                   bpStat->fetchBlockNotTakenBranchCount);
    writeHistogram(stream, "global_all_branch_distance",
                   global.allBranchDistance);
    writeHistogram(stream, "global_taken_branch_distance",
                   global.takenBranchDistance);
    writeHistogram(stream, "global_not_taken_branch_distance",
                   global.notTakenBranchDistance);
    writeHistogram(stream, "global_not_taken_between_taken",
                   global.notTakenBetweenTaken);

    writeHistogram(stream, "global_adjacent_branch_direction_pair",
                   global_direction_pairs);
    writeHistogram(stream, "global_adjacent_tt_stable_second_target",
                   global_stable_second_target);
    writeHistogram(stream, "predict_block_all_branch_distance",
                   bpStat->predictionBlockSequences.allBranchDistance);
    writeHistogram(stream, "predict_block_taken_branch_distance",
                   bpStat->predictionBlockSequences.takenBranchDistance);
    writeHistogram(stream, "predict_block_not_taken_branch_distance",
                   bpStat->predictionBlockSequences.notTakenBranchDistance);
    writeHistogram(stream, "predict_block_not_taken_between_taken",
                   bpStat->predictionBlockSequences.notTakenBetweenTaken);
    writeHistogram(stream, "fetch_block_all_branch_distance",
                   bpStat->fetchBlockSequences.allBranchDistance);
    writeHistogram(stream, "fetch_block_taken_branch_distance",
                   bpStat->fetchBlockSequences.takenBranchDistance);
    writeHistogram(stream, "fetch_block_not_taken_branch_distance",
                   bpStat->fetchBlockSequences.notTakenBranchDistance);
    writeHistogram(stream, "fetch_block_not_taken_between_taken",
                   bpStat->fetchBlockSequences.notTakenBetweenTaken);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        writeHistogram(stream,
                       ("ftq_occupancy_entries_tid" +
                        std::to_string(tid)).c_str(),
                       bpStat->ftqOccupancy[tid]);
    }

    simout.close(handle);
}

void
DecoupledBPUWithBTB::initDB()
{

    bpdb.init_db();
    enableBranchTrace = checkGivenSwitch(bpDBSwitches, std::string("basic"));
    if (enableBranchTrace) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("fsqId", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("controlType", UINT64),
            std::make_pair("taken", UINT64),
            std::make_pair("mispred", UINT64),
            std::make_pair("fallThruPC", UINT64),
            std::make_pair("source", UINT64),
            std::make_pair("target", UINT64)
        };
        bptrace = bpdb.addAndGetTrace("BPTRACE", fields_vec);
        bptrace->init_table();
        removeGivenSwitch(bpDBSwitches, std::string("basic"));
        someDBenabled = true;
    }

    enablePredFSQTrace = checkGivenSwitch(bpDBSwitches, std::string("predfsq"));
    if (enablePredFSQTrace) {
        // Initialize prediction trace manager for recording predictions
        std::vector<std::pair<std::string, DataType>> pred_fields_vec = {
            std::make_pair("fsqId", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("predEndPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("target", UINT64),
            std::make_pair("predSource", UINT64),
            std::make_pair("btbHit", UINT64)
        };
        predTraceManager = bpdb.addAndGetTrace("PREDTRACE", pred_fields_vec);
        predTraceManager->init_table();
        removeGivenSwitch(bpDBSwitches, std::string("predfsq"));
        someDBenabled = true;
    }
}

void
DecoupledBPUWithBTB::dumpStats()
{
    // Helper function: create output file and write header
    auto createOutputFile = [](const std::string& filename, const std::string& header) {
        auto handle = simout.create(filename, false, true);
        *handle->stream() << header << std::endl;
        return handle;
    };

    // Generic sort function by key
    auto sortByKey = [](auto& data, auto keyFn) {
        std::sort(data.begin(), data.end(),
            [&keyFn](const auto& a, const auto& b) {
                return keyFn(a) > keyFn(b);
            });
    };

    // 1. Output top mispredictions
    auto outFile = createOutputFile("topMisPredicts.csv", "startPC,control_pc,count");
    // topMisPredPC: Vector of mispredict records (startPC, controlPC) -> count
    // startPC: Starting address of fetch block
    // controlPC: Address of branch instruction
    // count: Number of mispredictions
    std::vector<std::pair<std::pair<Addr, Addr>, int>> topMisPredPC(
        topMispredicts.begin(), topMispredicts.end());

    sortByKey(topMisPredPC, [](const auto& entry) { return entry.second; });
    for (const auto& entry : topMisPredPC) {
        *outFile->stream() << std::hex << entry.first.first << ","
            << entry.first.second << ","
            << std::dec << entry.second << std::endl;
    }
    simout.close(outFile);

    // 2. Output mispredictions by branch
    outFile = createOutputFile("topMispredictsByBranch.csv",
                "pc,type,mispredicts,total,misPermil,dirMiss,tgtMiss,noPredMiss");

    // topMisPredPCByBranch: Detailed misprediction records per branch
    std::vector<std::tuple<Addr, int, int, int, double, int, int, int>> topMisPredPCByBranch;
    for (const auto &it : topMispredictsByBranch) {
        const auto &stats = it.second;
        topMisPredPCByBranch.push_back(std::make_tuple(
            stats.pc, stats.branchType,
            stats.mispredCount, stats.totalCount,
            stats.getMispredRate(),
            stats.dirWrongCount, stats.targetWrongCount, stats.noPredCount));
    }

    sortByKey(topMisPredPCByBranch,
        [](const auto& entry) {
            return std::get<2>(entry);  // Sort by mispredCount
        });

    for (const auto& entry : topMisPredPCByBranch) {
        *outFile->stream()
            << std::hex << std::get<0>(entry) << ","
            << std::dec << std::get<1>(entry) << ","
            << std::get<2>(entry) << ","
            << std::get<3>(entry) << ","
            << std::get<4>(entry) << ","
            << std::get<5>(entry) << ","
            << std::get<6>(entry) << ","
            << std::get<7>(entry) << std::endl;
    }
    simout.close(outFile);

    // 3. Sort branches by misrate (per-mille)
    outFile = createOutputFile("topMisrateByBranch.csv",
                "pc,type,mispredicts,total,misPermil,dirMiss,tgtMiss,noPredMiss");

    // Reuse previous data, but sort by misrate
    int mispCntThres = 100;
    sortByKey(topMisPredPCByBranch, [](const auto& entry) { return std::get<4>(entry); });
    for (const auto& entry : topMisPredPCByBranch) {
        if (std::get<3>(entry) < mispCntThres) continue;

        *outFile->stream() << std::hex << std::get<0>(entry) << std::dec
            << "," << std::get<1>(entry)
            << "," << std::get<2>(entry)
            << "," << std::get<3>(entry)
            << "," << (int)std::get<4>(entry)
            << "," << (int)std::get<5>(entry)
            << "," << (int)std::get<6>(entry)
            << "," << (int)std::get<7>(entry) << std::endl;
    }
    simout.close(outFile);

    // Create CSV header for topN tables
    auto createTopNHeader = [](std::ostream& out, int outputTopN, const std::string& prefix) {
        out << prefix;
        for (int i = 0; i < outputTopN; i++) {
            out << ",topMispPC_" << i
                << ",type_" << i
                << ",misCnt_" << i;
        }
        out << std::endl;
    };

    // Output phase-classified mispredictions
    int outputTopN = 5;

    // 4. Phase-based mispredictions
    auto processPhaseData = [&](const std::string& filename,
                           const auto& dataByPhase,
                           const auto& takenBranches) {
        outFile = simout.create(filename, false, true);
        auto& out = *outFile->stream();

        // Write header
        createTopNHeader(out, outputTopN,
                        filename.find("Sub") != std::string::npos ?
                        "subPhaseID,numBranches,numEverTakenBranches,totalMispredicts" :
                        "phaseID,numBranches,numEverTakenBranches,totalMispredicts");

        int phaseID = 0;
        for (const auto& phaseData : dataByPhase) {
            int numStaticBranches = phaseData.size();
            int numEverTakenStaticBranches = takenBranches[phaseID].size();

            // Copy data and calculate total mispredictions
            std::vector<std::pair<BranchKey, BranchStats>> phaseRecords;
            for (const auto& record : phaseData) {
                phaseRecords.push_back(record);
            }

            // Calculate total mispredicts
            int totalMispredicts = 0;
            for (const auto& rec : phaseRecords) {
                totalMispredicts += rec.second.mispredCount;
            }

            // Output phase basic info
            out << phaseID << "," << numStaticBranches << ","
                << numEverTakenStaticBranches << "," << totalMispredicts;

            // Sort by misprediction count
            std::sort(phaseRecords.begin(), phaseRecords.end(),
                [](const auto& a, const auto& b) {
                    return a.second.mispredCount > b.second.mispredCount;
                });

            // Output top-N
            for (int i = 0; i < outputTopN && i < phaseRecords.size(); i++) {
                const auto& stats = phaseRecords[i].second;
                out << "," << std::hex << stats.pc // pc
                    << "," << std::dec << stats.branchType // type
                    << "," << stats.mispredCount; // count
            }
            out << std::dec << std::endl;
            phaseID++;
        }
        simout.close(outFile);
    };

    processPhaseData("topMispredictByPhase.csv",
                     topMispredictsByBranchByPhase,
                     takenBranchesByPhase);

    processPhaseData("topMispredictBySubPhase.csv",
                     topMispredictsByBranchBySubPhase,
                     takenBranchesBySubPhase);

    // 5. Output history misprediction data
    outFile = createOutputFile("topMisPredictHist.csv", "Hist,count");
    // Vector of (history pattern, count) pairs
    std::vector<std::pair<uint64_t, uint64_t>>
        topMisPredHistVec(topMispredHist.begin(), topMispredHist.end());

    sortByKey(topMisPredHistVec, [](const auto& entry) { return entry.second; });
    for (const auto& entry : topMisPredHistVec) {
        *outFile->stream() << std::hex << entry.first << ","
            << std::dec << entry.second << std::endl;
    }
    simout.close(outFile);

    // 6. Output indirect mispredictions
    outFile = createOutputFile("misPredIndirectStream.csv", "count,address");
    // Vector of (address, count) pairs for indirect branches
    std::vector<std::pair<Addr, unsigned>>
        indirectVec(topMispredIndirect.begin(), topMispredIndirect.end());

    sortByKey(indirectVec, [](const auto& entry) { return entry.second; });
    for (const auto& entry : indirectVec) {
        *outFile->stream() << std::oct << entry.second << ","
            << std::hex << entry.first << std::endl;
    }
    simout.close(outFile);

    // Process FSQ distribution data
    auto processFsqDistribution = [&](const std::string& filename,
                                  const auto& distByPhase) {
        outFile = simout.create(filename, false, true);
        auto& out = *outFile->stream();

        // Write header
        out << "phaseID";
        for (int i = 0; i <= maxInstsNum; i++) {
            out << "," << i;
        }
        out << ",average" << std::endl;

        // Write data for each phase
        int phaseID = 0;
        for (const auto& dist : distByPhase) {
            out << phaseID;

            int numFsqEntries = 0;
            for (int i = 0; i <= maxInstsNum; i++) {
                numFsqEntries += dist[i];
            }

            for (int i = 0; i <= maxInstsNum; i++) {
                out << "," << dist[i];
            }

            out << "," << (double)phaseSizeByInst / (double)numFsqEntries << std::endl;
            phaseID++;
        }

        simout.close(outFile);
    };

    // 7-8. Output FSQ distribution data
    processFsqDistribution("fsqEntryCommittedInstNumDistsByPhase.csv",
                          fsqEntryNumCommittedInstDistByPhase);

    processFsqDistribution("fsqEntryFetchedInstNumDistsByPhase.csv",
                          fsqEntryNumFetchedInstDistByPhase);

    // 9. Output BTB entries
    int outputTopNEntries = 1;
    std::stringstream headerSS;
    headerSS << "phaseID,numBTBEntries";
    for (int i = 0; i <= outputTopNEntries; i++) {
        headerSS << ",entry_" << i << "_pc,entry_" << i << "_type";
    }
    outFile = createOutputFile("btbEntriesByPhase.csv", headerSS.str());

    int phaseID = 0;
    for (auto& phase : BTBEntriesByPhase) {
        auto& out = *outFile->stream();
        out << std::dec << phaseID << "," << phase.size();

        // Vector of (PC, BTBEntry, count) tuples
        std::vector<std::tuple<Addr, BTBEntry, int>> btbEntries;
        for (auto& entry : phase) {
            btbEntries.push_back(std::make_tuple(
                entry.first, entry.second.first, entry.second.second));
        }

        std::sort(btbEntries.begin(), btbEntries.end(),
            [](const auto &a, const auto &b) {
                 return std::get<2>(a) > std::get<2>(b);
            });

        for (int i = 0; i <= outputTopNEntries && i < btbEntries.size(); i++) {
            const auto &entry = btbEntries[i];
            out << "," << std::hex << std::get<0>(entry);
            // BTBEntry.getType() is not a const method, need to create a copy
            BTBEntry btbEntry = std::get<1>(entry);
            out << "," << std::dec << btbEntry.getType();
        }

        out << std::endl;
        phaseID++;
    }
    simout.close(outFile);

    // Save the database
    if (someDBenabled) {
        bpdb.save_db(simout.resolve("bp.db").c_str());
    }
}

DecoupledBPUWithBTB::BpTrace::BpTrace(uint64_t fsqId, FetchTarget &target, const DynInstPtr &inst, bool mispred)
{
    _tick = curTick();
    Addr pc = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr targetpc = rv_pc.npc();
    Addr fallThru = rv_pc.getFallThruPC();
    BranchInfo info(pc, targetpc, inst->staticInst, fallThru-pc);
    set(fsqId, target.startPC, pc, info.getType(), inst->branching(), mispred, fallThru, target.predSource, targetpc);
    // for (auto it = _uint64_data.begin(); it != _uint64_data.end(); it++) {
    //     printf("%s: %ld\n", it->first.c_str(), it->second);
    // }
}


namespace {

constexpr std::array<const char*, DecoupledBPUWithBTB::NumBranchClasses>
    BranchClassLabels = {
        "cond_branch",
        "direct_call",
        "indirect_call",
        "return",
        "direct_jump",
        "indirect_jump",
        "unknown"
    };

template <typename InstPtr>
DecoupledBPUWithBTB::BranchClass
classifyBranchImpl(const InstPtr &inst)
{
    using BranchClass = DecoupledBPUWithBTB::BranchClass;

    if (!inst || !inst->isControl()) {
        return BranchClass::Unknown;
    }

    if (inst->isReturn()) {
        return BranchClass::Return;
    }

    if (inst->isCall()) {
        return inst->isIndirectCtrl() ? BranchClass::IndirectCall
                                      : BranchClass::DirectCall;
    }

    if (inst->isCondCtrl()) {
        return BranchClass::CondBranch;
    }

    if (inst->isIndirectCtrl()) {
        return BranchClass::IndirectJump;
    }

    if (inst->isDirectCtrl() || inst->isUncondCtrl()) {
        return BranchClass::DirectJump;
    }

    return BranchClass::Unknown;
}

} // anonymous namespace

DecoupledBPUWithBTB::DBPBTBStats::DBPBTBStats(
    statistics::Group* parent, unsigned numStages, unsigned fsqSize, unsigned maxInstsNum):
    statistics::Group(parent),
    ADD_STAT(condNum, statistics::units::Count::get(), "the number of cond branches"),
    ADD_STAT(uncondNum, statistics::units::Count::get(), "the number of uncond branches"),
    ADD_STAT(returnNum, statistics::units::Count::get(), "the number of return branches"),
    ADD_STAT(otherNum, statistics::units::Count::get(), "the number of other branches"),
    ADD_STAT(condMiss, statistics::units::Count::get(), "the number of cond branch misses"),
    ADD_STAT(uncondMiss, statistics::units::Count::get(), "the number of uncond branch misses"),
    ADD_STAT(returnMiss, statistics::units::Count::get(), "the number of return branch misses"),
    ADD_STAT(otherMiss, statistics::units::Count::get(), "the number of other branch misses"),
    ADD_STAT(branchClassCounts, statistics::units::Count::get(), "branch counts by fine-grained class"),
    ADD_STAT(branchClassMisses, statistics::units::Count::get(), "branch mispredictions by fine-grained class"),
    ADD_STAT(branchClassCountsTotal, statistics::units::Count::get(), "total number of classified branches"),
    ADD_STAT(controlSquashByClass, statistics::units::Count::get(), "commit/resolve-path squashes by branch class"),
    ADD_STAT(staticBranchNum, statistics::units::Count::get(), "the number of all (different) static branches"),
    ADD_STAT(staticBranchNumEverTaken, statistics::units::Count::get(), "the number of all (different) static branches that are once taken"),
    ADD_STAT(predsOfEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for final pred"),
    ADD_STAT(overrideBubbleNum,  statistics::units::Count::get(), "the number of override bubbles"),
    ADD_STAT(overrideCount, statistics::units::Count::get(), "the number of overrides"),
    ADD_STAT(commitPredsFromEachStage, statistics::units::Count::get(),
    "the number of preds of each stage that account for a committed target"),
    ADD_STAT(commitOverrideBubbleNum, statistics::units::Count::get(),
    "the number of override bubbles, on the commit path"),
    ADD_STAT(commitOverrideCount, statistics::units::Count::get(), "the number of overrides, on the commit path"),
    ADD_STAT(overrideFallThruMismatch, statistics::units::Count::get(),
    "Number of overrides due to validity mismatches, on commit path"),
    ADD_STAT(overrideControlAddrMismatch, statistics::units::Count::get(),
    "Number of overrides due to control address mismatches, on commit path"),
    ADD_STAT(overrideTargetMismatch, statistics::units::Count::get(),
    "Number of overrides due to target mismatches, on commit path"),
    ADD_STAT(fsqEntryDist, statistics::units::Count::get(), "the distribution of number of entries in fsq"),
    ADD_STAT(fsqEntryEnqueued, statistics::units::Count::get(), "the number of fsq entries enqueued"),
    ADD_STAT(fsqEntryCommitted, statistics::units::Count::get(), "the number of fsq entries committed at last"),
    ADD_STAT(controlSquashFromDecode, statistics::units::Count::get(), "the number of control squashes in bpu from decode"),
    ADD_STAT(controlSquashFromCommit, statistics::units::Count::get(), "the number of control squashes in bpu from commit"),
    ADD_STAT(nonControlSquash, statistics::units::Count::get(), "the number of non-control squashes in bpu"),
    ADD_STAT(trapSquash, statistics::units::Count::get(), "the number of trap squashes in bpu"),
    ADD_STAT(ftqNotValid, statistics::units::Count::get(), "fetch needs ftq req but ftq not valid"),
    ADD_STAT(fsqNotValid, statistics::units::Count::get(), "ftq needs fsq req but fsq not valid"),
    ADD_STAT(fsqFullCannotEnq, statistics::units::Count::get(), "bpu has req but fsq full cannot enqueue"),
    ADD_STAT(ftqFullCannotEnq, statistics::units::Count::get(), "fsq has entry but ftq full cannot enqueue"),
    ADD_STAT(fsqFullFetchHungry, statistics::units::Count::get(), "fetch hungry when fsq full and bpu cannot enqueue"),
    ADD_STAT(fsqEmpty, statistics::units::Count::get(), "fsq is empty"),
    ADD_STAT(commitFsqEntryHasInsts, statistics::units::Count::get(), "number of insts that commit fsq entries have"),
    ADD_STAT(commitFsqEntryFetchedInsts, statistics::units::Count::get(), "number of insts that commit fsq entries fetched"),
    ADD_STAT(commitFsqEntryOnlyHasOneJump, statistics::units::Count::get(), "number of fsq entries with only one instruction (jump)"),
    ADD_STAT(btbHit, statistics::units::Count::get(), "btb hits (in predict block)"),
    ADD_STAT(btbMiss, statistics::units::Count::get(), "btb misses (in predict block)"),
    ADD_STAT(btbEntriesWithDifferentStart, statistics::units::Count::get(), "number of btb entries with different start PC"),
    ADD_STAT(btbEntriesWithOnlyOneJump, statistics::units::Count::get(), "number of btb entries with different start PC starting with a jump"),
    ADD_STAT(predFalseHit, statistics::units::Count::get(), "false hit detected at pred"),
    ADD_STAT(commitFalseHit, statistics::units::Count::get(), "false hit detected at commit"),
    ADD_STAT(predictionBlockedForUpdate, statistics::units::Count::get(), "prediction blocked for update priority"),
    ADD_STAT(scheduleIneligibleThreadSkips, statistics::units::Count::get(),
    "round-robin BPU scheduler skipped an active thread that could not start a new prediction"),
    ADD_STAT(scheduleNoEligibleThread, statistics::units::Count::get(),
    "round-robin BPU scheduler found no thread eligible to start a new prediction"),
    ADD_STAT(redirectPendingPredictionSkips, statistics::units::Count::get(),
    "round-robin BPU scheduler skipped prediction because a redirect was pending"),
    ADD_STAT(s1PredWrongFallthrough, statistics::units::Count::get(), "S1pred wrong full throughs"),
    ADD_STAT(s1PredWrongUbtb, statistics::units::Count::get(),"S1pred wrong using ubtb "),
    ADD_STAT(s1PredWrongAbtb, statistics::units::Count::get(), "S1pred wrong using abtb "),
    ADD_STAT(s1PredWrongBySourceAndReason, statistics::units::Count::get(),
             "committed S1 mispredictions bucketed by S1 source and override reason"),
    ADD_STAT(s3PredWrongMbtb, statistics::units::Count::get(), "S3pred wrong blame mbtb "),
    ADD_STAT(s3PredWrongTage, statistics::units::Count::get(), "S3pred wrong blame tage "),
    ADD_STAT(s3PredWrongIttage, statistics::units::Count::get(), "S3pred wrong blame ittage "),
    ADD_STAT(s3PredWrongRas, statistics::units::Count::get(), "S3pred wrong blame ras ")

{
    predsOfEachStage.init(numStages);
    commitPredsFromEachStage.init(numStages+1);
    commitOverrideBubbleNum = commitPredsFromEachStage[1] + 2 * commitPredsFromEachStage[2] ;
    commitOverrideCount = commitPredsFromEachStage[1] + commitPredsFromEachStage[2];
    fsqEntryDist.init(0, fsqSize, 20).flags(statistics::total);
    commitFsqEntryHasInsts.init(0, maxInstsNum >> 1, 1);
    commitFsqEntryFetchedInsts.init(0, maxInstsNum >> 1, 1);
    s1PredWrongBySourceAndReason.init(NumS1SourceBuckets, NumOverrideReasonBuckets);
    branchClassCounts.init(NumBranchClasses);
    branchClassMisses.init(NumBranchClasses);
    controlSquashByClass.init(NumBranchClasses);
    for (int i = 0; i < NumS1SourceBuckets; ++i) {
        s1PredWrongBySourceAndReason.subname(i, S1SourceLabels[i]);
    }
    for (int i = 0; i < NumOverrideReasonBuckets; ++i) {
        s1PredWrongBySourceAndReason.ysubname(i, OverrideReasonLabels[i]);
    }
    for (size_t i = 0; i < NumBranchClasses; ++i) {
        branchClassCounts.subname(i, BranchClassLabels[i]);
        branchClassMisses.subname(i, BranchClassLabels[i]);
        controlSquashByClass.subname(i, BranchClassLabels[i]);
    }
}

void DecoupledBPUWithBTB::overrideStats(OverrideReason overrideReason)
{

        // Track specific override reasons for statistics
        switch (overrideReason) {
            case OverrideReason::FALL_THRU:
                dbpBtbStats.overrideFallThruMismatch++;
                break;
            case OverrideReason::CONTROL_ADDR:
                dbpBtbStats.overrideControlAddrMismatch++;
                break;
            case OverrideReason::TARGET:
                dbpBtbStats.overrideTargetMismatch++;
                break;
            default:
                break;
        }
}

void
DecoupledBPUWithBTB::processFetchDistributions(std::vector<int> &currentPhaseCommittedDist,
                                              std::vector<int> &currentPhaseFetchedDist)
{
    // Initialize distributions with zeros
    currentPhaseCommittedDist.resize(maxInstsNum+1, 0);
    currentPhaseFetchedDist.resize(maxInstsNum+1, 0);

    // Calculate the difference between current and last phase values
    for (int i = 0; i <= maxInstsNum; i++) {
        currentPhaseCommittedDist[i] = commitFsqEntryHasInstsVector[i] -
                                     lastPhaseFsqEntryNumCommittedInstDist[i];
        lastPhaseFsqEntryNumCommittedInstDist[i] = commitFsqEntryHasInstsVector[i];

        currentPhaseFetchedDist[i] = commitFsqEntryFetchedInstsVector[i] -
                                   lastPhaseFsqEntryNumFetchedInstDist[i];
        lastPhaseFsqEntryNumFetchedInstDist[i] = commitFsqEntryFetchedInstsVector[i];
    }
}

std::unordered_map<Addr, std::pair<BTBEntry, int>>
DecoupledBPUWithBTB::processBTBEntries()
{
    std::unordered_map<Addr, std::pair<BTBEntry, int>> currentPhaseBTBEntries;

    // Process each BTB entry
    for (auto &it : totalBTBEntries) {
        auto &entry = it.second.first;
        auto visit_cnt = it.second.second;

        // Check if this entry was already present in last phase
        auto last_it = lastPhaseBTBEntries.find(it.first);
        if (last_it != lastPhaseBTBEntries.end()) {
            visit_cnt -= last_it->second.second;
        }

        // Only add entries with new visits in this phase
        if (visit_cnt > 0) {
            currentPhaseBTBEntries[it.first] = std::make_pair(entry, visit_cnt);
        }
    }

    // Update last phase BTB entries for next time
    lastPhaseBTBEntries = totalBTBEntries;

    return currentPhaseBTBEntries;
}

bool
DecoupledBPUWithBTB::processPhase(bool isSubPhase, int phaseID, int &phaseToDump,
                                BranchStatsMap &lastPhaseStats,
                                std::vector<BranchStatsMap> &phaseStatsList,
                                std::unordered_map<Addr, int> &currentPhaseBranches,
                                std::vector<std::unordered_map<Addr, int>> &phaseBranchesList)
{
    // Check if this phase should be processed
    if (phaseToDump > phaseID) {
        return false;
    }

    // Debug output
    DPRINTF(Profiling, "dump %s phase %d\n",
            isSubPhase ? "sub" : "main", phaseToDump);

    // Create map for current phase statistics
    BranchStatsMap currentPhaseStats;

    // Process each branch in the global statistics
    for (auto &it : topMispredictsByBranch) {
        const auto &key = it.first;
        const auto &stats = it.second;

        // Find stats from last phase
        auto lastIt = lastPhaseStats.find(key);

        // If branch exists in last phase, calculate difference
        if (lastIt != lastPhaseStats.end()) {
            const auto &lastStats = lastIt->second;

            // Skip branches with no new executions
            if (stats.totalCount <= lastStats.totalCount) {
                continue;
            }

            // Create stats for current phase (delta from last phase)
            BranchStats phaseStats(stats.pc, stats.branchType);
            phaseStats.totalCount = stats.totalCount - lastStats.totalCount;
            phaseStats.mispredCount = stats.mispredCount - lastStats.mispredCount;
            phaseStats.dirWrongCount = stats.dirWrongCount - lastStats.dirWrongCount;
            phaseStats.targetWrongCount = stats.targetWrongCount - lastStats.targetWrongCount;
            phaseStats.noPredCount = stats.noPredCount - lastStats.noPredCount;

            currentPhaseStats[key] = phaseStats;
        } else {
            // This is a new branch in this phase
            currentPhaseStats[key] = stats;
        }
    }

    // Store the processed data
    lastPhaseStats = topMispredictsByBranch;
    phaseStatsList.push_back(currentPhaseStats);

    // Handle taken branches map
    phaseBranchesList.push_back(currentPhaseBranches);
    currentPhaseBranches.clear();

    // Increment phase counter for next time
    phaseToDump++;

    return true;
}

void
DecoupledBPUWithBTB::dumpFsq(const char *when)
{
    // DPRINTF(DecoupleBPProbe, "dumping fsq entries %s...\n", when);
    // for (size_t i = 0; i < fetchTargetQueue.size(); ++i) {
    //     DPRINTFR(DecoupleBPProbe, "TargetID %lu, ",
    //              static_cast<uint64_t>(fetchTargetBaseId + i));
    //     printTarget(fetchTargetQueue[i]);
    // }
}

DecoupledBPUWithBTB::BranchClass
DecoupledBPUWithBTB::classifyBranch(const DynInstPtr &inst) const
{
    return classifyBranchImpl(inst);
}

DecoupledBPUWithBTB::BranchClass
DecoupledBPUWithBTB::classifyBranch(const StaticInstPtr &inst) const
{
    return classifyBranchImpl(inst);
}

const char *
DecoupledBPUWithBTB::branchClassName(BranchClass cls)
{
    auto idx = static_cast<size_t>(cls);
    if (idx < BranchClassLabels.size()) {
        return BranchClassLabels[idx];
    }
    return "invalid";
}

void
DecoupledBPUWithBTB::addBranchClassStat(BranchClass cls, bool mispred)
{
    auto idx = static_cast<size_t>(cls);
    if (idx >= NumBranchClasses) {
        DPRINTF(DBPBTBStats, "Skip invalid branch class stats update %d\n",
                static_cast<int>(cls));
        return;
    }

    dbpBtbStats.branchClassCounts[idx]++;
    if (mispred) {
        dbpBtbStats.branchClassMisses[idx]++;
        dbpBtbStats.branchClassCountsTotal++;
    }

    DPRINTF(DBPBTBStats, "Branch classified as %s, mispred=%d\n",
            branchClassName(cls), mispred);
}

void
DecoupledBPUWithBTB::addControlSquashCommitStat(BranchClass cls)
{
    auto idx = static_cast<size_t>(cls);
    if (idx >= NumBranchClasses) {
        DPRINTF(DBPBTBStats,
                "Skip invalid commit squash class stats update %d\n",
                static_cast<int>(cls));
        return;
    }

    dbpBtbStats.controlSquashByClass[idx]++;
    DPRINTF(DBPBTBStats, "Commit squash classified as %s\n",
            branchClassName(cls));
}

void
DecoupledBPUWithBTB::updateStatistics(const FetchTarget &target)
{
    // Check if this target was mispredicted
    bool miss_predicted = target.squashType == SQUASH_CTRL;
    // Track indirect mispredictions
    if (miss_predicted && target.exeBranchInfo.isIndirect) {
        topMispredIndirect[target.startPC]++;
    }

    // --- BTB Statistics ---
    if (target.isHit) {
        // Count BTB hits
        dbpBtbStats.btbHit++;
    } else {
        // Count BTB misses for taken branches
        if (target.exeTaken) {
            dbpBtbStats.btbMiss++;
            DPRINTF(BTB, "BTB miss detected when update, target start %#lx, predTick %lu, printing branch info:\n",
                    target.startPC, target.predTick);
            auto &slot = target.exeBranchInfo;
            DPRINTF(BTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
        }

        // Count false hits
        if (target.falseHit) {
            dbpBtbStats.commitFalseHit++;
        }
    }

    if (target.isHit || target.exeTaken) {
        // Update BTB entry statistics
        auto it = totalBTBEntries.find(target.startPC);
        if (it == totalBTBEntries.end()) {
            auto &btb_entry = target.updateNewBTBEntry;
            totalBTBEntries[target.startPC] = std::make_pair(btb_entry, 1);
            dbpBtbStats.btbEntriesWithDifferentStart++;
        } else {
            it->second.second++;
            it->second.first = target.updateNewBTBEntry;
        }
    }

    // Track which predictor stage was used
    dbpBtbStats.commitPredsFromEachStage[target.predSource]++;
    overrideStats(target.overrideReason);

    // --- Instruction Statistics ---
    // Track committed instruction counts
    dbpBtbStats.commitFsqEntryHasInsts.sample(target.commitInstNum, 1);
    if (target.commitInstNum >= 0 && target.commitInstNum <= maxInstsNum) {
        commitFsqEntryHasInstsVector[target.commitInstNum]++;
        if (target.commitInstNum == 1 && target.exeBranchInfo.isUncond()) {
            dbpBtbStats.commitFsqEntryOnlyHasOneJump++;
        }
    }

    // Track fetched instruction counts
    dbpBtbStats.commitFsqEntryFetchedInsts.sample(target.fetchInstNum, 1);
    if (target.fetchInstNum >= 0 && target.fetchInstNum <= maxInstsNum) {
        commitFsqEntryFetchedInstsVector[target.fetchInstNum]++;
    }

    // --- Misprediction Statistics ---
    // Track control squashes (mispredictions)
    if (target.squashType == SQUASH_CTRL) {
        // Record mispredict pair (start PC, branch PC)
        auto find_it = topMispredicts.find(std::make_pair(target.startPC, target.exeBranchInfo.pc));
        if (find_it == topMispredicts.end()) {
            topMispredicts[std::make_pair(target.startPC, target.exeBranchInfo.pc)] = 1;
        } else {
            find_it->second++;
        }

        // Track history pattern for mispredictions
        auto hist(target.history);
        hist.resize(18);
        uint64_t pattern = hist.to_ulong();
        auto find_it_hist = topMispredHist.find(pattern);
        if (find_it_hist == topMispredHist.end()) {
            topMispredHist[pattern] = 1;
        } else {
            find_it_hist->second++;
        }
    }
}

void
DecoupledBPUWithBTB::commitBranch(const DynInstPtr &inst, bool mispred)
{
    // ---------- Update overall branch statistics ----------
    if (inst->isUncondCtrl()) {
        addCfi(UNCOND, mispred);
    }
    if (inst->isCondCtrl()) {
        addCfi(COND, mispred);
    }
    if (inst->isReturn()) {
        addCfi(RETURN, mispred);
    } else if (inst->isIndirectCtrl()) {
        addCfi(OTHER, mispred);
    }

    auto branchClass = classifyBranch(inst);
    addBranchClassStat(branchClass, mispred);

    // ---------- Find corresponding fetch target entry ----------
    auto entry = ftq.get(inst->ftqId, inst->threadNumber);

    // Record branch trace if enabled
    if (enableBranchTrace) {
        bptrace->write_record(BpTrace(inst->ftqId, entry, inst, mispred));
    }

    // ---------- Extract branch information ----------
    Addr branchAddr = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr targetAddr = rv_pc.npc();
    Addr fallThruPC = rv_pc.getFallThruPC();
    BranchInfo info(branchAddr, targetAddr, inst->staticInst, fallThruPC-branchAddr);
    bool taken = rv_pc.branching() || inst->isUncondCtrl();

    // ---------- Process misprediction and update statistics ----------
    processMisprediction(entry, branchAddr, info, taken, mispred);

    // ---------- Track taken branches for statistics ----------
    if (taken) {
        trackTakenBranch(branchAddr);
    }

    // ---------- Update predictor components ----------
    for (auto component : components) {
        component->commitBranch(entry, inst);
    }
    //here add final counter

    if (mispred) {
        commitPredWrongSource(entry);
    }

}

void
DecoupledBPUWithBTB::commitPredWrongSource(const FetchTarget &entry)
{
    int ubtbid = ubtb->getComponentIdx();
    int abtbid = abtb->getComponentIdx();
    int mbtbid = mbtb->getComponentIdx();
    int tageid = tage->getComponentIdx();
    int ittageid = ittage->getComponentIdx();
    int rasid = ras->getComponentIdx();

    int s1PredSource = entry.s1Source;
    int s3PredSource = entry.s3Source;

    auto exeBranchInfo = entry.exeBranchInfo;

    bool onlyDirectionWrong = entry.exeTaken != entry.predTaken;
    int s1SourceBucket = 0;

    assert(s1PredSource < mbtbid);
    if (s1PredSource == ubtbid) {
        dbpBtbStats.s1PredWrongUbtb++;
        s1SourceBucket = 1;
    } else if (s1PredSource == abtbid) {
        dbpBtbStats.s1PredWrongAbtb++;
        s1SourceBucket = 2;
    }else {
        dbpBtbStats.s1PredWrongFallthrough++;
    }

    dbpBtbStats.s1PredWrongBySourceAndReason
        [s1SourceBucket][overrideReasonBucket(entry.overrideReason)]++;

    if (s3PredSource == rasid) {
        if (exeBranchInfo.isCond) {
            dbpBtbStats.s3PredWrongTage++;
        } else if (exeBranchInfo.isReturn) {
            dbpBtbStats.s3PredWrongRas++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    } else if (s3PredSource == ittageid) {
        if (exeBranchInfo.isIndirect) {
            dbpBtbStats.s3PredWrongIttage++;
        } else if (exeBranchInfo.isCond) {
            dbpBtbStats.s3PredWrongTage++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    } else if (s3PredSource == tageid) {
        if (exeBranchInfo.isCond) {
            if (onlyDirectionWrong) {
                dbpBtbStats.s3PredWrongTage++;
            } else {
                dbpBtbStats.s3PredWrongMbtb++;
            }
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    }else if (s3PredSource == mbtbid) {
        if (exeBranchInfo.isCond) {
            if (onlyDirectionWrong) {
                dbpBtbStats.s3PredWrongTage++;
            } else {
                dbpBtbStats.s3PredWrongMbtb++;
            }
        } else if (exeBranchInfo.isIndirect) {
            dbpBtbStats.s3PredWrongIttage++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    }else if (s3PredSource == -1) {
        dbpBtbStats.s3PredWrongMbtb++;
    }
}
/**
 * @brief Handle instruction commits and phase-based statistics
 *
 * This function is called whenever an instruction is committed. It updates
 * instruction counts and maintains phase-based statistics. When a phase
 * boundary is reached, it collects detailed statistics for the phase.
 */
void
DecoupledBPUWithBTB::notifyInstCommit(const DynInstPtr &inst)
{
    if (bpStatEnabled) {
        recordBpStatCommittedInst(inst);
    }

    // Update committed instruction count for target
    ftq.get(inst->ftqId, inst->threadNumber).commitInstNum++;

    // Update global committed instruction count
    numInstCommitted++;

    DPRINTF(Profiling, "notifyInstCommit, inst=%s, commitInstNum=%d\n",
            inst->staticInst->disassemble(inst->pcState().instAddr()),
            ftq.get(inst->ftqId, inst->threadNumber).commitInstNum);

    // ----------------------- Main Phase Processing -------------------------
    if (numInstCommitted % phaseSizeByInst == 0) {
        int currentPhaseID = numInstCommitted / phaseSizeByInst;

        // Process main phase statistics if needed
        if (processPhase(false, currentPhaseID, phaseIdToDump,
                        lastPhaseTopMispredictsByBranch,
                        topMispredictsByBranchByPhase,
                        currentPhaseTakenBranches,
                        takenBranchesByPhase)) {

            // Process fetch instruction distributions
            std::vector<int> committedInstDist, fetchedInstDist;
            processFetchDistributions(committedInstDist, fetchedInstDist);

            // Store the distributions
            fsqEntryNumCommittedInstDistByPhase.push_back(committedInstDist);
            fsqEntryNumFetchedInstDistByPhase.push_back(fetchedInstDist);

            // Process BTB entries
            BTBEntriesByPhase.push_back(processBTBEntries());
        }
    }

    // ---------------------- Sub-Phase Processing --------------------------
    if (numInstCommitted % subPhaseSizeByInst() == 0) {
        int currentSubPhaseID = numInstCommitted / subPhaseSizeByInst();

        // Process sub-phase statistics if needed
        processPhase(true, currentSubPhaseID, subPhaseIdToDump,
                    lastSubPhaseTopMispredictsByBranch,
                    topMispredictsByBranchBySubPhase,
                    currentSubPhaseTakenBranches,
                    takenBranchesBySubPhase);
    }
}


/**
 * @brief Process branch misprediction, determine type and update statistics
 *
 * @param entry The fetch target entry
 * @param branchAddr Branch instruction address
 * @param info Branch information
 * @param taken Whether the branch was taken
 * @param mispred Whether the branch was mispredicted
 */
void
DecoupledBPUWithBTB::processMisprediction(
    const FetchTarget &entry,
    Addr branchAddr,
    const BranchInfo &info,
    bool taken,
    bool mispred)
{
    MispredType mispredType = FAKE_LAST;

    // Determine misprediction type only if there was a misprediction
    if (mispred) {
        if (!taken) {
            // Only conditional branches can be not-taken
            assert(info.isCond);
            mispredType = DIR_WRONG; // Direction was wrong
        } else {
            // Check if this branch was in the predicted BTB entries
            bool predBranchInBTB = false;
            for (auto &e: entry.predBTBEntries) {
                if (e.pc == branchAddr) {
                    predBranchInBTB = true;
                    break;
                }
            }

            if (!predBranchInBTB) {
                mispredType = NO_PRED; // Branch wasn't predicted at all
            } else if (entry.predTaken && entry.predBranchInfo.pc == branchAddr) {
                mispredType = TARGET_WRONG; // Branch predicted taken but wrong target
            } else {
                // Branch predicted not taken or different branch predicted taken
                mispredType = DIR_WRONG;
            }
        }

        DPRINTF(Profiling, "branchAddr %#lx is mispredicted, taken %d, type %d, missType %d\n",
                branchAddr, taken, info.getType(), mispredType);
        assert(mispredType != FAKE_LAST);
    }

    // Create branch key for the statistics map
    auto branchKey = std::make_pair(branchAddr, info.getType());

    // Update branch statistics
    DPRINTF(Profiling, "lookup topMispredictsByBranch for branchAddr %#lx, type %d\n",
            branchAddr, info.getType());

    auto statsIt = topMispredictsByBranch.find(branchKey);

    if (statsIt == topMispredictsByBranch.end()) {
        // Create new statistics entry for this branch
        DPRINTF(Profiling, "not found, insert with mispred=%d\n", mispred);

        // Initialize new branch stats
        BranchStats stats(branchAddr, info.getType());
        stats.incrementTotal();  // Always increment total count

        // Only increment misprediction count if actually mispredicted
        if (mispred) {
            stats.incrementMispred(mispredType);
        }

        // Store in map
        topMispredictsByBranch[branchKey] = stats;
        dbpBtbStats.staticBranchNum++;
    } else {
        // Update existing statistics entry
        DPRINTF(Profiling, "found, total %d, miss %d\n",
                statsIt->second.totalCount, statsIt->second.mispredCount);

        // Always increment total count
        statsIt->second.incrementTotal();

        // Only increment misprediction count if actually mispredicted
        if (mispred) {
            statsIt->second.incrementMispred(mispredType);
        }
    }
}

/**
 * @brief Track statistics for taken branches
 *
 * @param branchAddr Branch instruction address
 */
void
DecoupledBPUWithBTB::trackTakenBranch(Addr branchAddr)
{
    // Helper function to update a branch map
    auto updateBranchMap = [branchAddr](std::unordered_map<Addr, int> &branchMap) {
        auto it = branchMap.find(branchAddr);
        if (it == branchMap.end()) {
            // Branch not found - add with count 1
            branchMap[branchAddr] = 1;
        } else {
            // Branch found - increment count
            it->second++;
        }
    };

    // Update all three branch maps
    updateBranchMap(takenBranches);
    updateBranchMap(currentPhaseTakenBranches);
    updateBranchMap(currentSubPhaseTakenBranches);
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
