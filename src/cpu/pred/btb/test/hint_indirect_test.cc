#include "cpu/pred/btb/test/hint_indirect_test.hh"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>

#include <boost/dynamic_bitset.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "base/logging.hh"
#include "base/output.hh"
#include "cpu/pred/btb/hint_history.hh"
#include "cpu/pred/btb/hint_indirect.hh"
#include "cpu/pred/btb/history_manager.hh"
#include "cpu/pred/btb/test/hint_history_ideal_test.hh"
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
    history = *snapshot.history;
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

// Convert only driver-owned finite fixtures. Production acceptance is always
// decided by HintIndirect's loader, never by this fixture generator.
std::string
idealizeMetadata(const std::string &finite)
{
    boost::property_tree::ptree tree;
    std::istringstream input(finite);
    boost::property_tree::read_json(input, tree);
    std::ostringstream output;
    output << "{\"format\":\"h2p-hint\",\"version\":3,"
        "\"storage\":\"unlimited\",\"hash_version\":2,\"ghr_bits\":970,"
        "\"mask_order\":[\"GHR\",\"GITHR\",\"LITHR\",\"Call\",\"Return\"],"
        "\"history\":{\"githr_targets\":64,\"lithr_targets\":4,\"call_pcs\":64,"
        "\"return_pcs\":64,\"target_shift\":1},"
        "\"finite_reference\":{\"cam_entries\":16,\"context_entries\":4096,"
        "\"ways\":2,\"slot_entries\":32},\"layout\":{\"conf_queue_depth\":8},"
        "\"source\":{\"purpose\":\"production-helper-test\"},\"cam\":[";
    bool first = true;
    for (const auto &node : tree.get_child("cam")) {
        if (!first) output << ',';
        first = false;
        const auto &row = node.second;
        output << "{\"pc\":\"" << row.get<std::string>("pc")
               << "\",\"mask\":" << row.get<unsigned>("mask")
               << ",\"valid\":" << (row.get<bool>("valid") ? "true" : "false")
               << '}';
    }
    output << "],\"slots\":[";
    first = true;
    for (const auto &node : tree.get_child("slots")) {
        if (!first) output << ',';
        first = false;
        output << '"' << node.second.get_value<std::string>() << '"';
    }
    output << "],\"entries\":[";
    first = true;
    for (const auto &node : tree.get_child("entries")) {
        if (!first) output << ',';
        first = false;
        const auto &row = node.second;
        output << "{\"key\":" << ((row.get<uint32_t>("tag") << 11) |
                                    row.get<uint32_t>("set"))
               << ",\"slot\":" << row.get<uint64_t>("slot")
               << ",\"conf\":" << row.get<unsigned>("conf")
               << ",\"valid\":" << (row.get<bool>("valid") ? "true" : "false")
               << '}';
    }
    output << "]}";
    return output.str();
}

std::string
idealizeCandidates(std::string finite)
{
    const std::string old_version = "\"version\":2";
    const auto position = finite.find(old_version);
    expect(position != std::string::npos, "finite candidate version fixture");
    finite.replace(position, old_version.size(),
                   "\"version\":3,\"storage\":\"unlimited\"");
    return finite;
}

std::shared_ptr<const HintSnapshot>
snapshotFor(const HintIndirect &hardware)
{
    if (!hardware.isIdeal()) {
        HintFiniteHistory finite;
        finite.configure(hardware.historyPCs());
        return std::make_shared<const HintSnapshot>(finite);
    }
    HintHistory history;
    history.configure(hardware.historyPCs(), hardware.isIdeal());
    return std::make_shared<const HintSnapshot>(
        history, boost::dynamic_bitset<>(HintGHRBits));
}

void
testMaskQueries(bool ideal = false)
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
        config.ideal = ideal;
        const std::string prefix = ideal ? "ideal-mask-test-" : "mask-test-";
        const std::string input_prefix = ideal ? "ideal-mask-test-" : "hint-mask-";
        config.metadataPath = writeInput(
            input_prefix + std::to_string(mask) + ".json",
            ideal ? idealizeMetadata(text) : text);
        config.outputPrefix = prefix + std::to_string(mask);
        HintIndirect hardware(config, nullptr);
        hardware.setMeasurementActive(true);
        auto query = hardware.query(snapshot, 0x1000, 0x1010, 1);
        const auto prediction = hardware.complete(query, 0x1000, 0x6000,
                                                   true, true, false, true);
        expect(query->key == key && query->mask == mask &&
               (ideal ? query->entryId == 0 : query->way == 1) &&
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
testHardware(const std::string &metadata, const std::string &candidates,
             bool ideal = false)
{
    HintIndirect::Config config;
    config.mode = "replay";
    config.ideal = ideal;
    config.metadataPath = metadata;
    config.candidatePath = candidates;
    config.outputPrefix = ideal ? "ideal-replay-test" : "replay-test";
    config.clockPeriod = 10;
    HintIndirect hardware(config, nullptr);
    auto confidence = [&](unsigned way) {
        return ideal ? hardware.confidenceAtEntry(way) :
                       hardware.confidenceAt(0, way);
    };
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
    expect(confidence(0) == 3 &&
           hardware.pendingUpdates() == 3, "no same-cycle confidence bypass");
    hardware.tick(11);
    hardware.tick(11);
    expect(confidence(0) == 2 &&
           hardware.pendingUpdates() == 2, "single write per cycle");
    hardware.tick(13);
    expect(confidence(0) == 0 &&
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
    expect(two_way->set == 0 &&
           (ideal ? two_way->entryId == 1 : two_way->way == 1) &&
           two_way->hintTarget == 0x5000, "second way selected by tag");
    auto absent = predict(18, 0x3000);
    expect(!absent->tagHit && !absent->overridden,
           "third key in same set has no implicit extra way");
    hardware.commit(absent, 18, 0x3000, 0x6000, true, 0x6000, 18);
    auto alias = predict(19, 0x20001002);
    expect(alias->tagHit && alias->set == 0 && alias->entryId == a->entryId,
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
    expect(hardware.pendingUpdates() == 6 && confidence(1) == 3,
           "saturation applies at writeback for multiple in-flight updates");
    hardware.finish();
    expect(hardware.pendingUpdates() == 6, "finish does not drain future writes");
    const auto stats = readOutput(config.outputPrefix + "/hint-stats.json");
    expect(stats.find("\"pending\":6") != std::string::npos,
           "pending updates are explicit in output");
    const auto log = readOutput(config.outputPrefix + "/conf0.txt");
    expect(std::count(log.begin(), log.end(), '\n') == 2 &&
           log.find("\"writeback_cycle\":13") != std::string::npos &&
           log.find("\"writeback_cycle\":16") != std::string::npos,
           "each logical confidence-zero write cycle is preserved");

    config.outputPrefix = ideal ? "ideal-reset-test" : "reset-test";
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
    expect(reset_hardware.confidenceAtEntry(0) == 2 &&
           reset_hardware.queueCounts().applied == 1,
           "carried-in write changes existing microarchitecture state");
    reset_hardware.resetStats();
    expect(reset_hardware.confidenceAtEntry(0) == 2,
           "stats reset preserves trained confidence");
    reset_hardware.finish();
}

[[maybe_unused]] void
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

void
testUnlimitedCapacity()
{
    constexpr size_t count = 5001;
    constexpr size_t target_count = 257;
    auto text = idealizeMetadata(metadataText(true));
    const auto replace = [&text](const std::string &old_text,
                                  const std::string &new_text) {
        const auto position = text.find(old_text);
        expect(position != std::string::npos, "ideal capacity fixture field");
        text.replace(position, old_text.size(), new_text);
    };
    std::ostringstream cam, targets, entries;
    cam << "\"cam\":[{\"pc\":\"0x200\",\"mask\":0,\"valid\":false}";
    for (size_t index = 0; index < count; ++index) {
        cam << ",{\"pc\":\"0x" << std::hex << ((index + 1) * 4096)
            << "\",\"mask\":0,\"valid\":true}";
    }
    cam << ",{\"pc\":\"0x20001002\",\"mask\":0,\"valid\":true}]";
    targets << "\"slots\":[";
    for (size_t index = 0; index < target_count; ++index) {
        if (index) targets << ',';
        targets << "\"0x" << std::hex << (0x80000000 + index * 2) << '"';
    }
    targets << ']';
    // Put an invalid row first and reverse the valid keys. A saved entry ID
    // must follow image row order, independent of key, PC, and finite set.
    entries << "\"entries\":[{\"key\":0,\"slot\":0,\"conf\":0,\"valid\":false}";
    for (size_t index = count; index-- > 0;) {
        entries << ",{\"key\":" << ((index + 1) * 2048)
                << ",\"slot\":" << (index % target_count)
                << ",\"conf\":3,\"valid\":true}";
    }
    entries << ']';
    replace("\"cam\":[]", cam.str());
    replace("\"slots\":[]", targets.str());
    replace("\"entries\":[]", entries.str());
    HintIndirect::Config config;
    config.mode = "replay";
    config.ideal = true;
    config.metadataPath = writeInput("hint-test-unlimited.json", text);
    config.outputPrefix = "unlimited-capacity-test";
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    using Table = HintIndirect::Table;
    expect(hardware.historyPCs().size() == count + 1,
           "unlimited CAM and local-history candidates exceed 20 PCs");
    for (const auto table : {Table::Cam, Table::Context, Table::Slots}) {
        const auto occupancy = hardware.occupancy(table);
        expect(occupancy.sampleCycles == 0 && occupancy.avgEntries == 0 &&
               occupancy.peakEntries == 0,
               "resident tables without measurement samples have zero mean/peak");
    }
    expect(hardware.occupancy(Table::Cam).residentEntries == count + 1 &&
           hardware.occupancy(Table::Context).residentEntries == count &&
           hardware.occupancy(Table::Slots).residentEntries == target_count,
           "valid resident counts exclude invalid rows and deduplicate slots");
    hardware.setMeasurementActive(true, 100);
    uint64_t cycle = 101;
    for (size_t index = 0; index < count; ++index) {
        const Addr pc = (index + 1) * 4096;
        auto query = hardware.query(snapshot, pc, pc + 16, cycle++);
        const auto prediction = hardware.complete(query, pc, 0x4000,
                                                   true, true, false, true);
        expect(query->set == 0 && query->tagHit &&
               query->entryId == count - index &&
               query->slot == index % target_count &&
               prediction == std::optional<Addr>(
                   0x80000000 + (index % target_count) * 2),
               "all 5001 keys in one original set survive with stable IDs");
    }
    auto original = hardware.query(snapshot, 0x1000, 0x1010, cycle++);
    hardware.complete(original, 0x1000, 0x6000, true, true, false, true);
    hardware.commit(original, 1, 0x1000, 0x6000,
                    true, original->hintTarget, cycle - 1);
    auto alias = hardware.query(snapshot, 0x20001002, 0x20001012, cycle++);
    hardware.complete(alias, alias->pc, 0x6000, true, true, false, true);
    expect(alias->key == original->key && alias->entryId == original->entryId &&
           alias->conf == 2 && hardware.confidenceAtEntry(1) == 3,
           "true 28-bit alias shares confidence; unrelated reverse ID is intact");
    hardware.commit(alias, 2, alias->pc, alias->hintTarget,
                    true, alias->hintTarget, cycle - 1);
    auto contention = hardware.query(snapshot, 0x1000, 0x3000, cycle++);
    expect(contention->pc == 0x1000 &&
           !hardware.complete(contention, 0x2000, 0x80000002,
                              true, true, false, true),
           "unlimited CAM still cancels a later selected candidate");
    const auto before_idle = hardware.occupancy(Table::Context).sampleCycles;
    hardware.tick(cycle + 10000);
    expect(hardware.occupancy(Table::Context).sampleCycles > before_idle + 9999,
           "idle measurement cycles are included without querying the table");
    hardware.setMeasurementActive(false, cycle + 20000);
    const auto measured = hardware.occupancy(Table::Context);
    expect(measured.sampleCycles == cycle + 20000 - 100 &&
           measured.avgEntries == count && measured.peakEntries == count,
           "frozen mean and peak equal resident count over exact phase boundaries");
    hardware.tick(cycle + 30000);
    expect(hardware.occupancy(Table::Context).sampleCycles == measured.sampleCycles,
           "measurement end stops occupancy sampling");
    hardware.finish();
    expect(readOutput(config.outputPrefix + "/hint-stats.json").find(
        "\"storage\":\"unlimited\"") != std::string::npos,
        "unlimited statistics explicitly identify storage mode");
}

void
testIdealOccupancyAndCollect()
{
    using Table = HintIndirect::Table;
    HintIndirect::Config config;
    config.mode = "replay";
    config.ideal = true;
    config.metadataPath = writeInput("hint-test-ideal-occupancy.json",
                                    idealizeMetadata(metadataText(false)));
    config.outputPrefix = "ideal-occupancy-test";
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    hardware.setMeasurementActive(true, 10);
    std::shared_ptr<HintQuery> latest;
    for (uint64_t cycle = 11; cycle <= 13; ++cycle) {
        latest = hardware.query(snapshot, 0x1000, 0x1010, cycle);
        hardware.complete(latest, 0x1000, 0x6000, true, true, false, true);
        hardware.commit(latest, cycle, 0x1000, 0x6000, true, 0x4000, cycle);
    }
    hardware.tick(14);
    expect(hardware.confidenceAtEntry(latest->entryId) == 0 &&
           hardware.occupancy(Table::Context).residentEntries == 2,
           "confidence-zero valid rows remain resident");
    auto shadow = hardware.query(snapshot, 0x1000, 0x1010, 15);
    hardware.complete(shadow, 0x1000, 0x4000, true, true, false, true);
    hardware.commit(shadow, 15, 0x1000, 0x4000, true, 0x4000, 15);
    hardware.resetStats(15);
    const auto reset = hardware.occupancy(Table::Context);
    expect(reset.residentEntries == 2 && reset.sampleCycles == 0 &&
           reset.avgEntries == 0 && reset.peakEntries == 0 &&
           hardware.pendingUpdates() == 1,
           "reset preserves frozen storage and FIFO but clears occupancy samples");
    hardware.tick(115);
    expect(hardware.confidenceAtEntry(shadow->entryId) == 1 &&
           hardware.occupancy(Table::Context).sampleCycles == 100,
           "carried shadow update recovers and idle sampling restarts at reset");
    hardware.setMeasurementActive(false, 125);
    hardware.finish();
    expect(hardware.occupancy(Table::Context).sampleCycles == 110,
           "occupancy records exact post-reset measure duration");

    config.outputPrefix = "ideal-no-samples-test";
    HintIndirect no_samples(config, nullptr);
    no_samples.setMeasurementActive(true, 100);
    no_samples.setMeasurementActive(false, 100);
    no_samples.finish();
    expect(no_samples.occupancy(Table::Context).sampleCycles == 0 &&
           no_samples.occupancy(Table::Context).avgEntries == 0 &&
           no_samples.occupancy(Table::Context).peakEntries == 0,
           "zero-length measure never fabricates an occupancy sample");

    config.outputPrefix = "ideal-empty-test";
    config.metadataPath = writeInput("hint-test-ideal-empty.json",
                                    idealizeMetadata(metadataText(true)));
    HintIndirect empty(config, nullptr);
    empty.setMeasurementActive(true, 10);
    empty.setMeasurementActive(false, 20);
    empty.finish();
    for (const auto table : {Table::Cam, Table::Context, Table::Slots}) {
        const auto occupancy = empty.occupancy(table);
        expect(occupancy.residentEntries == 0 && occupancy.avgEntries == 0 &&
               occupancy.peakEntries == 0 && occupancy.sampleCycles == 10,
               "empty table still records the measurement duration");
    }

    config.mode = "collect";
    config.metadataPath.clear();
    config.outputPrefix = "ideal-collect-test";
    std::ostringstream candidates;
    candidates << "{\"format\":\"h2p-hint-candidates\",\"version\":3,"
        "\"storage\":\"unlimited\",\"source\":{},\"pcs\":[";
    for (unsigned index = 0; index < 50; ++index) {
        if (index) candidates << ',';
        candidates << "\"0x" << std::hex << (0x1000 + index * 2) << '"';
    }
    candidates << "]}";
    config.candidatePath = writeInput("hint-test-ideal-candidates-50.json",
                                     candidates.str());
    HintIndirect collect(config, nullptr);
    collect.setMeasurementActive(true, 10);
    const auto query = collect.query(snapshotFor(collect), 0x1000, 0x1100, 11);
    expect(query->hasCandidate && query->pc == 0x1000,
           "unlimited collection keeps earliest-candidate arbitration metadata");
    expect(!collect.complete(query, 0x1062, 0x4000,
                             true, true, false, true),
           "unlimited collect never changes the prediction");
    collect.commit(query, 1, 0x1062, 0x4000, true, 0x4000, 12);
    collect.commit(nullptr, 2, 0x1000, 0x5000, false, 0, 13);
    collect.setMeasurementActive(false, 20);
    collect.finish();
    for (const auto table : {Table::Cam, Table::Context, Table::Slots}) {
        const auto occupancy = collect.occupancy(table);
        expect(occupancy.residentEntries == 0 && occupancy.sampleCycles == 0,
               "collect has no comparable resident hardware table occupancy");
    }
    const auto profile = readGzip(config.outputPrefix + "/hint-profile.jsonl.gz");
    expect(profile.find("\"version\":3") != std::string::npos &&
           profile.find("\"storage\":\"unlimited\"") != std::string::npos &&
           profile.find("\"pc\":\"0x1062\"") != std::string::npos &&
           profile.find("\"contexts\":null") != std::string::npos,
           "v3 collection retains later candidate and absent-context observations");
}

[[maybe_unused]] void
testFiniteIdealEquivalence()
{
    HintIndirect::Config finite_config;
    finite_config.mode = "replay";
    finite_config.metadataPath = writeInput("hint-equivalent-finite.json",
                                            metadataText(false));
    finite_config.outputPrefix = "equivalent-finite-test";
    auto ideal_config = finite_config;
    ideal_config.ideal = true;
    ideal_config.metadataPath = writeInput("hint-equivalent-ideal.json",
                                           idealizeMetadata(metadataText(false)));
    ideal_config.outputPrefix = "equivalent-ideal-test";
    HintIndirect finite(finite_config, nullptr);
    HintIndirect ideal(ideal_config, nullptr);
    const auto finite_snapshot = snapshotFor(finite);
    const auto ideal_snapshot = snapshotFor(ideal);
    finite.setMeasurementActive(true, 100);
    ideal.setMeasurementActive(true, 100);
    for (uint64_t cycle = 101; cycle < 201; ++cycle) {
        const Addr pc = cycle % 2 ? 0x1000 : 0x2000;
        const Addr expected = pc == 0x1000 ? 0x4000 : 0x5000;
        const Addr actual = cycle % 7 < 4 ? expected : 0x6000;
        const bool btb = cycle % 11 != 0;
        const bool is_return = cycle % 13 == 0;
        const bool taken = cycle % 17 != 0;
        auto a = finite.query(finite_snapshot, pc, pc + 16, cycle);
        auto b = ideal.query(ideal_snapshot, pc, pc + 16, cycle);
        const auto a_target = finite.complete(a, pc, 0x6000,
                                              taken, true, is_return, btb);
        const auto b_target = ideal.complete(b, pc, 0x6000,
                                             taken, true, is_return, btb);
        expect(a->key == b->key && a->conf == b->conf &&
               a->tagHit == b->tagHit && a->eligible == b->eligible &&
               a->overridden == b->overridden && a_target == b_target,
               "finite-fitting mapping preserves each query and S3 selection");
        // Both streams omit returns and a deterministic wrong-path subset.
        if (!is_return && cycle % 5) {
            const Addr final_target = a_target.value_or(0x6000);
            finite.commit(a, cycle, pc, actual, taken, final_target, cycle);
            ideal.commit(b, cycle, pc, actual, taken, final_target, cycle);
        }
    }
    finite.setMeasurementActive(false, 201);
    ideal.setMeasurementActive(false, 201);
    finite.finish();
    ideal.finish();
    const auto &a = finite.totals();
    const auto &b = ideal.totals();
    expect(a.executions == b.executions && a.baselineMisses == b.baselineMisses &&
           a.finalMisses == b.finalMisses && a.corrected == b.corrected &&
           a.harmed == b.harmed && a.eligible == b.eligible &&
           a.tagHits == b.tagHits && a.confEnabled == b.confEnabled &&
           a.overrides == b.overrides &&
           finite.queueCounts().applied == ideal.queueCounts().applied &&
           finite.queueCounts().enqueued == ideal.queueCounts().enqueued &&
           finite.queueCounts().dropped == ideal.queueCounts().dropped &&
           finite.queueCounts().zeroTransitions == ideal.queueCounts().zeroTransitions &&
           finite.queueCounts().recoveries == ideal.queueCounts().recoveries &&
           finite.pendingUpdates() == ideal.pendingUpdates(),
           "equivalent mappings preserve retirement and FIFO outcomes");
}

void
testPredictionBounds(bool ideal, bool collect)
{
    constexpr Addr last = std::numeric_limits<Addr>::max();
    // These PCs exercise both ends of the unsigned address space. The Context
    // fixture fits two ways even when the low and high PCs share a set.
    const std::array<Addr, 6> pcs = {
        0, 0x20, 0x1008, last - 63, last - 31, last - 1};
    std::ostringstream cam, candidates, entries;
    cam << "\"cam\":[";
    candidates << "{\"format\":\"h2p-hint-candidates\",\"version\":2,"
                  "\"source\":{},\"pcs\":[";
    entries << "\"entries\":[";
    std::map<unsigned, unsigned> ways;
    for (size_t index = 0; index < pcs.size(); ++index) {
        if (index) {
            cam << ',';
            candidates << ',';
            entries << ',';
        }
        cam << "{\"pc\":\"0x" << std::hex << pcs[index]
            << "\",\"mask\":0,\"valid\":true}";
        candidates << "\"0x" << std::hex << pcs[index] << '"';
        const auto key = foldWord((pcs[index] >> 1) & HintAddressWordMask);
        const unsigned set = key & 2047;
        const unsigned way = ways[set]++;
        expect(way < 2, "bounds fixture fits the finite Context table");
        entries << "{\"set\":" << set << ",\"way\":" << way
                << ",\"tag\":" << (key >> 11)
                << ",\"slot\":0,\"conf\":3,\"valid\":true}";
    }
    cam << ']';
    candidates << "]}";
    entries << ']';
    auto metadata = metadataText(true);
    const auto replace = [&metadata](const std::string &old_text,
                                      const std::string &new_text) {
        const auto position = metadata.find(old_text);
        expect(position != std::string::npos, "bounds metadata fixture field");
        metadata.replace(position, old_text.size(), new_text);
    };
    replace("\"cam\":[]", cam.str());
    replace("\"slots\":[]", "\"slots\":[\"0x4000\"]");
    replace("\"entries\":[]", entries.str());
    HintIndirect::Config config;
    config.mode = collect ? "collect" : "replay";
    config.ideal = ideal;
    config.outputPrefix = std::string(ideal ? "ideal-" : "finite-") +
                          (collect ? "collect-bounds-test" : "replay-bounds-test");
    config.candidatePath = writeInput(config.outputPrefix + "-candidates.json",
        ideal ? idealizeCandidates(candidates.str()) : candidates.str());
    if (!collect) {
        config.metadataPath = writeInput(config.outputPrefix + "-metadata.json",
            ideal ? idealizeMetadata(metadata) : metadata);
    }
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    const auto original_contexts = snapshot->contextsFor(0x20);
    hardware.setMeasurementActive(true, 100);

    auto seed = hardware.query(snapshot, 0x20, 0x40, 101);
    const auto seed_target = hardware.complete(seed, 0x20, 0x6000,
                                                true, true, false, true);
    hardware.commit(seed, 1, 0x20, 0x6000, true,
                    seed_target.value_or(0x6000), 101);
    expect(hardware.pendingUpdates() == (collect ? 0 : 1),
           "bounds fixture has one pending confidence update only in replay");

    // This is the BPU/ABTB/MBTB end construction for a 64-byte block.
    const auto block_end = [](Addr start) {
        return (start + Addr(64)) & ~Addr(31);
    };
    struct BoundsCase
    {
        Addr start;
        Addr end;
        std::optional<Addr> expected;
    };
    const std::array<BoundsCase, 12> cases = {{
        {last - 15, block_end(last - 15), std::nullopt},
        {last - 63, block_end(last - 63), std::nullopt},
        {last - 1, block_end(last - 1), std::nullopt},
        {last, block_end(last), std::nullopt},
        {0, 0, std::nullopt},
        {0x20, 0x20, std::nullopt},
        {0x1008, 0x20, std::nullopt},
        {last, last, std::nullopt},
        {last - 95, block_end(last - 95), last - 63},
        {last - 31, last, last - 31},
        {last - 1, last, last - 1},
        {0, 0x20, Addr(0)},
    }};
    uint64_t cycle = 102;
    unsigned expected_hits = 1; // The seed query above.
    for (const auto &bounds : cases) {
        std::optional<Addr> ordered_match;
        for (const auto pc : pcs) {
            if (pc >= bounds.start && pc < bounds.end &&
                (!ordered_match || pc < *ordered_match)) {
                ordered_match = pc;
            }
        }
        expect(ordered_match == bounds.expected,
               "bounds expectation agrees with original BTB range predicates");
        auto query = hardware.query(snapshot, bounds.start, bounds.end, cycle);
        expect(query && query->snapshot == snapshot &&
               query->queryCycle == cycle && query->blockStart == bounds.start &&
               query->blockEnd == bounds.end,
               "all bounds retain prediction-time metadata and normal cycle identity");
        expect(query->hasCandidate == bounds.expected.has_value() &&
               (!bounds.expected || query->pc == *bounds.expected),
               "finite and ideal use the same ordered half-open CAM range");
        expect(query->tagHit == (!collect && bounds.expected.has_value()),
               "empty and wrapped bounds cannot access a Context entry");
        const Addr selected = bounds.expected.value_or(0x20);
        const bool taken = bounds.expected.has_value();
        const auto target = hardware.complete(query, selected, 0x4000,
                                               taken, true, false, taken);
        expect(bool(target) == (!collect && bounds.expected.has_value()),
               "invalid bounds preserve the original target without override");
        if (!bounds.expected) {
            expect(query->entryId == std::numeric_limits<size_t>::max() &&
                   !query->overridden,
                   "no saved table identity is fabricated for empty ranges");
            // A wrapped wrong-path fetch is squashed rather than committed.
            // Collect must retain the snapshot without inventing eligibility.
            expect(!query->eligible,
                   "no BTB-selected branch means no valid query eligibility");
        } else {
            ++expected_hits;
        }
        expect(hardware.pendingUpdates() == 0 &&
               hardware.queueCounts().enqueued == (collect ? 0 : 1) &&
               hardware.queueCounts().applied == (collect ? 0 : 1),
               "wrapped query drains prior ready write but never scores a new entry");
        if (!collect) {
            expect(hardware.confidenceAtEntry(seed->entryId) == 2,
                   "bounds handling leaves trained confidence unchanged");
        }
        ++cycle;
    }
    expect(snapshot->contextsFor(0x20) == original_contexts,
           "query bounds do not mutate speculative history snapshots");
    hardware.setMeasurementActive(false, cycle);
    hardware.finish();
    boost::property_tree::ptree stats;
    std::istringstream stats_input(readOutput(config.outputPrefix +
                                               "/hint-stats.json"));
    boost::property_tree::read_json(stats_input, stats);
    expect(stats.get<unsigned>("query.lookups") == cases.size() + 1 &&
           stats.get<unsigned>("query.cam_hits") == expected_hits &&
           stats.get<unsigned>("query.tag_hits") == (collect ? 0 : expected_hits),
           "every range consumes one query attempt, with no hidden second CAM lookup");
}

void
testIdealInvalidEntry()
{
    auto text = idealizeMetadata(metadataText(false));
    const std::string valid =
        "\"key\":2048,\"slot\":0,\"conf\":3,\"valid\":true";
    const auto position = text.find(valid);
    expect(position != std::string::npos, "ideal invalid-entry fixture key");
    text.replace(position, valid.size(),
        "\"key\":2048,\"slot\":0,\"conf\":0,\"valid\":false");
    HintIndirect::Config config;
    config.mode = "replay";
    config.ideal = true;
    config.metadataPath = writeInput("hint-test-ideal-invalid-entry.json", text);
    config.outputPrefix = "ideal-invalid-entry-test";
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    hardware.setMeasurementActive(true, 0);

    // A neighbouring valid key makes this a nonempty Context table. Exercise
    // both the invalid key's own PC and a second PC sharing its 28-bit hash.
    for (uint64_t cycle = 1; cycle <= 4; ++cycle) {
        const Addr pc = cycle % 2 ? 0x1000 : 0x20001002;
        const Addr actual = cycle <= 2 ? 0x4000 : 0x6000;
        auto query = hardware.query(snapshot, pc, pc + 16, cycle);
        expect(query->hasCandidate && query->key == 2048 && !query->tagHit &&
               query->entryId == std::numeric_limits<size_t>::max(),
               "invalid ideal key and its alias cannot produce a saved table hit");
        expect(!hardware.complete(query, pc, actual,
                                  true, true, false, true),
               "invalid ideal entry cannot override even when its target is correct");
        hardware.commit(query, cycle, pc, actual, true, actual, cycle);
        expect(hardware.pendingUpdates() == 0 &&
               hardware.queueCounts().enqueued == 0 &&
               hardware.queueCounts().dropped == 0 &&
               hardware.totals().tagHits == 0 &&
               hardware.confidenceAtEntry(0) == 0,
               "committed correct and wrong target observations cannot score invalid ideal entry");
    }
    hardware.tick(10);
    expect(hardware.queueCounts().applied == 0 &&
           hardware.queueCounts().recoveries == 0 &&
           hardware.confidenceAtEntry(0) == 0 &&
           hardware.occupancy(HintIndirect::Table::Context).residentEntries == 1,
           "invalid ideal row stays disabled and unoccupied after future ticks");
    auto valid_query = hardware.query(snapshot, 0x2000, 0x2010, 11);
    expect(valid_query->tagHit && valid_query->entryId == 1 &&
           valid_query->conf == 3 && valid_query->hintTarget == 0x5000 &&
           hardware.complete(valid_query, 0x2000, 0x6000,
                             true, true, false, true) == std::optional<Addr>(0x5000),
           "invalid ideal key does not disable the neighbouring valid entry");
    hardware.commit(valid_query, 5, 0x2000, 0x5000, true, 0x5000, 11);
    hardware.tick(12);
    expect(hardware.totals().executions == 5 &&
           hardware.totals().tagHits == 1 &&
           hardware.queueCounts().enqueued == 1 &&
           hardware.queueCounts().applied == 1 &&
           hardware.pendingUpdates() == 0 &&
           hardware.confidenceAtEntry(0) == 0 &&
           hardware.confidenceAtEntry(1) == 3,
           "only the valid ideal entry participates in conserved confidence updates");
    hardware.setMeasurementActive(false, 12);
    hardware.finish();
}

void
testMixedConfidenceFIFO(bool ideal)
{
    HintIndirect::Config config;
    config.mode = "replay";
    config.ideal = ideal;
    config.outputPrefix = ideal ? "ideal-mixed-fifo-test" : "finite-mixed-fifo-test";
    config.metadataPath = writeInput(config.outputPrefix + "-metadata.json",
        ideal ? idealizeMetadata(metadataText(false)) : metadataText(false));
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    hardware.setMeasurementActive(true, 0);

    struct Score
    {
        Addr pc;
        Addr actual;
    };
    const std::array<Score, 9> scores = {{
        {0x1000, 0x6000}, // A -1: 3 -> 2.
        {0x2000, 0x6000}, // B -1: 3 -> 2.
        {0x1000, 0x4000}, // A +1: 2 -> 3.
        {0x2000, 0x6000}, // B -1: 2 -> 1.
        {0x1000, 0x6000}, // A -1: 3 -> 2.
        {0x2000, 0x5000}, // B +1: 1 -> 2.
        {0x1000, 0x6000}, // A -1: 2 -> 1.
        {0x2000, 0x6000}, // B -1: 2 -> 1.
        {0x2000, 0x5000}, // New B +1 must be dropped, not replace an old update.
    }};
    const std::array<std::array<unsigned, 2>, 8> after_write = {{{2, 3},
        {2, 2}, {3, 2}, {3, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}}};
    std::array<std::shared_ptr<HintQuery>, 9> queries;
    for (size_t index = 0; index < scores.size(); ++index) {
        const Addr pc = scores[index].pc;
        const Addr target = pc == 0x1000 ? 0x4000 : 0x5000;
        queries[index] = hardware.query(snapshot, pc, pc + 16, index + 1);
        expect(queries[index]->tagHit && queries[index]->conf == 3 &&
               hardware.complete(queries[index], pc, 0x6000,
                                 true, true, false, true) == std::optional<Addr>(target),
               "mixed FIFO queries save both original entry identities and targets");
    }
    const size_t entry_a = queries[0]->entryId;
    const size_t entry_b = queries[1]->entryId;
    expect(entry_a != entry_b, "mixed FIFO uses two distinct Context entries");
    hardware.tick(20);
    for (size_t index = 0; index < scores.size(); ++index) {
        hardware.commit(queries[index], index + 1, scores[index].pc,
                        scores[index].actual, true, queries[index]->hintTarget, 20);
        expect(hardware.totals().executions == index + 1,
               "FIFO-full commit returns normally and accounts for every branch");
    }
    expect(hardware.pendingUpdates() == 8 &&
           hardware.queueCounts().enqueued == 8 &&
           hardware.queueCounts().dropped == 1 &&
           hardware.queueCounts().applied == 0,
           "mixed FIFO accepts eight updates and drops the new ninth before any write");

    // Read in the commit cycle while the FIFO is full: neither +/- update
    // bypasses the table or writes before the next CPU cycle.
    auto same_cycle = hardware.query(snapshot, 0x1000, 0x1010, 20);
    expect(same_cycle->entryId == entry_a && same_cycle->conf == 3 &&
           hardware.confidenceAtEntry(entry_a) == 3 &&
           hardware.confidenceAtEntry(entry_b) == 3 &&
           hardware.pendingUpdates() == 8,
           "queued mixed scores never bypass a same-cycle prediction read");
    std::ostringstream observations;
    observations << "{\"format\":\"hint-mixed-fifo-driver\",\"ideal\":"
                 << (ideal ? "true" : "false")
                 << ",\"entry_a\":" << entry_a << ",\"entry_b\":" << entry_b
                 << ",\"commit_cycle\":20,\"reads\":["
                 << "{\"cycle\":20,\"a\":" << hardware.confidenceAtEntry(entry_a)
                 << ",\"b\":" << hardware.confidenceAtEntry(entry_b)
                 << ",\"applied\":" << hardware.queueCounts().applied
                 << ",\"pending\":" << hardware.pendingUpdates() << '}';
    for (size_t index = 0; index < after_write.size(); ++index) {
        const uint64_t cycle = 21 + index;
        const Addr pc = scores[index].pc;
        auto query = hardware.query(snapshot, pc, pc + 16, cycle);
        const unsigned expected_conf = after_write[index][pc == 0x1000 ? 0 : 1];
        expect(query->conf == expected_conf &&
               query->entryId == (pc == 0x1000 ? entry_a : entry_b) &&
               query->hintTarget == (pc == 0x1000 ? 0x4000 : 0x5000) &&
               hardware.confidenceAtEntry(entry_a) == after_write[index][0] &&
               hardware.confidenceAtEntry(entry_b) == after_write[index][1],
               "each mixed score writes its saved target entry in FIFO order before lookup");
        hardware.tick(cycle);
        expect(hardware.queueCounts().applied == index + 1 &&
               hardware.pendingUpdates() == 7 - index &&
               hardware.queueCounts().enqueued == 8 &&
               hardware.queueCounts().dropped == 1 &&
               hardware.confidenceAtEntry(entry_a) == after_write[index][0] &&
               hardware.confidenceAtEntry(entry_b) == after_write[index][1],
               "repeated same-cycle ticks cannot issue an additional confidence write");
        observations << ",{\"cycle\":" << cycle
                     << ",\"a\":" << hardware.confidenceAtEntry(entry_a)
                     << ",\"b\":" << hardware.confidenceAtEntry(entry_b)
                     << ",\"applied\":" << hardware.queueCounts().applied
                     << ",\"pending\":" << hardware.pendingUpdates() << '}';
    }
    hardware.tick(100);
    expect(hardware.confidenceAtEntry(entry_a) == 1 &&
           hardware.confidenceAtEntry(entry_b) == 1 &&
           hardware.queueCounts().applied == 8 &&
           hardware.pendingUpdates() == 0 &&
           hardware.totals().tagHits == 9 &&
           hardware.queueCounts().enqueued + hardware.queueCounts().dropped ==
               hardware.totals().tagHits,
           "dropped newest positive score never writes later; all scores conserve exactly");
    for (const auto &query : queries) {
        expect(query->conf == 3,
               "queued prediction confidence snapshots do not mutate with writeback");
    }
    observations << "],\"enqueued\":" << hardware.queueCounts().enqueued
                 << ",\"dropped\":" << hardware.queueCounts().dropped
                 << ",\"final_applied\":" << hardware.queueCounts().applied << "}\n";
    writeInput(config.outputPrefix + "-evidence.json", observations.str());
    hardware.setMeasurementActive(false, 100);
    hardware.finish();
}

void
testEarlierTakenCancellation(bool ideal)
{
    HintIndirect::Config config;
    config.mode = "replay";
    config.ideal = ideal;
    config.outputPrefix = ideal ? "ideal-earlier-taken-test" :
                                 "finite-earlier-taken-test";
    config.metadataPath = writeInput(config.outputPrefix + "-metadata.json",
        ideal ? idealizeMetadata(metadataText(false)) : metadataText(false));
    HintIndirect hardware(config, nullptr);
    const auto snapshot = snapshotFor(hardware);
    hardware.setMeasurementActive(true, 0);
    struct EarlierBranch
    {
        bool indirect;
        bool isReturn;
    };
    const std::array<EarlierBranch, 3> earlier = {{
        {false, false}, {true, true}, {true, false}}};
    for (size_t index = 0; index < earlier.size(); ++index) {
        const uint64_t cycle = index + 1;
        auto query = hardware.query(snapshot, 0xff0, 0x1010, cycle);
        expect(query->pc == 0x1000 && query->hasCandidate && query->tagHit &&
               query->conf == 3 && query->hintTarget == 0x4000,
               "earlier-taken fixture has a valid enabled shadow Hint target");
        const auto target = hardware.complete(query, 0xff8, 0x7000,
            true, earlier[index].indirect, earlier[index].isReturn, true);
        expect(query->baselinePC < query->pc && !target &&
               !query->overridden && !query->eligible,
               "earlier selected direct/return/indirect branch cancels later CAM takeover");
        // The real caller retires only non-return indirect branches into the
        // helper. An earlier indirect may commit, but cannot score the later
        // candidate's otherwise valid shadow prediction.
        if (earlier[index].indirect && !earlier[index].isReturn) {
            hardware.commit(query, cycle, 0xff8, 0x7000, true, 0x7000, cycle);
        }
        hardware.tick(cycle);
        expect(hardware.pendingUpdates() == 0 &&
               hardware.queueCounts().enqueued == 0 &&
               hardware.confidenceAtEntry(query->entryId) == 3,
               "earlier branch cannot train the later candidate's Context entry");
    }
    hardware.setMeasurementActive(false, 4);
    hardware.finish();
    boost::property_tree::ptree stats;
    std::istringstream input(readOutput(config.outputPrefix + "/hint-stats.json"));
    boost::property_tree::read_json(input, stats);
    expect(stats.get<unsigned>("query.lookups") == 3 &&
           stats.get<unsigned>("query.cam_hits") == 3 &&
           stats.get<unsigned>("query.tag_hits") == 3 &&
           stats.get<unsigned>("query.overrides") == 0 &&
           stats.get<unsigned>("query.cancel_not_indirect") == 1 &&
           stats.get<unsigned>("query.cancel_return") == 1 &&
           stats.get<unsigned>("query.cancel_pc_mismatch") == 1 &&
           stats.get<unsigned>("global.executions") == 1 &&
           stats.get<unsigned>("global.tag_hits") == 0,
           "earlier-taken cases use one query per block and no committed Hint score");
}

uint64_t
vectorNumber(const std::string &value)
{
    return std::stoull(value, nullptr, 0);
}

void
testFiniteV3CollectObserverHistory()
{
    HintFiniteHistory state;
    state.configure({0x1000});
    state.update(0x1000, 0x2000, true, false, false, true);
    const HintSnapshot snapshot(state, true);
    expect(snapshot.collectHistory && snapshot.finite,
           "finite collect keeps an exact observer-only history snapshot");
    const auto original = snapshot.v3ContextsFor(0);
    // Collect may not stall the BPU. Exercise both local ring wrap and more
    // than 64 younger global writes, which replay must instead backpressure.
    for (unsigned n = 0; n < 105; ++n)
        state.update(0x1000, 0x2000 + (n & 3) * 2,
                     true, n % 9 == 0, false, true);
    state = *snapshot.collectHistory;
    expect(state.snapshot().low128 == snapshot.finite->low128 &&
           state.snapshot().localLow16 == snapshot.finite->localLow16 &&
           HintSnapshot(state, true).v3ContextsFor(0) == original,
           "collect observer restores exact ITH/LHR/Call/Ret after ring wrap");
}

void
testFiniteV3Vectors(const std::string &path)
{
    boost::property_tree::ptree vectors;
    boost::property_tree::read_json(path, vectors);
    expect(vectors.get<unsigned>("version") == 7 &&
           vectors.get<std::string>("context_encoding") ==
               "xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1",
           "spec v7 vectors have frozen version and encoding");
    for (const auto &node : vectors.get_child("hash")) {
        const auto &row = node.second;
        auto component = [&](const char *name) {
            std::string text = row.get<std::string>(name);
            if (text.substr(0, 2) == "0x") text.erase(0, 2);
            std::array<uint64_t, 2> words{};
            words[0] = std::stoull(text.substr(text.size() > 16 ?
                                              text.size() - 16 : 0), nullptr, 16);
            if (text.size() > 16)
                words[1] = std::stoull(text.substr(0, text.size() - 16), nullptr, 16);
            return hintEncode128(words);
        };
        HintV3Contexts components = {component("ith_low128"),
            component("call_low128"), component("ret_low128"),
            hintEncodeWord(vectorNumber(row.get<std::string>("lhr_low16")))};
        unsigned index = 0;
        for (const auto &value : row.get_child("components"))
            expect(index < 4 && components[index++] == value.second.get_value<uint32_t>(),
                   "v7 independent component vector matches");
        expect(index == 4, "four spec context components");
        const auto key = hintHashV3(vectorNumber(row.get<std::string>("pc")),
                                   components, row.get<unsigned>("mask"));
        expect(key == row.get<uint32_t>("key") &&
               (key & 2047) == row.get<unsigned>("set") &&
               (key >> 11) == row.get<unsigned>("tag"),
               "spec key/set/tag match frozen Python oracle vectors");
    }
    verifyHintContextForkHistory();
}

std::string
finiteV3Metadata(unsigned cam_count = 1, unsigned mask = 0)
{
    const std::string digest(64, '0');
    std::ostringstream out;
    out << "{\"format\":\"h2p-hint\",\"version\":7,"
           "\"storage\":\"finite\",\"hash_version\":7,"
           "\"context_encoding\":\"xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1\","
           "\"mask_order\":[\"ITH\",\"Call\",\"Ret\",\"LHR\"],"
           "\"history\":{\"ith_entries\":128,\"ith_use_bits\":128,"
           "\"lhr_entries\":16,\"lhr_bits\":32,\"lhr_use_bits\":16,\"lhr_head_bits\":5,"
           "\"call_entries\":128,\"ret_entries\":128,"
           "\"path_use_bits\":128,\"target_shift\":1},"
           "\"layout\":{\"cam_entries\":16,\"context_entries\":4096,"
           "\"ways\":2,\"slot_entries\":32,\"conf_queue_depth\":32,"
           "\"hint_meta_entries\":16,\"lhr_max_uncommitted\":8},"
           "\"cam\":[";
    for (unsigned i = 0; i < cam_count; ++i) {
        if (i)
            out << ',';
        out << "{\"pc\":\"0x" << std::hex << (0x1000 + i * 0x80)
            << "\",\"mask\":" << std::dec << mask << '}';
    }
    out << "],\"slots\":[\"0x2000\"],\"entries\":[";
    for (unsigned i = 0; i < cam_count; ++i) {
        if (i) out << ',';
        const uint32_t key = hintHashV3(0x1000 + i * 0x80,
                                       HintV3Contexts{}, mask);
        out << "{\"set\":" << (key & 2047)
            << ",\"way\":0,\"tag\":" << (key >> 11)
            << ",\"slot\":0,\"conf\":3}";
    }
    out << "],\"source\":{\"workload_id\":\"hint-self-test\","
           "\"checkpoint_sha256\":\"" << digest
        << "\",\"profile_sha256\":\"" << digest
        << "\",\"builder_sha256\":\"" << digest << "\"}}";
    return out.str();
}

HintIndirect::Config
finiteV3Config(const std::string &prefix, unsigned cam_count = 1,
               unsigned mask = 0)
{
    HintIndirect::Config config;
    config.mode = "replay";
    config.confQueueDepth = 32;
    config.outputPrefix = prefix;
    config.metadataPath = writeInput(prefix + "-metadata.json",
                                     finiteV3Metadata(cam_count, mask));
    return config;
}

void
testFiniteV3Predictor()
{
    HintIndirect hardware(finiteV3Config("finite-v3-predictor"), nullptr);
    HintFiniteHistory history;
    history.configure(hardware.historyPCs());
    hardware.setMeasurementActive(true, 0);
    std::array<std::shared_ptr<HintQuery>, 4> queries;
    std::shared_ptr<const HintSnapshot> first_snapshot;
    for (unsigned i = 0; i < queries.size(); ++i) {
        auto snapshot = std::make_shared<const HintSnapshot>(history);
        if (!i)
            first_snapshot = snapshot;
        auto query = hardware.query(snapshot, 0x1000, 0x1040, i + 1);
        const auto target = hardware.complete(query, 0x1000, 0x3000,
                                               true, true, false, true);
        expect(query->key == 0x800 && query->set == 0 && query->tagHit &&
               query->conf == 3 - i &&
               bool(target) == (i < 3) &&
               (i == 3 || *target == 0x2000),
               "v3 pending confidence forwarding reaches conf0 fallback");
        expect(hardware.canReserve(0x1000),
               "fourth same-LHR write is still accepted");
        hardware.reserve(query, 0x1000, history.localPhase(0));
        history.update(0x1000, target.value_or(0x3000),
                       true, false, false, true);
        hardware.bind(query, i + 1);
        queries[i] = query;
        if (i < 3)
            hardware.resolve(query, 0x1000, 0x3000, i + 1, i + 1);
    }
    expect(hardware.canReserve(0x1000) &&
               hardware.reserveBlockReason(0x1000) ==
                   HintIndirect::StallReason::None,
           "no-LHR mask does not block the fifth same-PC prediction");
    hardware.tick(5);
    hardware.tick(6);
    fatal_if(hardware.confidenceAtEntry(queries[0]->entryId) != 2 ||
             hardware.pendingUpdates() != 2,
             "Hint self-test: single-port expected conf=2/pending=2, "
             "got conf=%u/pending=%u",
             hardware.confidenceAtEntry(queries[0]->entryId),
             unsigned(hardware.pendingUpdates()));
    hardware.redirect(history, 1, 0x1000, true, 0x2000, 1, 6);
    history.restore(*first_snapshot->finite);
    history.update(0x1000, 0x2000, true, false, false, true);
    expect(history.low64(history.ith) == 0 &&
           history.localLow16(0) == 0 &&
           hardware.confidenceAtEntry(queries[0]->entryId) == 3 &&
           hardware.pendingUpdates() == 1 &&
           hardware.queueCounts().reverted == 1 &&
           hardware.queueCounts().canceled == 2 &&
           hardware.canReserve(0x1000),
           "H2P redirect reverses younger local writes and applied scores");
    const auto after = hardware.query(std::make_shared<const HintSnapshot>(history),
                                      0x1000, 0x1040, 7);
    expect(after->conf == 3 && after->tagHit,
           "corrected shadow score forwards before table write");
    hardware.commitTarget(1);
    hardware.tick(8);
    hardware.tick(9);
    expect(hardware.pendingUpdates() == 0 &&
           hardware.confidenceAtEntry(queries[0]->entryId) == 3,
           "committed corrected request writes independently of metadata release");
    hardware.commit(queries[0], 1, 0x1000, 0x2000, true, 0x2000, 9);
    expect(hardware.totals().corrected == 1 && hardware.totals().harmed == 0,
           "replay target triple records one real dynamic correction");
    hardware.finish();
}

void
testFiniteV3MaskAwareAdmission()
{
    HintIndirect no_lhr(finiteV3Config("finite-v3-no-lhr"), nullptr);
    HintFiniteHistory no_lhr_history;
    no_lhr_history.configure(no_lhr.historyPCs());
    no_lhr.setMeasurementActive(true, 0);
    const auto before = no_lhr_history.snapshot();
    const auto before_phase = no_lhr_history.localPhase(0);
    for (unsigned i = 0; i < 5; ++i) {
        const auto query = no_lhr.query(
            std::make_shared<const HintSnapshot>(no_lhr_history),
            0x1000, 0x1040, i + 1);
        no_lhr.complete(query, 0x1000, 0x3000,
                        true, true, false, true);
        expect(no_lhr.canReserve(0x1000),
               "no-LHR mask accepts same-PC writes beyond four in flight");
        no_lhr.reserve(query, 0x1000, no_lhr_history.localPhase(0));
        no_lhr_history.update(0x1000, 0x2000 + 2 * i,
                              true, false, false, true);
        no_lhr.bind(query, i + 1);
    }
    expect(no_lhr.inFlightHints() == 5 &&
               no_lhr_history.localPhase(0) == ((before_phase + 5) & 31) &&
               no_lhr.reserveBlockReason(0x1000) ==
                   HintIndirect::StallReason::None,
           "fifth no-LHR prediction reserves metadata and writes physical LHR");
    no_lhr.redirect(no_lhr_history, 1, 0x800, false, 0, 0, 5);
    no_lhr_history.restore(before);
    expect(no_lhr.inFlightHints() == 0 &&
               no_lhr_history.localPhase(0) == before_phase,
           "redirect releases five no-LHR reservations and restores phase");
    for (unsigned i = 0; i < 16; ++i) {
        auto query = no_lhr.query(
            std::make_shared<const HintSnapshot>(no_lhr_history),
            0x1000, 0x1040, i + 6);
        no_lhr.complete(query, 0x1000, 0x3000,
                        true, true, false, true);
        expect(no_lhr.canReserve(0x1000),
               "no-LHR admission remains bounded by available metadata");
        no_lhr.reserve(query, 0x1000, no_lhr_history.localPhase(0));
        no_lhr_history.update(0x1000, 0x2000,
                              true, false, false, true);
        no_lhr.bind(query, i + 1);
    }
    expect(!no_lhr.canReserve(0x1000) &&
               no_lhr.reserveBlockReason(0x1000) ==
                   HintIndirect::StallReason::MetaQueue,
           "no-LHR mask still stalls when all 16 metadata entries are used");
    no_lhr.commitTarget(1);
    expect(no_lhr.canReserve(0x1000),
           "commit releases one no-LHR metadata reservation");
    no_lhr.finish();

    HintIndirect reads_lhr(finiteV3Config("finite-v3-reads-lhr", 1, 2),
                           nullptr);
    HintFiniteHistory reads_lhr_history;
    reads_lhr_history.configure(reads_lhr.historyPCs());
    reads_lhr.setMeasurementActive(true, 0);
    for (unsigned i = 0; i < 4; ++i) {
        auto query = reads_lhr.query(
            std::make_shared<const HintSnapshot>(reads_lhr_history),
            0x1000, 0x1040, i + 1);
        reads_lhr.complete(query, 0x1000, 0x3000,
                           true, true, false, true);
        expect(reads_lhr.canReserve(0x1000),
               "LHR-reading mask accepts the first four writes");
        reads_lhr.reserve(query, 0x1000,
                          reads_lhr_history.localPhase(0));
        reads_lhr_history.update(0x1000, 0x2000,
                                 true, false, false, true);
        reads_lhr.bind(query, i + 1);
    }
    expect(!reads_lhr.canReserve(0x1000) &&
               reads_lhr.reserveBlockReason(0x1000) ==
                   HintIndirect::StallReason::Lhr,
           "LHR-reading mask blocks the fifth uncommitted same-PC write");
    reads_lhr.recordStall(HintIndirect::StallReason::Lhr);
    reads_lhr.recordStall(HintIndirect::StallReason::None);
    expect(reads_lhr.queueCounts().lhrStalls == 1 &&
               reads_lhr.queueCounts().metaQueueStalls == 0,
           "LHR-only capacity event increments only the LHR stall counter");
    reads_lhr.commitTarget(1);
    expect(reads_lhr.canReserve(0x1000),
           "LHR-reading mask resumes after the oldest reservation commits");
    reads_lhr.finish();
}

void
testFiniteV3NonHintRedirect()
{
    HintIndirect hardware(finiteV3Config("finite-v3-nonhint-redirect"), nullptr);
    HintFiniteHistory history;
    history.configure(hardware.historyPCs());
    const auto before = history.snapshot();
    hardware.setMeasurementActive(true, 0);
    std::array<std::shared_ptr<HintQuery>, 3> queries;
    for (unsigned i = 0; i < queries.size(); ++i) {
        auto query = hardware.query(std::make_shared<const HintSnapshot>(history),
                                    0x1000, 0x1040, i + 1);
        hardware.complete(query, 0x1000, 0x3000, true, true, false, true);
        hardware.reserve(query, 0x1000, history.localPhase(0));
        history.update(0x1000, 0x2000, true, false, false, true);
        hardware.bind(query, i + 2);
        hardware.resolve(query, 0x1000, 0x3000, i + 2, i + 1);
        queries[i] = query;
    }
    for (uint64_t cycle = 4; cycle <= 7; ++cycle)
        hardware.tick(cycle);
    expect(hardware.confidenceAtEntry(queries[0]->entryId) == 0 &&
           hardware.queueCounts().applied == 3,
           "three younger wrong scores apply before unrelated redirect");
    hardware.redirect(history, 1, 0x800, false, 0, 0, 7);
    history.restore(before);
    expect(hardware.confidenceAtEntry(queries[0]->entryId) == 3 &&
           hardware.queueCounts().reverted == 3 &&
           hardware.canReserve(0x1000) &&
           history.localLow16(0) == 0,
           "non-H2P redirect undoes applied younger scores in reverse order");
    hardware.finish();
}

void
testFiniteV3PhaseAndCapacity()
{
    HintIndirect hardware(finiteV3Config("finite-v3-phase"), nullptr);
    HintFiniteHistory history;
    history.configure(hardware.historyPCs());
    auto make_query = [&](uint64_t cycle, uint64_t ftq_id) {
        auto query = hardware.query(std::make_shared<const HintSnapshot>(history),
                                    0x1000, 0x1040, cycle);
        hardware.complete(query, 0x1000, 0x3000, true, true, false, true);
        hardware.reserve(query, 0x1000, history.localPhase(0));
        history.update(0x1000, 0x2000, true, false, false, true);
        hardware.bind(query, ftq_id);
        return query;
    };
    auto warm = make_query(1, 1);
    hardware.setMeasurementActive(true, 1);
    hardware.resolve(warm, 0x1000, 0x3000, 1, 1, false);
    hardware.commitTarget(1);
    expect(hardware.pendingUpdates() == 0 &&
           hardware.confidenceAtEntry(warm->entryId) == 3,
           "warmup resolve cannot score after later phase transition");
    auto measured = make_query(3, 2);
    hardware.setMeasurementActive(false, 3);
    hardware.resolve(measured, 0x1000, 0x3000, 2, 3, true);
    hardware.commitTarget(2);
    expect(hardware.pendingUpdates() == 1 &&
           hardware.queueCounts().enqueued == 1,
           "measure resolve remains eligible after measure ends");
    hardware.tick(5);
    expect(hardware.confidenceAtEntry(measured->entryId) == 2,
           "measure-eligible score writes while measurement is inactive");
    auto delayed = make_query(6, 3);
    hardware.commitTarget(3);
    expect(hardware.inFlightHints() == 0,
           "FTQ commit releases an unexecuted predicted Hint branch");
    hardware.resolveByFTQId(3, 0x1000, 0x3000, 3, 7, true);
    expect(hardware.inFlightHints() == 0 &&
           hardware.pendingUpdates() == 0 && delayed->resolved,
           "retired unexecuted branch cannot gain a late score");
    hardware.finish();

    HintIndirect not_taken(finiteV3Config("finite-v3-not-taken"), nullptr);
    HintFiniteHistory not_taken_history;
    not_taken_history.configure(not_taken.historyPCs());
    not_taken.setMeasurementActive(true, 0);
    auto not_taken_query = not_taken.query(
        std::make_shared<const HintSnapshot>(not_taken_history),
        0x1000, 0x1040, 1);
    not_taken.complete(not_taken_query, 0x1000, 0x3000,
                       true, true, false, true);
    not_taken.reserve(not_taken_query, 0x1000, 15);
    not_taken.bind(not_taken_query, 1);
    not_taken.resolve(not_taken_query, 0x1000, 0x1004, 1, 1, true, false);
    not_taken.commitTarget(1);
    expect(not_taken.inFlightHints() == 0 &&
           not_taken.pendingUpdates() == 0 &&
           not_taken.confidenceAt(0, 0) == 3,
           "not-taken resolve frees Hint metadata without scoring");
    not_taken.finish();

    HintIndirect full(finiteV3Config("finite-v3-full", 16), nullptr);
    full.recordStall(HintIndirect::StallReason::HistoryWindow);
    expect(full.queueCounts().historyWindowStalls == 0,
           "restore/warmup stalls do not enter measurement counters");
    full.setMeasurementActive(true, 0);
    for (unsigned i = 0; i < 16; ++i) {
        const Addr pc = 0x1000 + i * 0x80;
        expect(full.canReserve(pc), "free Hint metadata slot is available");
        auto query = std::make_shared<HintQuery>();
        full.reserve(query, pc, 15);
        full.bind(query, i + 1);
        full.resolve(query, pc, 0x2000, i + 1, i + 1);
    }
    expect(!full.canReserve(0x1000),
           "17th in-flight H2P prediction stalls at 16-entry metadata limit");
    expect(full.reserveBlockReason(0x1000) ==
               HintIndirect::StallReason::MetaQueue,
           "full metadata queue has priority over per-LHR capacity");
    full.recordStall(HintIndirect::StallReason::MetaQueue);
    full.recordStall(HintIndirect::StallReason::HistoryWindow);
    expect(full.queueCounts().metaQueueStalls == 1 &&
               full.queueCounts().historyWindowStalls == 1 &&
               full.queueCounts().lhrStalls == 0,
           "measured failed enqueue attempts are separated by reason");
    full.commitTarget(1);
    expect(full.canReserve(0x1000),
           "commit releases one Hint metadata queue slot");
    full.finish();
}

void
testFiniteV3ShadowAndOverflow()
{
    HintIndirect hardware(finiteV3Config("finite-v3-shadow"), nullptr);
    HintFiniteHistory history;
    history.configure(hardware.historyPCs());
    hardware.setMeasurementActive(true, 0);
    const auto predict = [&](uint64_t cycle, uint64_t ftq_id,
                             Addr actual) {
        auto query = hardware.query(std::make_shared<const HintSnapshot>(history),
                                    0x1000, 0x1040, cycle);
        const auto override = hardware.complete(query, 0x1000, 0x3000,
                                                true, true, false, true);
        expect(hardware.canReserve(0x1000), "shadow fixture has LHR capacity");
        hardware.reserve(query, 0x1000, history.localPhase(0));
        history.update(0x1000, override.value_or(0x3000),
                       true, false, false, true);
        hardware.bind(query, ftq_id);
        hardware.resolve(query, 0x1000, actual, ftq_id, cycle);
        return query;
    };
    for (unsigned i = 0; i < 3; ++i) {
        auto query = predict(i + 1, i + 1, 0x3000);
        hardware.commitTarget(i + 1);
        expect(query->tagHit, "wrong targets still have valid shadow entries");
    }
    for (uint64_t cycle = 4; cycle <= 7; ++cycle)
        hardware.tick(cycle);
    expect(hardware.confidenceAt(0, 0) == 0,
           "three wrong scores reach confidence zero");
    auto shadow = predict(8, 4, 0x2000);
    expect(shadow->tagHit && shadow->conf == 0 && !shadow->overridden,
           "confidence-zero shadow preserves baseline target");
    auto recovered = hardware.query(
        std::make_shared<const HintSnapshot>(history), 0x1000, 0x1040, 9);
    expect(recovered->conf == 1 &&
           hardware.complete(recovered, 0x1000, 0x3000,
                             true, true, false, true) ==
               std::optional<Addr>(0x2000),
           "correct shadow resolve forwards recovery before writeback");
    hardware.commitTarget(4);
    hardware.tick(10);
    hardware.tick(11);
    expect(hardware.confidenceAt(0, 0) == 1 &&
           hardware.queueCounts().recoveries == 1,
           "shadow recovery reaches the single-port table");
    hardware.finish();

    HintIndirect overflow(finiteV3Config("finite-v3-overflow"), nullptr);
    HintFiniteHistory overflow_history;
    overflow_history.configure(overflow.historyPCs());
    overflow.setMeasurementActive(true, 0);
    for (uint64_t cycle = 1; cycle <= 33; ++cycle) {
        auto query = overflow.query(
            std::make_shared<const HintSnapshot>(overflow_history),
            0x1000, 0x1040, cycle);
        overflow.complete(query, 0x1000, 0x3000,
                          true, true, false, true);
        expect(overflow.canReserve(0x1000),
               "committed metadata releases LHR capacity under read pressure");
        overflow.reserve(query, 0x1000,
                         overflow_history.localPhase(0));
        overflow_history.update(0x1000, 0x2000,
                                true, false, false, true);
        overflow.bind(query, cycle);
        overflow.resolve(query, 0x1000, 0x3000, cycle, cycle);
        overflow.commitTarget(cycle);
    }
    expect(overflow.pendingUpdates() == 32 &&
           overflow.queueCounts().enqueued == 32 &&
           overflow.queueCounts().dropped == 1 &&
           overflow.queueCounts().applied == 0,
           "read-priority starvation fills 32-entry FIFO and drops 33rd score");
    overflow.finish();
}

void
testSpecV7Predictor()
{
    // All helpers below execute the production Hint implementation. Times
    // are CPU cycles; the caller retains the existing BPU future-stage model.
    auto makeSnapshot = [](const HintFiniteHistory &history) {
        return std::make_shared<const HintSnapshot>(history);
    };
    {
        HintIndirect hw(finiteV3Config("spec-v7-timing"), nullptr);
        HintFiniteHistory hist;
        hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        auto q = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 1);
        expect(q->admitted && q->s3Cycle == 3 && q->tagHit && q->conf == 3,
               "exact startPC accepts S1, S3 is N+2");
        expect(hw.complete(q, 0x1010, 0x3000, true, true, false, true) == 0x2000,
               "S3 target covers actual indirect PC inside exact start window");
        hw.reserve(q, 0x1010, hist.localPhase(0));
        hw.bind(q, 1);
        auto q2 = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 2);
        auto q3 = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 3);
        expect(!q2->admitted && !q3->admitted,
               "nonreentrant path falls back during N+1 and N+2");
        expect(!hw.complete(q2, 0x1010, 0x3000, true, true, false, true),
               "busy path cannot override original predictor");
        auto q4 = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 4);
        expect(q4->admitted && q4->conf == 3, "next query admitted at N+3");
        hw.complete(q4, 0x1010, 0x3000, true, true, false, true);
        hw.reserve(q4, 0x1010, hist.localPhase(0)); hw.bind(q4, 2);
        // The second prediction retains conf=3 even though both eventual
        // updates are wrong. Its write must be snapshot 3-1, not current 2-1.
        hw.resolve(q, 0x1010, 0x3000, 1, 4);
        hw.resolve(q4, 0x1010, 0x3000, 2, 4);
        hw.tick(6); // read priority reserved cycle 5
        expect(hw.pendingUpdates() == 2 && hw.confidenceAtEntry(q->entryId) == 3,
               "S2 Context read prevents FIFO write in cycle 5");
        hw.query(makeSnapshot(hist), 0x1002, 0x1040, 6);
        hw.tick(8);
        expect(hw.pendingUpdates() == 0 && hw.confidenceAtEntry(q->entryId) == 2,
               "CAM miss leaves write port free; snapshot updates allow stale overwrite");
        auto in_pipe = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 8);
        expect(in_pipe->admitted, "another S2 read may already be reserved at redirect");
        hw.cancelPending();
        hw.redirect(hist, 1, 0x1000, false, 0, 3, 8);
        expect(hw.confidenceAtEntry(q->entryId) == 2 && hw.pendingRollbacks() == 2 &&
               hw.inFlightHints() == 2,
               "redirect retains applied journal slots without multiwrite SRAM undo");
        auto recovering = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 9);
        expect(!recovering->admitted && hw.queueCounts().rollbackFallbacks == 1,
               "rollback disables only Hint prediction");
        hw.tick(10);
        expect(hw.pendingRollbacks() == 2 && hw.queueCounts().rollbackReadConflicts == 1,
               "already reserved S2 read wins over rollback port");
        hw.tick(11);
        expect(hw.pendingRollbacks() == 1 && hw.confidenceAtEntry(q->entryId) == 2,
               "youngest SRAM update undoes in one distinct port cycle");
        hw.tick(12);
        expect(hw.pendingRollbacks() == 0 && hw.confidenceAtEntry(q->entryId) == 3 &&
               hw.inFlightHints() == 0 && hw.queueCounts().rollbackWrites == 2,
               "older SRAM update undoes next cycle and releases journal");
        auto miss = hw.query(makeSnapshot(hist), 0x0ff0, 0x1040, 12);
        expect(!miss->hasCandidate, "CAM does not range-match a later branch/startPC");
        auto canceled = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 13);
        hw.cancelPending();
        expect(!hw.complete(canceled, 0x1010, 0x3000, true, true, false, true),
               "canceled query cannot publish S3 target");
        hw.finish();
    }
    for (const unsigned mask : {0U, 8U}) {
        HintIndirect hw(finiteV3Config("spec-v7-lhr-full-" +
                            std::to_string(mask), 1, mask), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        // Prime all physical cells to ensure restoration preserves a full
        // nonzero eight-target prediction window, not merely an empty ring.
        for (unsigned i = 0; i < 16; ++i)
            hist.update(0x1010, 0x2000 + 2 * ((i + 1) & 3),
                        true, false, false, true, 0, true);
        const auto saved = hist.snapshot();
        const auto original_local = hist.localLow16(0);
        const auto original_phase = hist.localPhase(0);
        for (unsigned i = 0; i < 8; ++i) {
            auto q = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 1 + i * 3);
            expect(q->admitted, "first eight local writes admitted for every mask");
            hw.complete(q, 0x1010, 0x3000, true, true, false, true);
            hw.reserve(q, 0x1010, hist.localPhase(0)); hw.bind(q, i + 1);
            hist.update(0x1010, 0x2000 + i * 2, true, false, false, true, 0, true);
        }
        expect(hist.localPhase(0) == ((original_phase + 8) & 31),
               "eight local writes advance the five-bit phase exactly eight cells");
        const auto local = hist.localLow16(0);
        auto ninth = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 25);
        expect(!ninth->admitted && hw.inFlightHints() == 8 &&
               hw.queueCounts().lhrStalls == 1 &&
               hw.queueCounts().metaQueueStalls == 0,
               "ninth occurrence falls back for both LHR-reading and non-LHR masks");
        expect(!hw.complete(ninth, 0x1010, 0x3000, true, true, false, true),
               "ninth occurrence retains the original predictor target");
        hist.update(0x1010, 0x3332, true, false, false, true, 0, false);
        expect(hist.localLow16(0) == local,
               "ninth fallback pauses local write while ITH continues");
        // A redirect at a skipped ninth occurrence must preserve all eight
        // older writes; there is no ninth local write to rewind or replay.
        hw.redirect(hist, 9, 0x1010, true, 0x3332, 9, 26);
        expect(hist.localLow16(0) == local && hw.inFlightHints() == 8,
               "redirect on skipped ninth instance preserves older LHR writes");
        hw.redirect(hist, 1, 0x1000, false, 0, 1, 27);
        hist.restoreGlobals(saved);
        expect(hist.localLow16(0) == original_local &&
               hist.localPhase(0) == original_phase && hw.inFlightHints() == 0,
               "ordinary non-H2P redirect exactly restores eight-target history after eight writes");
        hw.finish();
    }
    {
        HintIndirect hw(finiteV3Config("spec-v7-lhr-eighth-redirect"), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        for (unsigned i = 0; i < 8; ++i) {
            auto q = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 1 + i * 3);
            expect(q->admitted, "eighth-redirect fixture admits eight instances");
            hw.complete(q, 0x1010, 0x3000, true, true, false, true);
            hw.reserve(q, 0x1010, hist.localPhase(0)); hw.bind(q, i + 1);
            hist.update(0x1010, 0x2000 + 2 * i, true, false, false, true, 0, true);
        }
        // Rewind the eighth predicted target and replay the resolved target.
        hw.redirect(hist, 8, 0x1010, true, 0x3332, 8, 25);
        hist.update(0x1010, 0x3332, true, false, false, true, 0, true);
        expect((hist.localLow16(0) & 3) == 1 && hw.inFlightHints() == 8,
               "eighth-instance redirect replays the correct target without a ninth reservation");
        hw.commitTarget(1);
        auto resumed = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 28);
        expect(resumed->admitted && hw.inFlightHints() == 7,
               "committing one of eight instances restores Hint admission");
        hw.finish();
    }
    {
        HintIndirect hw(finiteV3Config("spec-v7-meta-full", 16), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        for (unsigned i = 0; i < 16; ++i) {
            const Addr start = 0x1000 + i * 0x80;
            auto q = hw.query(makeSnapshot(hist), start, start + 0x40, 1 + i * 3);
            expect(q->admitted, "16 different selector reservations fit metadata");
            hw.complete(q, start + 0x10, 0x3000, true, true, false, true);
            hw.reserve(q, start + 0x10, hist.localPhase(i)); hw.bind(q, i + 1);
        }
        auto full = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 49);
        expect(!full->admitted && hw.inFlightHints() == 16 &&
               hw.queueCounts().metaQueueStalls == 1,
               "metadata full returns Hint fallback and retains normal prediction opportunity");
        hw.finish();
    }
    {
        HintIndirect hw(finiteV3Config("spec-v7-two-lhr-meta-full", 2), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned sel = i / 8;
            const Addr start = 0x1000 + sel * 0x80;
            auto q = hw.query(makeSnapshot(hist), start, start + 0x40, 1 + i * 3);
            expect(q->admitted, "two selectors may each occupy eight metadata slots");
            hw.complete(q, start + 0x10, 0x3000, true, true, false, true);
            hw.reserve(q, start + 0x10, hist.localPhase(sel)); hw.bind(q, i + 1);
            hist.update(start + 0x10, 0x2000 + 2 * i,
                        true, false, false, true, sel, true);
        }
        auto full = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 49);
        expect(!full->admitted && hw.inFlightHints() == 16 &&
               hw.queueCounts().metaQueueStalls == 1 &&
               hw.queueCounts().lhrStalls == 0,
               "eight-plus-eight fills metadata and reports total capacity first");
        hw.commitTarget(1);
        auto freed = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 52);
        expect(freed->admitted && hw.inFlightHints() == 15,
               "commit frees both one per-PC slot and one global metadata slot");
        hw.finish();
    }
    {
        // Drive a bounded resolve burst directly into the production queue;
        // this is a FIFO capacity test, not a legal per-cycle CPU trace.
        HintIndirect hw(finiteV3Config("spec-v7-conf-full"), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        auto source = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 1);
        hw.complete(source, 0x1010, 0x3000, true, true, false, true);
        for (unsigned i = 1; i <= 33; ++i) {
            auto q = std::make_shared<HintQuery>(*source);
            q->resolved = false; q->ftqId = 0;
            hw.reserve(q, 0x1010, hist.localPhase(0)); hw.bind(q, i);
            hw.resolve(q, 0x1010, 0x3000, i, 1);
            hw.commitTarget(i);
        }
        expect(hw.pendingUpdates() == 32 && hw.queueCounts().dropped == 1,
               "32-entry confidence queue drops the 33rd update without front-end backpressure");
        hw.tick(4);
        expect(hw.pendingUpdates() == 31 && hw.confidenceAtEntry(source->entryId) == 2,
               "single port skips reserved read and writes at most one entry per cycle");
        hw.finish();
    }

    {
        HintIndirect hw(finiteV3Config("spec-v7-training-gates"), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        auto snap = makeSnapshot(hist);
        auto warm = hw.query(snap, 0x1000, 0x1040, 1);
        hw.complete(warm, 0x1010, 0x3000, true, true, false, true);
        hw.reserve(warm, 0x1010, hist.localPhase(0)); hw.bind(warm, 1);
        hw.resolve(warm, 0x1010, 0x3000, 1, 1, false);
        hw.commitTarget(1); hw.tick(4);
        expect(hw.pendingUpdates() == 0 && hw.confidenceAtEntry(warm->entryId) == 3,
               "warmup resolve cannot train confidence");
        hw.setMeasurementActive(true, 4);
        for (unsigned i = 0; i < 3; ++i) {
            const uint64_t cycle = 4 + i * 3;
            auto q = hw.query(snap, 0x1000, 0x1040, cycle);
            expect(q->conf == 3 - i, "SRAM confidence decreases across wrong predictions");
            expect(bool(hw.complete(q, 0x1010, 0x3000, true, true, false, true)),
                   "positive-confidence Hint is actually used");
            hw.reserve(q, 0x1010, hist.localPhase(0)); hw.bind(q, i + 2);
            hw.resolve(q, 0x1010, 0x3000, i + 2, cycle, true);
            hw.commitTarget(i + 2); hw.tick(cycle + 3);
        }
        auto zero = hw.query(snap, 0x1000, 0x1040, 13);
        expect(zero->tagHit && zero->conf == 0 &&
               !hw.complete(zero, 0x1010, 0x3000, true, true, false, true),
               "conf0 falls back to existing predictor");
        hw.reserve(zero, 0x1010, hist.localPhase(0)); hw.bind(zero, 5);
        hw.resolve(zero, 0x1010, 0x2000, 5, 13, true);
        hw.commitTarget(5); hw.tick(16);
        expect(hw.pendingUpdates() == 0 && hw.confidenceAtEntry(zero->entryId) == 0,
               "unused conf0 shadow target does not train or recover confidence");
        hw.finish();
    }

    {
        // Explicit positive-confidence prediction snapshots can overwrite a
        // different current SRAM value. Test both nonadjacent transitions.
        HintIndirect hw(finiteV3Config("spec-v7-stale-transitions"), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        auto source = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 1);
        hw.complete(source, 0x1010, 0x3000, true, true, false, true);
        for (unsigned i = 1; i <= 2; ++i) {
            auto q = std::make_shared<HintQuery>(*source);
            q->conf = i; q->resolved = false; q->ftqId = 0;
            hw.reserve(q, 0x1010, hist.localPhase(0)); hw.bind(q, i);
            hw.resolve(q, 0x1010, i == 1 ? 0x3000 : 0x2000, i, 1, true);
            hw.commitTarget(i);
        }
        hw.tick(4);
        expect(hw.confidenceAtEntry(source->entryId) == 0 &&
               hw.queueCounts().zeroTransitions == 1,
               "stale snapshot 1-1 records actual SRAM 3-to-0 transition");
        hw.tick(5);
        expect(hw.confidenceAtEntry(source->entryId) == 3 &&
               hw.queueCounts().recoveries == 1,
               "correct snapshot 2+1 records actual SRAM 0-to-3 recovery");
        hw.finish();
    }

    {
        HintIndirect hw(finiteV3Config("spec-v7-distinct-set-undo", 2), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        const auto initial = hist.snapshot();
        hw.setMeasurementActive(true, 0);
        std::array<std::shared_ptr<HintQuery>, 2> q;
        for (unsigned i = 0; i < 2; ++i) {
            const Addr start = 0x1000 + i * 0x80;
            const uint64_t cycle = 1 + i * 3;
            q[i] = hw.query(makeSnapshot(hist), start, start + 0x40, cycle);
            expect(q[i]->tagHit && hw.complete(q[i], start + 0x10, 0x3000,
                   true, true, false, true).has_value(), "different set Hint query accepted");
            hw.reserve(q[i], start + 0x10, hist.localPhase(i)); hw.bind(q[i], i + 1);
            hist.update(start + 0x10, 0x2000, true, false, false, true, i, true);
        }
        expect(q[0]->set != q[1]->set, "rollback scenario uses different SRAM sets");
        hw.resolve(q[0], 0x1010, 0x3000, 1, 4, true);
        hw.resolve(q[1], 0x1090, 0x3000, 2, 4, true);
        hw.tick(8);
        expect(hw.confidenceAtEntry(q[0]->entryId) == 2 &&
               hw.confidenceAtEntry(q[1]->entryId) == 2,
               "both different-set updates applied before redirect");
        auto already_in_pipe = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 8);
        expect(already_in_pipe->admitted, "redirect has an outstanding reserved S2 read");
        hw.cancelPending();
        hw.redirect(hist, 1, 0x1000, false, 0, 3, 8);
        hist.restoreGlobals(initial);
        expect(hw.pendingRollbacks() == 2 && hw.inFlightHints() == 2 &&
               hist.localLow16(0) == 0 && hist.localLow16(1) == 0,
               "journal rewinds local selectors immediately but retains two SRAM undo slots");
        hw.commitTarget(100);
        hw.redirect(hist, 1, 0x1000, false, 0, 4, 9);
        hist.restoreGlobals(initial);
        expect(hw.pendingRollbacks() == 2 && hw.inFlightHints() == 2,
               "commit and repeated redirect cannot release or double-rewind pending undo");
        auto fallback = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 9);
        expect(!fallback->admitted, "pending rollback only suppresses Hint");
        hw.tick(10);
        expect(hw.pendingRollbacks() == 2 && hw.confidenceAtEntry(q[1]->entryId) == 2,
               "reserved read delays all rollback writes");
        hw.tick(11);
        expect(hw.pendingRollbacks() == 1 &&
               hw.confidenceAtEntry(q[1]->entryId) == 3 &&
               hw.confidenceAtEntry(q[0]->entryId) == 2,
               "only youngest set restored during first free write cycle");
        hw.tick(12);
        expect(hw.pendingRollbacks() == 0 && hw.inFlightHints() == 0 &&
               hw.confidenceAtEntry(q[0]->entryId) == 3 &&
               hw.queueCounts().rollbackWrites == 2,
               "older set restored during following write cycle");
        hw.finish();
    }

    {
        HintIndirect hw(finiteV3Config("spec-v7-retained-undo-redo"), nullptr);
        HintFiniteHistory hist; hist.configure(hw.historyPCs());
        hw.setMeasurementActive(true, 0);
        auto q = hw.query(makeSnapshot(hist), 0x1000, 0x1040, 1);
        hw.complete(q, 0x1010, 0x3000, true, true, false, true);
        hw.reserve(q, 0x1010, hist.localPhase(0)); hw.bind(q, 1);
        hw.resolve(q, 0x1010, 0x3000, 1, 1, true);
        hw.tick(4);
        expect(hw.confidenceAtEntry(q->entryId) == 2, "initial wrong score was applied");
        hw.redirect(hist, 1, 0x1010, true, 0x2000, 1, 4);
        expect(hw.pendingRollbacks() == 1 && hw.pendingUpdates() == 0,
               "retained redirect branch defers its replacement score until undo");
        hw.commitTarget(1);
        expect(hw.inFlightHints() == 1,
               "committing retained branch cannot prematurely release its undo journal");
        hw.tick(6);
        expect(hw.pendingRollbacks() == 0 && hw.pendingUpdates() == 1 &&
               hw.confidenceAtEntry(q->entryId) == 3 &&
               hw.queueCounts().applied == 1,
               "one cycle undo completes before replacement can consume the shared port");
        hw.tick(7);
        expect(hw.pendingUpdates() == 0 && hw.confidenceAtEntry(q->entryId) == 3 &&
               hw.queueCounts().applied == 2 && hw.queueCounts().rollbackWrites == 1,
               "replacement snapshot score writes in a later cycle and remains separate from undo accounting");
        hw.finish();
    }

}

} // anonymous namespace

void
HintIndirectTest::startup()
{
    SimObject::startup();
    if (!metadata.empty()) {
        HintIndirect::Config config;
        config.mode = "replay";
        config.ideal = ideal;
        config.confQueueDepth = ideal ? 8 : 32;
        config.metadataPath = metadata;
        config.candidatePath = candidates;
        HintIndirect hardware(config, nullptr);
        if (rangePortReuse) {
            const auto snapshot = snapshotFor(hardware);
            hardware.query(snapshot, std::numeric_limits<Addr>::max() - 15,
                           0x20, 1);
            hardware.query(snapshot, 0x1000, 0x1010, 1);
            fatal("Wrapped Hint query incorrectly released its same-cycle port");
        }
        hardware.finish();
    } else if (contextFork) {
        testFiniteV3Vectors(vectors);
        testSpecV7Predictor();
    } else {
        testHistory();
        testIdealHistory();
        testRetiredPrefix();
        testFiniteV3CollectObserverHistory();
        testFiniteV3Vectors(vectors);
        testFiniteV3CollectObserverHistory();
        testFiniteV3Predictor();
        testFiniteV3MaskAwareAdmission();
        testFiniteV3NonHintRedirect();
        testFiniteV3PhaseAndCapacity();
        testFiniteV3ShadowAndOverflow();
        testMaskQueries(true);
        const auto candidate_path = writeInput("hint-test-candidates.json",
            "{\"format\":\"h2p-hint-candidates\",\"version\":2,\"source\":{},"
            "\"pcs\":[\"0x1000\",\"0x1008\",\"0x2000\",\"0x3000\",\"0x20001002\"]}");
        testHardware(writeInput("hint-test-ideal-metadata.json",
                                idealizeMetadata(metadataText(false))),
                     writeInput("hint-test-ideal-candidates.json",
                                idealizeCandidates(readOutput(
                                    "hint-test-candidates.json"))), true);
        testUnlimitedCapacity();
        testIdealOccupancyAndCollect();
        for (const bool collect : {false, true})
            testPredictionBounds(true, collect);
        testIdealInvalidEntry();
        testMixedConfidenceFIFO(true);
        testEarlierTakenCancellation(true);

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
