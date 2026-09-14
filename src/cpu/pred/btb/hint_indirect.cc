#include "cpu/pred/btb/hint_indirect.hh"

#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/json_parser/detail/read.hpp>
#include <boost/property_tree/ptree.hpp>

#include "base/logging.hh"
#include "base/output.hh"
#include "base/statistics.hh"
#include "cpu/pred/btb/hint_history.hh"

namespace gem5::branch_prediction::btb_pred
{
namespace
{

using Tree = boost::property_tree::ptree;

// PropertyTree's default callbacks erase JSON scalar and container types.
// Preserve them while using Boost's parser for syntax, escaping and UTF-8.
// In particular, string "3" must not become integer 3 and {} must not become [].
class TypedJsonCallbacks : public
    boost::property_tree::json_parser::detail::standard_callbacks<Tree>
{
    using Base = boost::property_tree::json_parser::detail::standard_callbacks<Tree>;

  public:
    void on_null()
    {
        Base::on_null();
        current_value() = "Z";
    }
    void on_boolean(bool value)
    {
        Base::on_boolean(value);
        current_value().insert(0, "B");
    }
    template <typename Range>
    void on_number(Range value)
    {
        Base::on_number(value);
        current_value().insert(0, "N");
    }
    void on_begin_number()
    {
        Base::on_begin_number();
        current_value() = "N";
    }
    void on_begin_string()
    {
        Base::on_begin_string();
        if (!is_key()) {
            current_value() = "S";
        }
    }
    void on_begin_array()
    {
        Base::on_begin_array();
        current_value() = "A";
    }
    void on_begin_object()
    {
        Base::on_begin_object();
        current_value() = "O";
    }
};

std::string
hexAddress(Addr value)
{
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

std::string
jsonString(const std::string &value)
{
    std::ostringstream text;
    text << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
          case '"': text << "\\\""; break;
          case '\\': text << "\\\\"; break;
          case '\n': text << "\\n"; break;
          case '\r': text << "\\r"; break;
          case '\t': text << "\\t"; break;
          default:
            if (ch < 0x20) {
                const char *digits = "0123456789abcdef";
                text << "\\u00" << digits[ch >> 4] << digits[ch & 15];
            } else {
                text << ch;
            }
        }
    }
    text << '"';
    return text.str();
}

void
require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void
checkObject(const Tree &tree, const std::string &where)
{
    require(tree.data() == "O", where + " must be an object");
    std::set<std::string> names;
    for (const auto &[name, value] : tree) {
        require(!name.empty(), where + " must be an object");
        require(names.insert(name).second,
                where + " has duplicate field " + name);
    }
}

const Tree &
arrayField(const Tree &tree, const char *name)
{
    const auto &array = tree.get_child(name);
    require(array.data() == "A", std::string(name) + " must be an array");
    for (const auto &item : array) {
        require(item.first.empty(), std::string(name) + " must be an array");
    }
    return array;
}

uint64_t
integer(const Tree &tree, const std::string &name)
{
    const auto &child = tree.get_child(name);
    require(child.empty(), name + " must be an integer");
    require(!child.data().empty() && child.data()[0] == 'N',
            name + " must be a JSON number");
    const auto value = child.data().substr(1);
    require(!value.empty() && std::all_of(value.begin(), value.end(),
        [](char c) { return c >= '0' && c <= '9'; }),
        name + " must be a nonnegative integer");
    size_t end = 0;
    const auto result = std::stoull(value, &end, 10);
    require(end == value.size(), name + " has invalid integer suffix");
    return result;
}

bool
boolean(const Tree &tree, const std::string &name)
{
    const auto &child = tree.get_child(name);
    require(child.empty(), name + " must be a boolean");
    require(!child.data().empty() && child.data()[0] == 'B',
            name + " must be a JSON boolean");
    const auto value = child.data().substr(1);
    require(value == "true" || value == "false",
            name + " must be true or false");
    return value == "true";
}

std::string
stringValue(const Tree &value)
{
    require(value.empty() && !value.data().empty() && value.data()[0] == 'S',
            "expected a JSON string");
    return value.data().substr(1);
}

std::string
stringField(const Tree &tree, const std::string &name)
{
    return stringValue(tree.get_child(name));
}

Addr
address(const std::string &value)
{
    require(value.size() > 2 && value.size() <= 18 &&
            value.compare(0, 2, "0x") == 0,
            "address must be a 64-bit hexadecimal string: " + value);
    require(std::all_of(value.begin() + 2, value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    }), "invalid address: " + value);
    size_t end = 0;
    const Addr result = std::stoull(value, &end, 16);
    require(end == value.size() && !(result & 1),
            "Hint address must be aligned to two bytes: " + value);
    const uint64_t high = result >> 48;
    const uint64_t expected = (result & (uint64_t(1) << 47)) ? 0xffff : 0;
    require(high == expected,
            "Hint address is not canonical for pc/target[47:1]: " + value);
    return result;
}

Tree
readJson(const std::string &path)
{
    std::ifstream input(path);
    require(bool(input), "cannot read Hint JSON file: " + path);
    namespace JsonDetail = boost::property_tree::json_parser::detail;
    using Iterator = std::istreambuf_iterator<char>;
    TypedJsonCallbacks callbacks;
    JsonDetail::encoding<char> encoding;
    JsonDetail::read_json_internal(Iterator(input), Iterator(), encoding,
                                   callbacks, path);
    Tree tree;
    tree.swap(callbacks.output());
    checkObject(tree, path);
    return tree;
}

void
writeContexts(std::ostream &out, const HintContexts &contexts)
{
    out << '[';
    for (size_t i = 0; i < contexts.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << contexts[i];
    }
    out << ']';
}

void
writeCounts(std::ostream &out, const HintIndirect::Counts &counts)
{
    out << "\"executions\":" << counts.executions
        << ",\"baseline_misses\":" << counts.baselineMisses
        << ",\"final_misses\":" << counts.finalMisses
        << ",\"corrected\":" << counts.corrected
        << ",\"harmed\":" << counts.harmed
        << ",\"eligible\":" << counts.eligible
        << ",\"tag_hits\":" << counts.tagHits
        << ",\"conf_enabled\":" << counts.confEnabled
        << ",\"overrides\":" << counts.overrides;
}

const char *HistoryJson =
    "{\"githr_targets\":64,\"lithr_targets\":4,\"call_pcs\":64,"
    "\"return_pcs\":64,\"target_shift\":1}";
const char *MaskOrderJson = "[\"GHR\",\"GITHR\",\"LITHR\",\"Call\",\"Return\"]";

} // anonymous namespace

struct HintIndirect::Statistics : public statistics::Group
{
    statistics::Scalar queries;
    statistics::Scalar overrides;
    statistics::Scalar committed;
    statistics::Scalar corrected;
    statistics::Scalar harmed;
    statistics::Scalar queueEnqueued;
    statistics::Scalar queueApplied;
    statistics::Scalar queueDropped;
    statistics::Scalar zeroTransitions;
    statistics::Scalar recoveries;

    explicit Statistics(statistics::Group *parent)
        : statistics::Group(parent, "h2pHint"),
          ADD_STAT(queries, statistics::units::Count::get(),
                   "Hint block query attempts in measurement"),
          ADD_STAT(overrides, statistics::units::Count::get(),
                   "Hint targets selected at S3 in measurement"),
          ADD_STAT(committed, statistics::units::Count::get(),
                   "Committed non-return indirect branches in measurement"),
          ADD_STAT(corrected, statistics::units::Count::get(),
                   "Hint corrected a baseline target miss at commit"),
          ADD_STAT(harmed, statistics::units::Count::get(),
                   "Hint replaced a correct baseline target at commit"),
          ADD_STAT(queueEnqueued, statistics::units::Count::get(),
                   "Confidence updates accepted into the bounded FIFO"),
          ADD_STAT(queueApplied, statistics::units::Count::get(),
                   "Confidence updates written by the single port"),
          ADD_STAT(queueDropped, statistics::units::Count::get(),
                   "New confidence updates dropped because FIFO was full"),
          ADD_STAT(zeroTransitions, statistics::units::Count::get(),
                   "Confidence writes transitioning from one to zero"),
          ADD_STAT(recoveries, statistics::units::Count::get(),
                   "Confidence writes transitioning from zero to one")
    {}
};

struct HintIndirect::Output
{
    gzFile profile = nullptr;
    std::ofstream confZero;

    ~Output()
    {
        if (profile) {
            gzclose(profile);
        }
    }

    void writeProfile(const std::string &line)
    {
        fatal_if(!profile, "Hint profile stream is not open");
        fatal_if(line.size() > std::numeric_limits<unsigned>::max(),
                 "Hint profile line exceeds zlib write limit");
        const auto written = gzwrite(profile, line.data(), line.size());
        fatal_if(written != static_cast<int>(line.size()),
                 "Failed to write Hint profile");
    }
};

HintIndirect::HintIndirect(const Config &config, statistics::Group *parent)
    : config(config)
{
    fatal_if(config.mode != "off" && config.mode != "collect" &&
             config.mode != "replay", "Invalid Hint mode %s", config.mode);
    if (!enabled()) {
        fatal_if(!config.candidatePath.empty() || !config.metadataPath.empty(),
                 "Hint input files require collect or replay mode");
        return;
    }
    fatal_if(!config.camEntries || !config.contextEntries || !config.ways ||
             !config.slotEntries || !config.confQueueDepth ||
             !config.clockPeriod || config.contextEntries % config.ways,
             "Hint geometry and clock period must be positive and consistent");
    sets = config.contextEntries / config.ways;
    fatal_if((sets & (sets - 1)) || sets > (1U << 27),
             "Hint number of sets must be a power of two below 2^28");
    for (unsigned value = sets; value > 1; value >>= 1) {
        ++indexBits;
    }
    entries.resize(config.contextEntries);
    try {
        if (!config.candidatePath.empty()) {
            loadCandidates();
        }
        if (isCollect()) {
            require(!config.candidatePath.empty(),
                    "Hint collect requires a candidate file");
            require(config.metadataPath.empty(),
                    "Hint collect does not load a replay metadata image");
            historyPcs = candidates;
        } else {
            require(!config.metadataPath.empty(),
                    "Hint replay requires metadata");
            loadMetadata();
            for (const auto &entry : cam) {
                historyPcs.push_back(entry.pc);
            }
        }
    } catch (const std::exception &error) {
        fatal("Invalid Hint input: %s", error.what());
    }
    for (const auto pc : candidates) {
        perPc.emplace(pc, Counts{});
    }
    for (const auto pc : historyPcs) {
        perPc.emplace(pc, Counts{});
    }
    // Tests may leave this null and inspect production counts directly.
    if (parent) {
        stats = std::make_unique<Statistics>(parent);
    }
    openOutputs();
}

HintIndirect::~HintIndirect() = default;

void
HintIndirect::loadCandidates()
{
    const auto tree = readJson(config.candidatePath);
    require(stringField(tree, "format") == "h2p-hint-candidates" &&
            integer(tree, "version") == 2, "unknown candidate format/version");
    checkObject(tree.get_child("source"), "source");
    std::set<Addr> seen;
    for (const auto &node : arrayField(tree, "pcs")) {
        require(node.second.empty(), "candidate PC must be a scalar");
        const Addr pc = address(stringValue(node.second));
        require(seen.insert(pc).second, "duplicate candidate PC");
        candidates.push_back(pc);
    }
    require(candidates.size() <= 20, "Hint collect supports at most 20 PCs");
}

void
HintIndirect::loadMetadata()
{
    const auto tree = readJson(config.metadataPath);
    require(stringField(tree, "format") == "h2p-hint" &&
            integer(tree, "version") == 2 &&
            integer(tree, "hash_version") == 2 &&
            integer(tree, "ghr_bits") == 970,
            "unknown Hint format, hash, or GHR version");
    const std::vector<std::string> expected =
        {"GHR", "GITHR", "LITHR", "Call", "Return"};
    std::vector<std::string> order;
    for (const auto &node : arrayField(tree, "mask_order")) {
        order.push_back(stringValue(node.second));
    }
    require(order == expected, "Hint mask bit order mismatch");
    const auto &history = tree.get_child("history");
    checkObject(history, "history");
    require(integer(history, "githr_targets") == 64 &&
            integer(history, "lithr_targets") == 4 &&
            integer(history, "call_pcs") == 64 &&
            integer(history, "return_pcs") == 64 &&
            integer(history, "target_shift") == 1,
            "Hint history encoding mismatch");
    const auto &layout = tree.get_child("layout");
    checkObject(layout, "layout");
    require(integer(layout, "cam_entries") == config.camEntries &&
            integer(layout, "context_entries") == config.contextEntries &&
            integer(layout, "ways") == config.ways &&
            integer(layout, "slot_entries") == config.slotEntries &&
            integer(layout, "conf_queue_depth") == config.confQueueDepth,
            "Hint metadata layout differs from configured hardware");
    checkObject(tree.get_child("source"), "source");

    std::set<Addr> seenPcs;
    const auto &camArray = arrayField(tree, "cam");
    require(camArray.size() <= config.camEntries, "Hint CAM capacity exceeded");
    for (const auto &node : camArray) {
        checkObject(node.second, "CAM entry");
        const auto pc = address(stringField(node.second, "pc"));
        const auto mask = integer(node.second, "mask");
        require(mask < 32, "Hint mask must be in 0..31");
        require(seenPcs.insert(pc).second, "duplicate Hint CAM PC");
        if (boolean(node.second, "valid")) {
            cam.push_back({pc, static_cast<unsigned>(mask)});
        }
    }
    std::sort(cam.begin(), cam.end(), [](const auto &a, const auto &b) {
        return a.pc < b.pc;
    });
    std::set<Addr> seenTargets;
    for (const auto &node : arrayField(tree, "slots")) {
        require(node.second.empty(), "Hint target must be a scalar");
        const auto target = address(stringValue(node.second));
        require(seenTargets.insert(target).second, "duplicate shared target");
        slots.push_back(target);
    }
    require(slots.size() <= config.slotEntries,
            "Hint shared target capacity exceeded");

    std::set<std::pair<unsigned, unsigned>> positions;
    std::set<std::pair<unsigned, uint32_t>> tags;
    for (const auto &node : arrayField(tree, "entries")) {
        const auto &value = node.second;
        checkObject(value, "Context entry");
        const auto set = integer(value, "set");
        const auto way = integer(value, "way");
        const auto tag = integer(value, "tag");
        const auto slot = integer(value, "slot");
        const auto conf = integer(value, "conf");
        const bool valid = boolean(value, "valid");
        require(set < sets && way < config.ways,
                "Hint SRAM position is out of range");
        require(tag < (uint64_t(1) << (28 - indexBits)),
                "Hint SRAM tag is out of range");
        require(positions.emplace(set, way).second,
                "duplicate Hint SRAM physical position");
        require(slot < config.slotEntries && conf <= 3,
                "Hint SRAM slot/confidence is out of range");
        if (valid) {
            require(slot < slots.size(), "Hint entry references unloaded target");
            require(conf == 3, "valid Hint entries must start at confidence 3");
            require(tags.emplace(set, tag).second,
                    "duplicate valid Hint tag in one set");
        }
        entries[set * config.ways + way] =
            {valid, static_cast<uint32_t>(tag), static_cast<unsigned>(slot),
             static_cast<unsigned>(conf)};
    }
}

std::string
HintIndirect::outputName(const std::string &name) const
{
    return config.outputPrefix.empty() ? name : config.outputPrefix + "/" + name;
}

void
HintIndirect::openOutputs()
{
    if (!config.outputPrefix.empty()) {
        simout.createSubdirectory(config.outputPrefix);
    }
    for (const auto &name : {"hint-profile.jsonl.gz", "hint-stats.json",
                             "conf0.txt"}) {
        fatal_if(std::filesystem::exists(simout.resolve(outputName(name))),
                 "Hint output already exists: %s; use a fresh output directory",
                 outputName(name));
    }
    output = std::make_unique<Output>();
    output->confZero.open(simout.resolve(outputName("conf0.txt")));
    fatal_if(!output->confZero, "Cannot open Hint confidence-zero log");
    if (isCollect()) {
        output->profile = gzopen(
            simout.resolve(outputName("hint-profile.jsonl.gz")).c_str(), "wb1");
        fatal_if(!output->profile, "Cannot open Hint profile output");
        fatal_if(gzbuffer(output->profile, 1U << 20) != 0,
                 "Cannot allocate Hint profile compression buffer");
        writeHeader();
    }
}

void
HintIndirect::writeHeader()
{
    std::ostringstream line;
    line << "{\"kind\":\"header\",\"format\":\"h2p-hint-profile\","
         << "\"version\":2,\"hash_version\":2,\"ghr_bits\":970,"
         << "\"mask_order\":" << MaskOrderJson
         << ",\"history\":" << HistoryJson
         << ",\"layout\":{\"cam_entries\":" << config.camEntries
         << ",\"context_entries\":" << config.contextEntries
         << ",\"ways\":" << config.ways
         << ",\"slot_entries\":" << config.slotEntries
         << ",\"conf_queue_depth\":" << config.confQueueDepth << '}'
         << ",\"clock_period\":" << config.clockPeriod << ",\"pcs\":[";
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i) {
            line << ',';
        }
        line << jsonString(hexAddress(candidates[i]));
    }
    line << "],\"source\":{\"candidates_path\":"
         << jsonString(config.candidatePath) << ",\"metadata_path\":"
         << jsonString(config.metadataPath) << "}}\n";
    output->writeProfile(line.str());
}

std::shared_ptr<HintQuery>
HintIndirect::query(const std::shared_ptr<const HintSnapshot> &snapshot,
                    Addr block_start, Addr block_end, uint64_t cycle)
{
    if (!enabled() || finished) {
        return nullptr;
    }
    fatal_if(block_end < block_start, "Hint prediction block range wraps");
    fatal_if(lastQueryCycle && cycle <= *lastQueryCycle,
             "Hint permits at most one ordered block query per CPU cycle");
    lastQueryCycle = cycle;
    // The CPU normally invokes tick before fetch; this is also safe for the
    // standalone production-helper driver and never drains twice in a cycle.
    tick(cycle);
    auto result = std::make_shared<HintQuery>();
    result->snapshot = snapshot;
    result->blockStart = block_start;
    result->blockEnd = block_end;
    result->queryCycle = cycle;
    if (measurementActive) {
        ++queryStats.lookups;
        if (stats) {
            ++stats->queries;
        }
    }
    if (!snapshot) {
        return result;
    }
    unsigned matches = 0;
    auto match = [&](Addr pc, unsigned mask) {
        if (pc >= block_start && pc < block_end) {
            ++matches;
            if (!result->hasCandidate || pc < result->pc) {
                result->hasCandidate = true;
                result->pc = pc;
                result->mask = mask;
            }
        }
    };
    if (isReplay()) {
        for (const auto &entry : cam) {
            match(entry.pc, entry.mask);
        }
    } else {
        for (const auto pc : candidates) {
            match(pc, 0);
        }
    }
    if (measurementActive && matches) {
        ++queryStats.camHits;
        if (matches > 1) {
            ++queryStats.camContentions;
            queryStats.camExtraCandidates += matches - 1;
        }
    }
    if (!isReplay() || !result->hasCandidate) {
        return result;
    }
    result->key = hintHash(result->pc, snapshot->contextsFor(result->pc),
                           result->mask);
    result->set = result->key & (sets - 1);
    const uint32_t tag = result->key >> indexBits;
    for (unsigned way = 0; way < config.ways; ++way) {
        const auto &entry = entries[result->set * config.ways + way];
        if (!entry.valid || entry.tag != tag) {
            continue;
        }
        result->tagHit = true;
        result->way = way;
        result->slot = entry.slot;
        result->conf = entry.conf;
        result->hintTarget = slots[entry.slot];
        if (measurementActive) {
            ++queryStats.tagHits;
            queryStats.confEnabled += entry.conf > 0;
        }
        break;
    }
    return result;
}

std::optional<Addr>
HintIndirect::complete(const std::shared_ptr<HintQuery> &query,
    Addr selected_pc, Addr selected_target, bool taken, bool indirect,
    bool is_return, bool btb_identified)
{
    if (!query || !enabled() || finished) {
        return std::nullopt;
    }
    fatal_if(query->completed, "Hint query may be completed only once");
    query->completed = true;
    query->baselineValid = taken;
    query->baselinePC = selected_pc;
    query->baselineTarget = selected_target;
    query->baselineIndirect = indirect;
    query->baselineReturn = is_return;
    query->btbIdentified = btb_identified;
    if (isCollect()) {
        // Group-dependent CAM arbitration is replayed offline. Do not drop
        // the second candidate merely because collect observes up to 20 PCs.
        query->eligible = query->snapshot && taken && indirect &&
                          !is_return && btb_identified;
        return std::nullopt;
    }
    if (!query->hasCandidate || !query->snapshot) {
        return std::nullopt;
    }
    auto cancel = [&](uint64_t QueryCounts::*counter) {
        if (measurementActive) {
            ++(queryStats.*counter);
        }
    };
    if (!taken) {
        cancel(&QueryCounts::cancelNotTaken);
    } else if (is_return) {
        cancel(&QueryCounts::cancelReturn);
    } else if (!indirect) {
        cancel(&QueryCounts::cancelNotIndirect);
    } else if (selected_pc != query->pc) {
        cancel(&QueryCounts::cancelPcMismatch);
    } else if (!btb_identified) {
        cancel(&QueryCounts::cancelNoBtb);
    } else {
        query->eligible = true;
    }
    if (!query->eligible || !query->tagHit || !query->conf) {
        return std::nullopt;
    }
    query->overridden = true;
    if (measurementActive) {
        ++queryStats.overrides;
        if (stats) {
            ++stats->overrides;
        }
    }
    return query->hintTarget;
}

void
HintIndirect::commit(const std::shared_ptr<HintQuery> &query, uint64_t sequence,
    Addr pc, Addr actual, bool final_valid, Addr final_target, uint64_t cycle)
{
    if (!enabled() || !measurementActive || finished) {
        return;
    }
    // A missing Hint snapshot cannot have changed the selected target.
    const bool have_baseline = query && query->completed;
    const bool baseline_valid = have_baseline ?
        query->baselineValid && query->baselinePC == pc : final_valid;
    const Addr baseline_pc = have_baseline ? query->baselinePC : pc;
    const Addr baseline_target = have_baseline ?
        query->baselineTarget : final_target;
    const bool baseline_miss = !baseline_valid || baseline_target != actual;
    const bool final_miss = !final_valid || final_target != actual;
    const bool eligible = have_baseline && query->snapshot && query->eligible &&
                          query->baselinePC == pc;
    const bool tag_hit = isReplay() && eligible && query->tagHit &&
                         query->pc == pc;
    const bool conf_enabled = tag_hit && query->conf > 0;
    const bool overridden = tag_hit && query->overridden;
    auto record = [&](Counts &counts) {
        ++counts.executions;
        counts.baselineMisses += baseline_miss;
        counts.finalMisses += final_miss;
        counts.corrected += baseline_miss && !final_miss;
        counts.harmed += !baseline_miss && final_miss;
        counts.eligible += eligible;
        counts.tagHits += tag_hit;
        counts.confEnabled += conf_enabled;
        counts.overrides += overridden;
    };
    record(global);
    auto entry = perPc.find(pc);
    if (entry != perPc.end()) {
        record(entry->second);
    }
    if (stats) {
        ++stats->committed;
        stats->corrected += baseline_miss && !final_miss;
        stats->harmed += !baseline_miss && final_miss;
    }
    if (isCollect() && entry != perPc.end()) {
        writeProfile(query, sequence, pc, actual, baseline_valid,
                     baseline_pc, baseline_target, eligible, cycle);
    }
    // Include conf=0 shadow predictions. Only a real tag hit can recover;
    // an invalid SRAM entry never reaches this point.
    if (tag_hit) {
        enqueue(query, sequence, actual, cycle);
    }
}

void
HintIndirect::writeProfile(const std::shared_ptr<HintQuery> &query,
    uint64_t sequence, Addr pc, Addr actual, bool baseline_valid,
    Addr baseline_pc, Addr baseline_target, bool eligible, uint64_t cycle)
{
    std::ostringstream line;
    line << "{\"kind\":\"branch\",\"seq\":" << sequence
         << ",\"pc\":" << jsonString(hexAddress(pc))
         << ",\"actual\":" << jsonString(hexAddress(actual))
         << ",\"baseline_valid\":" << (baseline_valid ? "true" : "false")
         << ",\"baseline_pc\":" << jsonString(hexAddress(baseline_pc))
         << ",\"baseline_target\":" << jsonString(hexAddress(baseline_target))
         << ",\"query_cycle\":";
    if (query) {
        line << query->queryCycle;
    } else {
        line << "null";
    }
    line << ",\"commit_cycle\":" << cycle << ",\"block_start\":"
         << jsonString(hexAddress(query ? query->blockStart : 0))
         << ",\"block_end\":"
         << jsonString(hexAddress(query ? query->blockEnd : 0))
         << ",\"eligible\":" << (eligible ? "true" : "false")
         << ",\"contexts\":";
    if (query && query->snapshot) {
        writeContexts(line, query->snapshot->contextsFor(pc));
    } else {
        line << "null";
    }
    line << "}\n";
    output->writeProfile(line.str());
}

void
HintIndirect::enqueue(const std::shared_ptr<HintQuery> &query,
    uint64_t sequence, Addr actual, uint64_t cycle)
{
    if (updates.size() == config.confQueueDepth) {
        ++queueStats.dropped;
        if (stats) {
            ++stats->queueDropped;
        }
        return;
    }
    fatal_if(cycle == std::numeric_limits<uint64_t>::max(),
             "Hint confidence update cycle overflow");
    updates.push_back({query, actual, sequence, cycle, cycle + 1});
    ++queueStats.enqueued;
    queueStats.maxOccupancy = std::max<uint64_t>(queueStats.maxOccupancy,
                                               updates.size());
    if (stats) {
        ++stats->queueEnqueued;
    }
}

void
HintIndirect::tick(uint64_t cycle)
{
    if (!enabled() || finished) {
        return;
    }
    fatal_if(lastTick && cycle < *lastTick, "Hint CPU cycle moved backwards");
    if (lastTick && cycle == *lastTick) {
        return;
    }
    uint64_t next_cycle = lastTick ? *lastTick + 1 : cycle;
    lastTick = cycle;
    if (!measurementActive) {
        return;
    }
    queueStats.occupancySamples += cycle - next_cycle + 1;
    // Sleeping CPUs need no extra clock event. Catch up their finite FIFO at
    // the next observation, preserving each write's actual cycle. The loop is
    // bounded by queue depth, never by the number of elapsed idle cycles.
    while (!updates.empty()) {
        const uint64_t write_cycle =
            std::max(next_cycle, updates.front().readyCycle);
        if (write_cycle > cycle) {
            break;
        }
        queueStats.occupancySum +=
            (write_cycle - next_cycle + 1) * updates.size();
        const auto update = updates.front();
        updates.pop_front();
        const auto &query = *update.query;
        auto &entry = entries[query.set * config.ways + query.way];
        fatal_if(!entry.valid || entry.slot != query.slot ||
                 entry.tag != query.key >> indexBits,
                 "Frozen Hint entry identity changed before confidence writeback");
        const unsigned old_conf = entry.conf;
        if (query.hintTarget == update.actual) {
            entry.conf = std::min(3U, entry.conf + 1);
        } else if (entry.conf) {
            --entry.conf;
        }
        ++queueStats.applied;
        if (stats) {
            ++stats->queueApplied;
        }
        if (old_conf == 1 && entry.conf == 0) {
            ++queueStats.zeroTransitions;
            if (stats) {
                ++stats->zeroTransitions;
            }
            writeConfZero(update, write_cycle);
        } else if (old_conf == 0 && entry.conf == 1) {
            ++queueStats.recoveries;
            if (stats) {
                ++stats->recoveries;
            }
        }
        next_cycle = write_cycle + 1;
        if (write_cycle == cycle) {
            break;
        }
    }
    if (next_cycle <= cycle) {
        queueStats.occupancySum += (cycle - next_cycle + 1) * updates.size();
    }
}

void
HintIndirect::writeConfZero(const Update &update, uint64_t cycle)
{
    const auto &query = *update.query;
    auto &line = output->confZero;
    line << "{\"pc\":" << jsonString(hexAddress(query.pc))
         << ",\"seq\":" << update.sequence << ",\"mask\":" << query.mask
         << ",\"contexts\":";
    writeContexts(line, query.snapshot->contextsFor(query.pc));
    line << ",\"history\":" << query.snapshot->historyJson(query.pc)
         << ",\"hash\":" << query.key << ",\"set\":" << query.set
         << ",\"way\":" << query.way << ",\"slot\":" << query.slot
         << ",\"predicted_target\":" << jsonString(hexAddress(query.hintTarget))
         << ",\"actual_target\":" << jsonString(hexAddress(update.actual))
         << ",\"query_cycle\":" << query.queryCycle
         << ",\"commit_cycle\":" << update.commitCycle
         << ",\"writeback_cycle\":" << cycle
         << ",\"query_tick\":" << query.queryCycle * config.clockPeriod
         << ",\"commit_tick\":" << update.commitCycle * config.clockPeriod
         << ",\"writeback_tick\":" << cycle * config.clockPeriod << "}\n";
    fatal_if(!line, "Failed to write Hint confidence-zero log");
}

void
HintIndirect::setMeasurementActive(bool active)
{
    measurementActive = active;
}

void
HintIndirect::resetStats()
{
    if (!enabled() || finished) {
        return;
    }
    global = {};
    for (auto &[pc, counts] : perPc) {
        counts = {};
    }
    queryStats = {};
    queueStats = {};
    queueStats.carriedIn = updates.size();
    queueStats.maxOccupancy = updates.size();
    // Reset is a statistics operation. Keep all trained confidence and FIFO
    // contents, and explicitly account for updates accepted before this reset.
    if (output->profile) {
        fatal_if(gzclose(output->profile) != Z_OK,
                 "Failed to close Hint profile before reset");
        output->profile = nullptr;
        output->profile = gzopen(
            simout.resolve(outputName("hint-profile.jsonl.gz")).c_str(), "wb1");
        fatal_if(!output->profile || gzbuffer(output->profile, 1U << 20) != 0,
                 "Cannot reopen Hint profile after reset");
        writeHeader();
    }
    if (isReplay()) {
        output->confZero.close();
        output->confZero.open(simout.resolve(outputName("conf0.txt")),
                              std::ios::trunc);
        fatal_if(!output->confZero, "Cannot reset Hint confidence-zero log");
    }
}

unsigned
HintIndirect::confidenceAt(unsigned set, unsigned way) const
{
    fatal_if(set >= sets || way >= config.ways,
             "Hint confidence read position is out of range");
    return entries[set * config.ways + way].conf;
}

void
HintIndirect::writeStats()
{
    auto out = simout.create(outputName("hint-stats.json"), false, true);
    auto &line = *out->stream();
    line << "{\"format\":\"h2p-hint-stats\",\"version\":2,\"mode\":"
         << jsonString(config.mode) << ",\"global\":{";
    writeCounts(line, global);
    line << "},\"pcs\":[";
    bool first = true;
    for (const auto &[pc, counts] : perPc) {
        if (!first) {
            line << ',';
        }
        first = false;
        line << "{\"pc\":" << jsonString(hexAddress(pc)) << ',';
        writeCounts(line, counts);
        line << '}';
    }
    line << "],\"queue\":{\"enqueued\":" << queueStats.enqueued
         << ",\"applied\":" << queueStats.applied
         << ",\"dropped\":" << queueStats.dropped
         << ",\"carried_in\":" << queueStats.carriedIn
         << ",\"pending\":" << updates.size()
         << ",\"zero_transitions\":" << queueStats.zeroTransitions
         << ",\"recoveries\":" << queueStats.recoveries
         << ",\"max_occupancy\":" << queueStats.maxOccupancy
         << ",\"occupancy_samples\":" << queueStats.occupancySamples
         << ",\"occupancy_sum\":" << queueStats.occupancySum
         << "},\"query\":{\"lookups\":" << queryStats.lookups
         << ",\"cam_hits\":" << queryStats.camHits
         << ",\"cam_contentions\":" << queryStats.camContentions
         << ",\"cam_extra_candidates\":" << queryStats.camExtraCandidates
         << ",\"tag_hits\":" << queryStats.tagHits
         << ",\"conf_enabled\":" << queryStats.confEnabled
         << ",\"overrides\":" << queryStats.overrides
         << ",\"cancel_no_btb\":" << queryStats.cancelNoBtb
         << ",\"cancel_not_taken\":" << queryStats.cancelNotTaken
         << ",\"cancel_not_indirect\":" << queryStats.cancelNotIndirect
         << ",\"cancel_return\":" << queryStats.cancelReturn
         << ",\"cancel_pc_mismatch\":" << queryStats.cancelPcMismatch
         << "}}\n";
    fatal_if(!line, "Failed to write Hint statistics");
    simout.close(out);
}

void
HintIndirect::finish()
{
    if (!enabled() || finished) {
        return;
    }
    finished = true;
    measurementActive = false;
    fatal_if(queueStats.carriedIn + queueStats.enqueued !=
             queueStats.applied + updates.size(),
             "Hint confidence update accounting did not conserve entries");
    fatal_if(global.baselineMisses + global.harmed !=
             global.finalMisses + global.corrected,
             "Hint final-target accounting did not conserve misses");
    writeStats();
    // Do not advance the CPU or drain pending updates at measurement exit.
    if (output->profile) {
        fatal_if(gzclose(output->profile) != Z_OK,
                 "Failed to finish Hint profile output");
        output->profile = nullptr;
    }
    if (output->confZero.is_open()) {
        output->confZero.flush();
        fatal_if(!output->confZero, "Failed to finish Hint confidence log");
        output->confZero.close();
    }
}

} // namespace gem5::branch_prediction::btb_pred
