#include "cpu/pred/btb/test/hint_indirect_test.hh"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <boost/dynamic_bitset.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/pred/btb/hint_history.hh"
#include "cpu/pred/btb/hint_indirect.hh"
#include "cpu/pred/btb/history_manager.hh"
#include "sim/sim_exit.hh"

namespace gem5::branch_prediction::btb_pred
{
namespace
{

void
expect(bool value, const char *message)
{
    fatal_if(!value, "Hint self-test: %s", message);
}

std::string
writeInput(const std::string &name, const std::string &text)
{
    const auto path = simout.resolve(name);
    std::ofstream out(path);
    out << text;
    fatal_if(!out, "Cannot write Hint self-test input %s", path);
    return path;
}

std::string
readOutput(const std::string &name)
{
    std::ifstream input(simout.resolve(name));
    expect(bool(input), "missing test output");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string
readGzip(const std::string &name)
{
    gzFile input = gzopen(simout.resolve(name).c_str(), "rb");
    expect(input, "missing compressed profile");
    std::string result;
    std::array<char, 4096> buffer;
    int count;
    while ((count = gzread(input, buffer.data(), buffer.size())) > 0) {
        result.append(buffer.data(), count);
    }
    expect(count == 0 && gzclose(input) == Z_OK, "profile gzip integrity");
    return result;
}

uint32_t
foldBits(const boost::dynamic_bitset<> &bits)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            value ^= uint32_t(1) << (i % 28);
        }
    }
    return value;
}

uint32_t
foldWord(uint64_t word)
{
    return foldBits(boost::dynamic_bitset<>(64, word));
}

uint32_t
rotateBits(uint32_t value, unsigned amount)
{
    uint32_t result = 0;
    for (unsigned bit = 0; bit < 28; ++bit) {
        if (value & (uint32_t(1) << bit)) {
            result |= uint32_t(1) << ((bit + amount) % 28);
        }
    }
    return result;
}

uint32_t
explicitWindowFold(const HintPCWindow &window)
{
    uint32_t result = 0;
    for (unsigned age = 0; age < window.count; ++age) {
        result ^= rotateBits(foldWord(window.atAge(age)), age);
    }
    return result;
}

void
testHistory()
{
    HintHistory history;
    history.configure({0x1000, 0x2000});
    boost::dynamic_bitset<> ghr(HintGHRBits);
    for (unsigned bit = 0; bit < HintGHRBits; ++bit) {
        ghr[bit] = ((bit * bit + 3 * bit) % 7) < 3;
    }
    HintSnapshot empty(history, ghr);
    expect(empty.contextsFor(0x1000)[0] == foldBits(ghr), "970-bit GHR fold");

    history.update(0x1000, 0x8002, false, true, false, true);
    expect(history.calls.count == 1 && history.globalLow == 0,
           "direct call changes only call PC history");
    history.update(0x1000, 0x8006, true, true, false, true);
    expect(history.calls.count == 2 && history.globalLow == 3 &&
           history.local(0x1000) == 3, "indirect call target and call histories");
    history.update(0x1000, 0x8004, true, false, true, true);
    expect(history.returns.count == 1 && history.globalLow == 3,
           "return excludes indirect target histories");
    history.update(0x1000, 0x8000, true, true, true, true);
    expect(history.calls.count == 3 && history.returns.count == 2 &&
           history.globalLow == 3, "pop-and-push updates both PC windows");
    const auto before_not_taken = history;
    history.update(0x1000, 0x8002, true, true, true, false);
    expect(history.calls.folded == before_not_taken.calls.folded &&
           history.returns.folded == before_not_taken.returns.folded &&
           history.globalLow == before_not_taken.globalLow,
           "not-taken branch contributes no target/PC event");

    history.configure({0x1000, 0x2000});
    history.globalLow = history.globalHigh = 0;
    boost::dynamic_bitset<> expected_global(128);
    uint8_t expected_local = 0;
    for (unsigned event = 0; event < 130; ++event) {
        const auto bits = (event * 3 + 1) & 3;
        expected_global <<= 2;
        expected_global[0] = bits & 1;
        expected_global[1] = bits & 2;
        expected_local = (expected_local << 2) | bits;
        history.update(0x1000, 0x8000 | bits << 1,
                       true, false, false, true);
        expect(history.globalFold() == foldBits(expected_global),
               "128-bit GITHR exact 64-target retention");
        expect(history.local(0x1000) == expected_local &&
               history.local(0x2000) == 0, "per-PC four-target retention");
    }
    for (unsigned event = 0; event < 129; ++event) {
        history.update(0x1000 + event * 2, 0, false, true, true, true);
        expect(history.calls.count <= 64 && history.returns.count <= 64,
               "bounded call and return histories");
        expect(history.calls.folded == explicitWindowFold(history.calls) &&
               history.returns.folded == explicitWindowFold(history.returns),
               "64/65/129-event PC fold matches age-based oracle");
    }

    const HintSnapshot snapshot(history, ghr);
    const auto contexts = snapshot.contextsFor(0x1000);
    std::ostringstream vectors;
    vectors << "{\"pc\":\"0x1000\",\"contexts\":[";
    for (unsigned i = 0; i < 5; ++i) {
        if (i) vectors << ',';
        vectors << contexts[i];
    }
    vectors << "],\"keys\":[";
    for (unsigned mask = 0; mask < 32; ++mask) {
        uint32_t expected = foldWord(0x1000 >> 1);
        for (unsigned component = 0; component < 5; ++component) {
            if (mask & (1U << component)) {
                expected ^= rotateBits(contexts[component], component + 1);
            }
        }
        expect(hintHash(0x1000, contexts, mask) == expected,
               "all 32 mask hashes match bitwise oracle");
        if (mask) vectors << ',';
        vectors << expected;
    }
    vectors << "]}\n";
    writeInput("hint-hash-vectors.json", vectors.str());
    history.update(0x1000, 0x9998, true, true, false, true);
    expect(snapshot.contextsFor(0x1000) == contexts,
           "prediction snapshot stays immutable after speculative update");
    history = snapshot.history;
    expect(HintSnapshot(history, ghr).contextsFor(0x1000) == contexts,
           "snapshot copy restores all added histories");
}

void
testRetiredPrefix()
{
    BranchInfo call;
    call.pc = 0x1000;
    call.target = 0x1004;
    call.size = 4;
    call.isCall = true;
    call.isIndirect = true;
    call.isDirect = false;
    BranchInfo conditional;
    conditional.pc = 0x1004;
    conditional.target = 0x1008;
    conditional.size = 4;
    conditional.isCond = true;
    conditional.isDirect = true;
    BranchInfo ret;
    ret.pc = 0x1008;
    ret.target = 0x100c;
    ret.size = 4;
    ret.isIndirect = true;
    ret.isReturn = true;
    ret.isDirect = false;
    FetchTarget target;
    target.recordRetiredBranch(call, true, 16);
    target.recordRetiredBranch(conditional, false, 16);
    target.recordRetiredBranch(ret, true, 16);
    target.lastRetiredPC = 0x100c; // The final retired instruction is not a CFI.
    expect(target.retiredControls.size() == 3 && target.retiredTaken &&
           target.retiredTakenBranch.pc == 0x1008 &&
           target.lastRetiredPC == 0x100c,
           "trap prefix keeps both taken-to-fallthrough branches and final PC");

    HintHistory restored;
    restored.configure({0x1000});
    for (const auto &control : target.retiredControls) {
        const auto &branch = control.branch;
        restored.update(branch.pc, branch.target, branch.isIndirect,
                         branch.isCall, branch.isReturn, control.taken);
    }
    expect(restored.calls.count == 1 && restored.returns.count == 1 &&
           restored.local(0x1000) == 2,
           "retired trap prefix replays call, return and target classification");

    HintHistory base;
    base.configure({0x1000, 0x1008});
    target.hintSnapshot = std::make_shared<const HintSnapshot>(
        base, boost::dynamic_bitset<>(HintGHRBits));
    HintHistory recovered = base;
    recovered.update(0x1000, 0x7006, true, true, true, true);
    recovered.recoverForSquash(target, 0x1008, false);
    expect(recovered.calls.count == 1 && recovered.returns.count == 0 &&
           recovered.local(0x1000) == 2,
           "memory squash removes wrong path and excludes equal-PC control");
    target.exeBranchInfo = ret;
    target.exeTaken = true;
    recovered.recoverForSquash(target, 0x1008, true);
    expect(recovered.calls.count == 1 && recovered.returns.count == 1 &&
           recovered.local(0x1000) == 2,
           "actual return is appended once after older retired prefix");
    target.exeBranchInfo = call;
    target.exeBranchInfo.target = 0x8006;
    recovered.recoverForSquash(target, 0x1000, true);
    expect(recovered.calls.count == 1 && recovered.returns.count == 0 &&
           recovered.local(0x1000) == 3,
           "actual call replaces equal-PC retired event without duplication");
    recovered.recoverForSquash(target, target.lastRetiredPC, false);
    expect(recovered.calls.count == 1 && recovered.returns.count == 1 &&
           recovered.local(0x1000) == 2,
           "trap after ordinary instruction restores full retired prefix");

    HistoryManager manager(16);
    boost::dynamic_bitset<> ghr(128), phr(128);
    DirectionHistoryUpdate no_direction;
    PathHistoryUpdate no_path;
    manager.addSpeculativeHist(0x0800, ghr, phr, no_direction, no_path, call, 1);
    manager.addSpeculativeHist(0x1000, ghr, phr, no_direction, no_path, call, 2);
    manager.addSpeculativeHist(0x2000, ghr, phr, no_direction, no_path, ret, 3);
    manager.truncateFrom(2);
    expect(manager.getSpeculativeHist().size() == 1,
           "trap removes its predicted descriptor and all younger histories");
    for (const auto &control : target.retiredControls) {
        auto branch = control.branch;
        DirectionHistoryUpdate direction;
        direction.shamt = branch.isCond ? 1 : 0;
        direction.taken = branch.isCond && control.taken;
        PathHistoryUpdate path;
        path.taken = control.taken;
        path.pc = branch.pc;
        path.target = branch.target;
        manager.addSpeculativeHist(branch.pc, ghr, phr, direction, path,
                                    branch, 2);
    }
    const auto &entries = manager.getSpeculativeHist();
    expect(entries.size() == 4, "three retired events can share one stream ID");
    auto entry = std::next(entries.begin());
    expect(entry->pc == 0x1000 && entry->is_call, "first replayed event is call");
    ++entry;
    expect(entry->pc == 0x1004 && entry->shamt == 1 && !entry->cond_taken,
           "middle replayed event preserves not-taken direction");
    ++entry;
    expect(entry->pc == 0x1008 && entry->is_return,
           "last replayed event is return");
    manager.addSpeculativeHist(0x2000, ghr, phr, no_direction, no_path, ret, 3);
    manager.addSpeculativeHist(0x3000, ghr, phr, no_direction, no_path, call, 4);
    manager.squash(3, no_direction, no_path, ret);
    expect(manager.getSpeculativeHist().size() == 5,
           "younger squash preserves complete retired prefix");
    manager.commit(2);
    expect(manager.getSpeculativeHist().size() == 1 &&
           manager.getSpeculativeHist().front().streamId == 3,
           "commit removes every replayed event in committed stream");
}

std::string
metadataText(bool empty)
{
    return std::string(
        "{\"format\":\"h2p-hint\",\"version\":2,\"hash_version\":2,"
        "\"ghr_bits\":970,\"mask_order\":[\"GHR\",\"GITHR\",\"LITHR\",\"Call\",\"Return\"],"
        "\"history\":{\"githr_targets\":64,\"lithr_targets\":4,\"call_pcs\":64,"
        "\"return_pcs\":64,\"target_shift\":1},\"layout\":{\"cam_entries\":16,"
        "\"context_entries\":4096,\"ways\":2,\"slot_entries\":32,"
        "\"conf_queue_depth\":8},\"source\":{\"purpose\":\"production-helper-test\"},") +
        (empty ? "\"cam\":[],\"slots\":[],\"entries\":[]}" :
        "\"cam\":[{\"pc\":\"0x1000\",\"mask\":0,\"valid\":true},"
        "{\"pc\":\"0x1008\",\"mask\":0,\"valid\":true},"
        "{\"pc\":\"0x2000\",\"mask\":0,\"valid\":true},"
        "{\"pc\":\"0x3000\",\"mask\":0,\"valid\":true},"
        "{\"pc\":\"0x20001002\",\"mask\":0,\"valid\":true}],"
        "\"slots\":[\"0x4000\",\"0x5000\"],\"entries\":["
        "{\"set\":0,\"way\":0,\"tag\":1,\"slot\":0,\"conf\":3,\"valid\":true},"
        "{\"set\":0,\"way\":1,\"tag\":2,\"slot\":1,\"conf\":3,\"valid\":true}]}");
}

std::shared_ptr<const HintSnapshot>
snapshotFor(const HintIndirect &hardware)
{
    HintHistory history;
    history.configure(hardware.historyPCs());
    return std::make_shared<const HintSnapshot>(
        history, boost::dynamic_bitset<>(HintGHRBits));
}

void
testMaskQueries()
{
    HintHistory history;
    history.configure({0x1000});
    for (unsigned event = 0; event < 65; ++event) {
        history.update(0x1000, 0x8000 + ((event * 5 + 1) & 7) * 2,
                       true, true, false, true);
        history.update(0x2000 + event * 2, 0x9000,
                       true, false, true, true);
    }
    boost::dynamic_bitset<> ghr(HintGHRBits);
    ghr[0] = ghr[63] = ghr[969] = true;
    const auto snapshot = std::make_shared<const HintSnapshot>(history, ghr);
    const auto contexts = snapshot->contextsFor(0x1000);
    for (unsigned mask = 0; mask < 32; ++mask) {
        uint32_t key = foldWord(0x1000 >> 1);
        for (unsigned component = 0; component < 5; ++component) {
            if (mask & (1U << component)) {
                key ^= rotateBits(contexts[component], component + 1);
            }
        }
        auto text = metadataText(true);
        const auto replace = [&text](const std::string &old_text,
                                      const std::string &new_text) {
            const auto pos = text.find(old_text);
            expect(pos != std::string::npos, "mask metadata template field");
            text.replace(pos, old_text.size(), new_text);
        };
        replace("\"cam\":[]", "\"cam\":[{\"pc\":\"0x1000\",\"mask\":" +
            std::to_string(mask) + ",\"valid\":true}]");
        std::ostringstream slot_text;
        slot_text << "\"slots\":[";
        const unsigned slot = mask == 31 ? 31 : 0;
        for (unsigned index = 0; index <= slot; ++index) {
            if (index) slot_text << ',';
            slot_text << "\"0x" << std::hex << (0x4000 + index * 2) << "\"";
        }
        slot_text << ']';
        const Addr expected_target = 0x4000 + slot * 2;
        replace("\"slots\":[]", slot_text.str());
        replace("\"entries\":[]", "\"entries\":[{\"set\":" +
            std::to_string(key & 2047) + ",\"way\":1,\"tag\":" +
            std::to_string(key >> 11) +
            ",\"slot\":" + std::to_string(slot) +
            ",\"conf\":3,\"valid\":true}]");
        HintIndirect::Config config;
        config.mode = "replay";
        config.metadataPath = writeInput(
            "hint-mask-" + std::to_string(mask) + ".json", text);
        config.outputPrefix = "mask-test-" + std::to_string(mask);
        HintIndirect hardware(config, nullptr);
        hardware.setMeasurementActive(true);
        auto query = hardware.query(snapshot, 0x1000, 0x1010, 1);
        const auto prediction = hardware.complete(query, 0x1000, 0x6000,
                                                   true, true, false, true);
        expect(query->key == key && query->mask == mask && query->way == 1 &&
               prediction == std::optional<Addr>(expected_target),
               "all 32 loaded CAM masks drive actual production SRAM lookup");
        hardware.commit(query, 1, 0x1000, expected_target,
                        true, expected_target, 1);
        expect(hardware.totals().corrected == 1 &&
               hardware.totals().finalMisses == 0,
               "all masks preserve original baseline and final-target accounting");
        hardware.finish();
    }
}

void
testHardware(const std::string &metadata, const std::string &candidates)
{
    HintIndirect::Config config;
    config.mode = "replay";
    config.metadataPath = metadata;
    config.candidatePath = candidates;
    config.outputPrefix = "replay-test";
    config.clockPeriod = 10;
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    auto predict = [&](uint64_t cycle, Addr pc, Addr baseline = 0x6000) {
        auto query = hardware.query(snapshot, pc, pc + 16, cycle);
        hardware.complete(query, pc, baseline, true, true, false, true);
        return query;
    };
    auto warm = predict(1, 0x1000);
    expect(warm->tagHit && warm->hintTarget == 0x4000 && warm->overridden,
           "initial confidence-three entry predicts in warm-up");
    hardware.commit(warm, 1, 0x1000, 0x6000, true, 0x4000, 1);
    hardware.tick(2);
    expect(hardware.pendingUpdates() == 0 && hardware.totals().executions == 0,
           "warm-up neither collects nor scores confidence");
    hardware.setMeasurementActive(true);
    auto a = predict(3, 0x1000);
    auto b = predict(4, 0x1000);
    auto c = predict(5, 0x1000);
    hardware.tick(10);
    for (const auto &query : {a, b, c}) {
        hardware.commit(query, query->queryCycle, 0x1000, 0x6000,
                        true, 0x4000, 10);
    }
    hardware.tick(10);
    expect(hardware.confidenceAt(0, 0) == 3 &&
           hardware.pendingUpdates() == 3, "no same-cycle confidence bypass");
    hardware.tick(11);
    hardware.tick(11);
    expect(hardware.confidenceAt(0, 0) == 2 &&
           hardware.pendingUpdates() == 2, "single write per cycle");
    hardware.tick(13);
    expect(hardware.confidenceAt(0, 0) == 0 &&
           hardware.queueCounts().zeroTransitions == 1,
           "idle catch-up preserves writes at cycles 12 and 13");
    auto shadow = predict(14, 0x1000, 0x4000);
    expect(shadow->tagHit && shadow->conf == 0 && !shadow->overridden,
           "confidence-zero has a shadow target but cannot override");
    hardware.commit(shadow, 14, 0x1000, 0x4000, true, 0x4000, 14);
    auto recovered = predict(15, 0x1000);
    expect(recovered->conf == 1 && recovered->overridden &&
           hardware.queueCounts().recoveries == 1,
           "shadow correct update recovers before next query");
    hardware.commit(recovered, 15, 0x1000, 0x6000, true, 0x4000, 15);
    hardware.tick(16);
    expect(hardware.queueCounts().zeroTransitions == 2,
           "each recovery followed by one-to-zero is logged");

    auto two_way = predict(17, 0x2000, 0x5000);
    expect(two_way->set == 0 && two_way->way == 1 &&
           two_way->hintTarget == 0x5000, "second way selected by tag");
    auto absent = predict(18, 0x3000);
    expect(!absent->tagHit && !absent->overridden,
           "third key in same set has no implicit extra way");
    hardware.commit(absent, 18, 0x3000, 0x6000, true, 0x6000, 18);
    auto alias = predict(19, 0x20001002);
    expect(alias->tagHit && alias->set == 0 && alias->way == 0,
           "full-PC alias uses the same physical table entry");
    auto contention = hardware.query(snapshot, 0x1000, 0x1010, 20);
    expect(contention->pc == 0x1000, "CAM selects earliest PC in block");
    expect(!hardware.complete(contention, 0x1008, 0x4000,
                              true, true, false, true),
           "later selected candidate cannot obtain a second query port");
    auto not_btb = hardware.query(snapshot, 0x2000, 0x2010, 21);
    expect(!hardware.complete(not_btb, 0x2000, 0x6000,
                              true, true, false, false), "BTB miss cannot override");
    auto ret = hardware.query(snapshot, 0x2000, 0x2010, 22);
    expect(!hardware.complete(ret, 0x2000, 0x6000,
                              true, true, true, true), "return cannot override");
    auto direct = hardware.query(snapshot, 0x2000, 0x2010, 23);
    expect(!hardware.complete(direct, 0x2000, 0x6000,
                              true, false, false, true), "direct cannot override");
    auto not_taken = hardware.query(snapshot, 0x2000, 0x2010, 24);
    expect(!hardware.complete(not_taken, 0x2000, 0x6000,
                              false, true, false, true), "not-taken cannot override");
    auto squashed = predict(25, 0x2000);
    (void)squashed; // Wrong-path queries are discarded, never sent to commit.
    const auto accepted = hardware.queueCounts().enqueued;
    hardware.tick(30);
    expect(hardware.queueCounts().enqueued == accepted,
           "queries alone do not score confidence");
    std::vector<std::shared_ptr<HintQuery>> burst;
    for (uint64_t cycle = 31; cycle <= 39; ++cycle) {
        burst.push_back(predict(cycle, 0x2000));
    }
    hardware.tick(40);
    for (const auto &query : burst) {
        hardware.commit(query, query->queryCycle, 0x2000, 0x5000,
                        true, 0x5000, 40);
    }
    expect(hardware.pendingUpdates() == 8 &&
           hardware.queueCounts().dropped == 1,
           "eight-entry FIFO drops the new ninth update");
    hardware.tick(42);
    expect(hardware.pendingUpdates() == 6 && hardware.confidenceAt(0, 1) == 3,
           "saturation applies at writeback for multiple in-flight updates");
    hardware.finish();
    expect(hardware.pendingUpdates() == 6, "finish does not drain future writes");
    const auto stats = readOutput("replay-test/hint-stats.json");
    expect(stats.find("\"pending\":6") != std::string::npos,
           "pending updates are explicit in output");
    const auto log = readOutput("replay-test/conf0.txt");
    expect(std::count(log.begin(), log.end(), '\n') == 2 &&
           log.find("\"writeback_cycle\":13") != std::string::npos &&
           log.find("\"writeback_cycle\":16") != std::string::npos,
           "each logical confidence-zero write cycle is preserved");

    config.outputPrefix = "reset-test";
    HintIndirect reset_hardware(config, nullptr);
    reset_hardware.setMeasurementActive(true);
    auto query = reset_hardware.query(snapshot, 0x1000, 0x1010, 1);
    reset_hardware.complete(query, 0x1000, 0x4000, true, true, false, true);
    reset_hardware.commit(query, 1, 0x1000, 0x6000, true, 0x4000, 1);
    reset_hardware.resetStats();
    expect(reset_hardware.pendingUpdates() == 1 &&
           reset_hardware.queueCounts().carriedIn == 1 &&
           reset_hardware.totals().executions == 0,
           "stats reset preserves FIFO and records carried-in update");
    reset_hardware.tick(2);
    expect(reset_hardware.confidenceAt(0, 0) == 2 &&
           reset_hardware.queueCounts().applied == 1,
           "carried-in write changes existing microarchitecture state");
    reset_hardware.resetStats();
    expect(reset_hardware.confidenceAt(0, 0) == 2,
           "stats reset preserves trained confidence");
    reset_hardware.finish();
}

void
testCollect(const std::string &candidate_path)
{
    HintIndirect::Config config;
    config.mode = "collect";
    config.candidatePath = candidate_path;
    config.outputPrefix = "collect-test";
    HintIndirect collect(config, nullptr);
    const auto snapshot = snapshotFor(collect);
    auto warm = collect.query(snapshot, 0x1000, 0x1010, 1);
    collect.complete(warm, 0x1000, 0x4000, true, true, false, true);
    collect.commit(warm, 1, 0x1000, 0x4000, true, 0x4000, 1);
    collect.resetStats();
    expect(readOutput("collect-test/conf0.txt").empty(),
           "collect creates an empty confidence log and preserves it on reset");
    collect.setMeasurementActive(true);
    auto query = collect.query(snapshot, 0x1000, 0x1010, 2);
    expect(!collect.complete(query, 0x1008, 0x4000,
                             true, true, false, true),
           "collect never overrides a target");
    collect.commit(query, 2, 0x1008, 0x4000, true, 0x4000, 3);
    collect.commit(nullptr, 3, 0x1000, 0x5000, false, 0, 4);
    collect.commit(nullptr, 4, 0x9990, 0x6000, false, 0, 5);
    collect.finish();
    expect(readOutput("collect-test/conf0.txt").empty(),
           "collect never writes confidence transitions");
    expect(collect.totals().executions == 3 &&
           collect.totals().baselineMisses == 2 &&
           collect.totals().finalMisses == 2,
           "global counters include non-candidate indirect branches");
    const auto profile = readGzip("collect-test/hint-profile.jsonl.gz");
    expect(std::count(profile.begin(), profile.end(), '\n') == 3 &&
           profile.find("\"seq\":1,") == std::string::npos &&
           profile.find("\"seq\":4,") == std::string::npos,
           "profile includes every measured candidate, excludes warm-up and other PCs");
    expect(profile.find("\"pc\":\"0x1008\"") != std::string::npos &&
           profile.find("\"eligible\":true,\"contexts\":[") != std::string::npos,
           "collect retains later CAM candidates for separate group arbitration");
    expect(profile.find("\"query_cycle\":null") != std::string::npos &&
           profile.find("\"contexts\":null") != std::string::npos,
           "missing prediction metadata never fabricates contexts");
}

} // anonymous namespace

void
HintIndirectTest::startup()
{
    SimObject::startup();
    if (!metadata.empty()) {
        HintIndirect::Config config;
        config.mode = "replay";
        config.metadataPath = metadata;
        config.candidatePath = candidates;
        HintIndirect hardware(config, nullptr);
        hardware.finish();
    } else {
        testHistory();
        testRetiredPrefix();
        testMaskQueries();
        const auto candidate_path = writeInput("hint-test-candidates.json",
            "{\"format\":\"h2p-hint-candidates\",\"version\":2,\"source\":{},"
            "\"pcs\":[\"0x1000\",\"0x1008\",\"0x2000\",\"0x3000\",\"0x20001002\"]}");
        const auto metadata_path = writeInput("hint-test-metadata.json",
                                              metadataText(false));
        testHardware(metadata_path, candidate_path);
        testCollect(candidate_path);

        HintIndirect::Config empty_config;
        empty_config.mode = "replay";
        empty_config.metadataPath = writeInput("hint-test-empty.json",
                                                metadataText(true));
        empty_config.outputPrefix = "empty-test";
        HintIndirect empty(empty_config, nullptr);
        empty.setMeasurementActive(true);
        const auto snapshot = snapshotFor(empty);
        const auto query = empty.query(snapshot, 0x1000, 0x1010, 1);
        expect(!query->hasCandidate && !query->tagHit &&
               !empty.complete(query, 0x1000, 0x4000,
                               true, true, false, true),
               "empty metadata leaves every entry invalid");
        empty.commit(query, 1, 0x1000, 0x4000, true, 0x4000, 2);
        empty.finish();
        expect(empty.totals().finalMisses == 0 && empty.pendingUpdates() == 0,
               "empty table preserves baseline behavior");

        auto invalid_text = metadataText(false);
        const std::string valid_entry =
            "\"tag\":1,\"slot\":0,\"conf\":3,\"valid\":true";
        invalid_text.replace(invalid_text.find(valid_entry), valid_entry.size(),
            "\"tag\":1,\"slot\":0,\"conf\":0,\"valid\":false");
        auto invalid_config = empty_config;
        invalid_config.metadataPath = writeInput("hint-test-invalid-bit.json",
                                                 invalid_text);
        invalid_config.outputPrefix = "invalid-bit-test";
        HintIndirect invalid(invalid_config, nullptr);
        invalid.setMeasurementActive(true);
        const auto invalid_query = invalid.query(snapshotFor(invalid),
                                                  0x1000, 0x1010, 1);
        expect(!invalid_query->tagHit &&
               !invalid.complete(invalid_query, 0x1000, 0x4000,
                                  true, true, false, true),
               "valid-zero SRAM entry cannot become a shadow hit");
        invalid.commit(invalid_query, 1, 0x1000, 0x4000, true, 0x4000, 1);
        invalid.tick(2);
        expect(invalid.pendingUpdates() == 0 && invalid.confidenceAt(0, 0) == 0,
               "correct target cannot enable an invalid SRAM entry");
        invalid.finish();

        HintIndirect::Config off_config;
        off_config.outputPrefix = "off-test";
        HintIndirect off(off_config, nullptr);
        off.setMeasurementActive(true);
        off.commit(nullptr, 1, 0x1000, 0x4000, false, 0, 1);
        off.finish();
        expect(!std::filesystem::exists(simout.resolve("off-test")),
               "disabled mode does not collect or create files");
    }
    exitSimLoop("Hint self-test passed");
}

} // namespace gem5::branch_prediction::btb_pred
