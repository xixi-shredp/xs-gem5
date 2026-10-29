#include "cpu/pred/btb/hint_indirect.hh"

#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
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

std::vector<uint8_t>
rawImage(const std::string &path, const std::string &magic, size_t size)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot open Hint image: " + path);
    std::array<char, 8> header{};
    input.read(header.data(), header.size());
    if (std::string(header.data(), header.size()) != magic)
        return {};  // Legacy typed JSON remains a separate explicit format.
    input.clear();
    input.seekg(0, std::ios::end);
    require(input.tellg() == std::streamoff(size),
            "incorrect raw Hint image length");
    input.seekg(0);
    std::vector<uint8_t> bytes(size);
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    require(bool(input), "truncated raw Hint image");
    require(std::all_of(bytes.begin() + 8, bytes.begin() + 16,
                        [](uint8_t value) { return value == 0; }),
            "raw Hint header reserved bytes must be zero");
    return bytes;
}

uint64_t
littleWord(const std::vector<uint8_t> &bytes, size_t offset, unsigned size)
{
    uint64_t word = 0;
    for (unsigned i = 0; i < size; ++i)
        word |= uint64_t(bytes.at(offset + i)) << (8 * i);
    return word;
}

Addr
expandAddress(uint64_t word)
{
    Addr value = word << 1;
    if (value & (Addr(1) << 47))
        value |= 0xffff000000000000ULL;
    return value;
}

bool
traceAddress(Addr pc)
{
    return !(pc & 1) &&
        expandAddress((pc >> 1) & ((uint64_t(1) << 47) - 1)) == pc;
}

unsigned
trainedConfidence(const HintQuery &query, Addr pc, Addr actual)
{
    if (!query.conf)
        return !query.baselineValid || query.baselinePC != pc ||
               query.baselineTarget != actual ? 1 : 0;
    return query.hintTarget == actual ? std::min(3U, query.conf + 1) :
                                       query.conf - 1;
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
writeContexts(std::ostream &out, const HintV3Contexts &contexts)
{
    out << '[';
    for (size_t i = 0; i < contexts.size(); ++i) {
        if (i)
            out << ',';
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
        << ",\"overrides\":" << counts.overrides
        << ",\"override_misses\":" << counts.overridesMisses
        << ",\"tag_hit_misses\":" << counts.tagHitMisses;
}

const char *HistoryJson =
    "{\"githr_targets\":64,\"lithr_targets\":4,\"call_pcs\":64,"
    "\"return_pcs\":64,\"target_shift\":1}";
const char *MaskOrderJson = "[\"GHR\",\"GITHR\",\"LITHR\",\"Call\",\"Return\"]";
const char *FiniteReferenceJson =
    "{\"cam_entries\":16,\"context_entries\":4096,\"ways\":2,"
    "\"slot_entries\":32}";
const char *ForkContextEncoding = "xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1";
const char *V3HistoryJson =
    "{\"ith_entries\":128,\"ith_use_bits\":128,\"lhr_entries\":16,"
    "\"lhr_bits\":32,\"lhr_use_bits\":16,\"lhr_head_bits\":5,\"call_entries\":128,"
    "\"ret_entries\":128,\"path_use_bits\":128,\"target_shift\":1}";
const char *V3MaskOrderJson = "[\"ITH\",\"Call\",\"Ret\",\"LHR\"]";

void
writeOccupancy(std::ostream &out, const HintIndirect::Occupancy &value)
{
    const double capacity = value.finiteCapacity;
    out << "{\"resident_entries\":" << value.residentEntries
        << ",\"avg_entries\":" << value.avgEntries
        << ",\"peak_entries\":" << value.peakEntries
        << ",\"sample_cycles\":" << value.sampleCycles
        << ",\"finite_capacity\":" << value.finiteCapacity
        << ",\"resident_multiple\":" << value.residentEntries / capacity
        << ",\"avg_multiple\":" << value.avgEntries / capacity
        << ",\"peak_multiple\":" << value.peakEntries / capacity << '}';
}

} // anonymous namespace

struct HintIndirect::Statistics : public statistics::Group
{
    // These values are calculated only at dump, avoiding per-cycle scalar
    // writes for immutable resident tables. Elapsed cycles are maintained in
    // O(1) by the production helper, including cycles without any query.
    struct TableStats
    {
        statistics::Scalar residentEntries;
        statistics::Scalar avgEntries;
        statistics::Scalar peakEntries;
        statistics::Scalar sampleCycles;
        statistics::Scalar finiteCapacity;
        statistics::Scalar residentMultiple;
        statistics::Scalar avgMultiple;
        statistics::Scalar peakMultiple;

        TableStats(statistics::Group *parent, const std::string &table)
            : residentEntries(parent, (table + "_resident_entries").c_str(),
                  statistics::units::Count::get(), "Loaded valid Hint entries"),
              avgEntries(parent, (table + "_avg_entries").c_str(),
                  statistics::units::Count::get(),
                  "Measure-cycle-weighted mean resident Hint entries"),
              peakEntries(parent, (table + "_peak_entries").c_str(),
                  statistics::units::Count::get(),
                  "Peak resident Hint entries during measurement"),
              sampleCycles(parent, (table + "_sample_cycles").c_str(),
                  statistics::units::Cycle::get(),
                  "Elapsed measurement cycles for resident occupancy"),
              finiteCapacity(parent, (table + "_finite_capacity").c_str(),
                  statistics::units::Count::get(),
                  "Default finite Hint capacity used as comparison"),
              residentMultiple(parent, (table + "_resident_multiple").c_str(),
                  statistics::units::Ratio::get(),
                  "Resident entries divided by default finite capacity"),
              avgMultiple(parent, (table + "_avg_multiple").c_str(),
                  statistics::units::Ratio::get(),
                  "Average entries divided by default finite capacity"),
              peakMultiple(parent, (table + "_peak_multiple").c_str(),
                  statistics::units::Ratio::get(),
                  "Peak entries divided by default finite capacity")
        {}

        void update(const Occupancy &value)
        {
            residentEntries = value.residentEntries;
            avgEntries = value.avgEntries;
            peakEntries = value.peakEntries;
            sampleCycles = value.sampleCycles;
            finiteCapacity = value.finiteCapacity;
            const double capacity = value.finiteCapacity;
            residentMultiple = value.residentEntries / capacity;
            avgMultiple = value.avgEntries / capacity;
            peakMultiple = value.peakEntries / capacity;
        }
    };

    const HintIndirect &owner;
    statistics::Scalar queries;
    statistics::Scalar overrides;
    statistics::Scalar committed;
    statistics::Scalar corrected;
    statistics::Scalar harmed;
    statistics::Scalar queueEnqueued;
    statistics::Scalar queueApplied;
    statistics::Scalar queueDropped;
    statistics::Scalar historyWindowStalls;
    statistics::Scalar metaQueueStalls;
    statistics::Scalar lhrStalls;
    statistics::Scalar zeroTransitions;
    statistics::Scalar recoveries;
    TableStats cam;
    TableStats context;
    TableStats slots;

    Statistics(statistics::Group *parent, const HintIndirect &owner)
        : statistics::Group(parent, "h2pHint"), owner(owner),
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
          ADD_STAT(historyWindowStalls, statistics::units::Count::get(),
                   "Measured prediction-enqueue attempts blocked by the finite global history recovery window"),
          ADD_STAT(metaQueueStalls, statistics::units::Count::get(),
                   "Measured prediction-enqueue attempts blocked by the configured Hint metadata queue"),
          ADD_STAT(lhrStalls, statistics::units::Count::get(),
                   "Measured Hint queries falling back after eight uncommitted updates to the same H2P PC"),
          ADD_STAT(zeroTransitions, statistics::units::Count::get(),
                   "Confidence writes transitioning from one to zero"),
          ADD_STAT(recoveries, statistics::units::Count::get(),
                   "Confidence writes transitioning from zero to one"),
          cam(this, "cam"), context(this, "context"), slots(this, "slots")
    {}

    void preDumpStats() override
    {
        statistics::Group::preDumpStats();
        cam.update(owner.occupancy(Table::Cam));
        context.update(owner.occupancy(Table::Context));
        slots.update(owner.occupancy(Table::Slots));
    }
};

struct HintIndirect::Output
{
    gzFile profile = nullptr;
    gzFile commitHistory = nullptr;
    std::ofstream confZero;
    std::ofstream detail;
    std::ofstream detailAudit;

    ~Output()
    {
        if (profile) {
            gzclose(profile);
        }
        if (commitHistory)
            gzclose(commitHistory);
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
    fatal_if(config.profileContext != "prediction" &&
             config.profileContext != "commit-pre",
             "Invalid Hint profiling context source %s", config.profileContext);
    fatal_if(config.ideal && config.profileContext != "prediction",
             "Commit-pre Hint profiling requires finite mode");
    fatal_if(config.ideal && !enabled(),
             "Ideal Hint requires collect or replay mode");
    fatal_if(config.metaEntries != 16 && config.metaEntries != 32,
             "Hint metadata queue supports 16 or 32 entries");
    if (!enabled()) {
        fatal_if(!config.candidatePath.empty() || !config.metadataPath.empty(),
                 "Hint input files require collect or replay mode");
        return;
    }
    fatal_if(!config.camEntries || !config.contextEntries || !config.ways ||
             !config.slotEntries || !config.confQueueDepth ||
             !config.clockPeriod || config.contextEntries % config.ways,
             "Hint geometry and clock period must be positive and consistent");
    fatal_if(!config.ideal && (config.camEntries != 16 ||
             config.contextEntries != 4096 || config.ways != 2 ||
             config.slotEntries != 32 || config.confQueueDepth != 32),
             "Finite spec v7 Hint requires 16 CAM, 4096 Context, two ways, "
             "32 Slots and 32 confidence FIFO entries");
    fatal_if(config.ideal && (config.camEntries != 16 ||
             config.contextEntries != 4096 || config.ways != 2 ||
             config.slotEntries != 32),
             "Ideal Hint cannot override finite table geometry parameters");
    sets = config.contextEntries / config.ways;
    fatal_if((sets & (sets - 1)) || sets > (1U << 27),
             "Hint number of sets must be a power of two below 2^28");
    for (unsigned value = sets; value > 1; value >>= 1) {
        ++indexBits;
    }
    if (!config.ideal) {
        entries.resize(config.contextEntries);
    }
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
                historyPcs.push_back(entry.valid ? entry.pc : MaxAddr);
            }
        }
    } catch (const std::exception &error) {
        fatal("Invalid Hint input: %s", error.what());
    }
    for (const auto pc : candidates) {
        if (pc != MaxAddr)
            perPc.emplace(pc, Counts{});
    }
    for (const auto pc : historyPcs) {
        if (pc != MaxAddr)
            perPc.emplace(pc, Counts{});
    }
    // Tests may leave this null and inspect production counts directly.
    if (parent) {
        stats = std::make_unique<Statistics>(parent, *this);
    }
    if (isReplay()) {
        residentEntries = {static_cast<uint64_t>(std::count_if(
            cam.begin(), cam.end(), [](const auto &entry) { return entry.valid; })),
            static_cast<uint64_t>(std::count_if(
            entries.begin(), entries.end(),
            [](const auto &entry) { return entry.valid; })), slots.size()};
    }
    openOutputs();
}

HintIndirect::~HintIndirect() = default;

void
HintIndirect::loadCandidates()
{
    if (!config.ideal) {
        const auto image = rawImage(config.candidatePath, "H2PCAM58", 144);
        if (!image.empty()) {
            loadRawCam(image);
            for (const auto &entry : cam)
                candidates.push_back(entry.valid ? entry.pc : MaxAddr);
            return;
        }
        require(false, "finite Hint requires a CAM58 H2PCAM58 image");
    }
    const auto tree = readJson(config.candidatePath);
    require(stringField(tree, "format") == "h2p-hint-candidates" &&
            integer(tree, "version") == (config.ideal ? 3 : 7),
            "candidate format/version does not match Hint storage mode");
    require(stringField(tree, "storage") ==
            (config.ideal ? "unlimited" : "finite"),
            "candidate storage does not match Hint mode");
    if (!config.ideal)
        require(stringField(tree, "context_encoding") == ForkContextEncoding,
                "candidate fork Context encoding mismatch");
    checkObject(tree.get_child("source"), "source");
    std::set<Addr> seen;
    for (const auto &node : arrayField(tree, "pcs")) {
        require(node.second.empty(), "candidate PC must be a scalar");
        const Addr pc = address(stringValue(node.second));
        require(seen.insert(pc).second, "duplicate candidate PC");
        candidates.push_back(pc);
    }
    require(config.ideal || candidates.size() <= 16,
            "finite spec v7 Hint collect supports at most 16 CAM PCs");
    if (config.ideal) {
        candidateIndex.assign(seen.begin(), seen.end());
    }
}

void
HintIndirect::loadRawCam(const std::vector<uint8_t> &image)
{
    cam.assign(16, CamEntry{0, 0, false});
    std::set<Addr> seen;
    for (unsigned index = 0; index < 16; ++index) {
        const auto word = littleWord(image, 16 + index * 8, 8);
        require(!(word >> 58), "CAM58 reserved bits must be zero");
        const bool valid = word & (uint64_t(1) << 51);
        require(valid || word == 0, "invalid raw CAM row must be zero");
        if (!valid)
            continue;
        const Addr pc = expandAddress(word & ((uint64_t(1) << 47) - 1));
        require(seen.insert(pc).second, "duplicate raw CAM startPC");
        const unsigned position = (word >> 52) & 31;
        const __int128 cfi = __int128(pc & ~Addr(31)) + 2 * position;
        const __int128 end = std::min(((__int128(pc) + 64) >> 5) << 5,
                                      ((__int128(pc) >> 12) + 1) << 12);
        require(pc <= MaxAddr - 64 && cfi >= pc && cfi < end && cfi <= MaxAddr,
                "CAM58 CFI position is outside its window");
        cam[index] = {pc, unsigned((word >> 47) & 15), true, position,
                      bool((word >> 57) & 1)};
    }
}

void
HintIndirect::loadMetadata()
{
    if (!config.ideal) {
        loadFiniteV3Metadata();
        return;
    }
    const auto tree = readJson(config.metadataPath);
    require(stringField(tree, "format") == "h2p-hint" &&
            integer(tree, "version") == (config.ideal ? 3 : 2) &&
            integer(tree, "hash_version") == 2 &&
            integer(tree, "ghr_bits") == 970,
            "Hint format/storage, hash, or GHR version mismatch");
    if (config.ideal || tree.get_child_optional("storage")) {
        require(stringField(tree, "storage") ==
                (config.ideal ? "unlimited" : "finite"),
                "metadata storage does not match Hint mode");
    }
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
    require(integer(layout, "conf_queue_depth") == config.confQueueDepth,
            "Hint metadata layout differs from configured hardware");
    if (config.ideal) {
        require(layout.size() == 1,
                "unlimited Hint layout must contain only conf_queue_depth");
        const auto &reference = tree.get_child("finite_reference");
        checkObject(reference, "finite_reference");
        require(reference.size() == 4 &&
                integer(reference, "cam_entries") == 16 &&
                integer(reference, "context_entries") == 4096 &&
                integer(reference, "ways") == 2 &&
                integer(reference, "slot_entries") == 32,
                "unlimited Hint finite reference mismatch");
    } else {
        require(integer(layout, "cam_entries") == config.camEntries &&
                integer(layout, "context_entries") == config.contextEntries &&
                integer(layout, "ways") == config.ways &&
                integer(layout, "slot_entries") == config.slotEntries,
                "Hint metadata layout differs from configured hardware");
    }
    checkObject(tree.get_child("source"), "source");

    std::set<Addr> seenPcs;
    const auto &camArray = arrayField(tree, "cam");
    require(config.ideal || camArray.size() <= config.camEntries,
            "Hint CAM capacity exceeded");
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
    require(config.ideal || slots.size() <= config.slotEntries,
            "Hint shared target capacity exceeded");

    if (config.ideal) {
        const auto &array = arrayField(tree, "entries");
        entries.reserve(array.size());
        idealIndex.reserve(array.size());
        std::set<uint32_t> keys;
        for (const auto &node : array) {
            const auto &value = node.second;
            checkObject(value, "Context entry");
            require(value.size() == 4,
                    "unlimited Context entry requires valid/key/slot/conf");
            const auto key = integer(value, "key");
            const auto slot = integer(value, "slot");
            const auto conf = integer(value, "conf");
            const bool valid = boolean(value, "valid");
            require(key <= HintHashMask, "Hint Context key exceeds 28 bits");
            require(keys.insert(key).second, "duplicate Hint Context key");
            require(slot <= std::numeric_limits<size_t>::max() && conf <= 3,
                    "Hint Context slot/confidence is out of range");
            if (valid) {
                require(slot < slots.size(),
                        "Hint entry references unloaded target");
                require(conf == 3,
                        "valid Hint entries must start at confidence 3");
                idealIndex.emplace(key, entries.size());
            }
            // The immutable vector position is the prediction-time identity,
            // including holes from invalid metadata rows.
            entries.push_back({valid, static_cast<uint32_t>(key >> indexBits),
                static_cast<uint32_t>(key), static_cast<size_t>(slot),
                static_cast<unsigned>(conf)});
        }
        return;
    }

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
            {valid, static_cast<uint32_t>(tag),
             static_cast<uint32_t>((tag << indexBits) | set),
             static_cast<size_t>(slot),
             static_cast<unsigned>(conf)};
    }
}

void
HintIndirect::loadFiniteV3Metadata()
{
    const auto image = rawImage(config.metadataPath, "H2PLD058", 16784);
    if (!image.empty()) {
        loadRawCam(image);
        slots.resize(32);
        for (unsigned slot = 0; slot < 32; ++slot) {
            const auto word = littleWord(image, 0x4090 + slot * 8, 8);
            require(!(word >> 47), "raw Slot reserved bits must be zero");
            slots[slot] = expandAddress(word);  // Slot zero is a valid target.
        }
        for (unsigned pos = 0; pos < 4096; ++pos) {
            const auto word = littleWord(image, 0x90 + pos * 4, 4);
            require(!(word >> 25), "raw Context reserved bits must be zero");
            const bool valid = word & (1U << 24);
            require(valid || word == 0, "invalid raw Context row must be zero");
            if (!valid)
                continue;
            const unsigned tag = word & 0x1ffff;
            const unsigned slot = (word >> 17) & 31;
            const unsigned conf = (word >> 22) & 3;
            require(!(pos & 1) || !entries[pos - 1].valid ||
                    entries[pos - 1].tag != tag,
                    "duplicate raw Context tag in one set");
            entries[pos] = {true, tag, (tag << 11) | (pos / 2), slot, conf};
        }
        return;
    }
    require(false, "finite Hint requires a CAM58 H2PLD058 image");
    const auto tree = readJson(config.metadataPath);
    require(stringField(tree, "format") == "h2p-hint" &&
            integer(tree, "version") == 7 &&
            stringField(tree, "storage") == "finite" &&
            integer(tree, "hash_version") == 7 &&
            stringField(tree, "context_encoding") == ForkContextEncoding,
            "finite Hint requires fork metadata/hash/storage version 7 and encoding");
    std::vector<std::string> order;
    for (const auto &node : arrayField(tree, "mask_order"))
        order.push_back(stringValue(node.second));
    require(order == std::vector<std::string>({"ITH", "Call", "Ret", "LHR"}),
            "finite spec v7 mask bit order mismatch");
    const auto &history = tree.get_child("history");
    checkObject(history, "history");
    const std::array<std::pair<const char *, unsigned>, 10> history_fields = {{
        {"ith_entries", 128}, {"ith_use_bits", 128},
        {"lhr_entries", 16}, {"lhr_bits", 32}, {"lhr_use_bits", 16}, {"lhr_head_bits", 5},
        {"call_entries", 128}, {"ret_entries", 128},
        {"path_use_bits", 128}, {"target_shift", 1}}};
    require(history.size() == history_fields.size(),
            "finite spec v7 history field count mismatch");
    for (const auto &[name, value] : history_fields)
        require(integer(history, name) == value,
                std::string("finite spec v7 history mismatch: ") + name);
    const auto &layout = tree.get_child("layout");
    checkObject(layout, "layout");
    const std::array<std::pair<const char *, unsigned>, 7> layout_fields = {{
        {"cam_entries", 16}, {"context_entries", 4096}, {"ways", 2},
        {"slot_entries", 32}, {"conf_queue_depth", 32},
        {"hint_meta_entries", config.metaEntries}, {"lhr_max_uncommitted", 8}}};
    require(layout.size() == layout_fields.size(),
            "finite spec v7 layout field count mismatch");
    for (const auto &[name, value] : layout_fields)
        require(integer(layout, name) == value,
                std::string("finite spec v7 layout mismatch: ") + name);
    const auto &source = tree.get_child("source");
    checkObject(source, "source");
    const auto profile_source = source.get_child_optional("profile_context_source");
    const auto online_source = source.get_child_optional("online_context_source");
    require(bool(profile_source) == bool(online_source),
            "Hint metadata context origins must occur together");
    const std::string training_source = profile_source ?
        stringField(source, "profile_context_source") : "prediction";
    require(training_source == "prediction" || training_source == "commit-pre",
            "Invalid Hint metadata profiling context origin");
    require(!online_source ||
            stringField(source, "online_context_source") == "prediction",
            "Hint online context origin must remain prediction");
    require(training_source == config.profileContext,
            "Hint metadata profiling context origin differs from requested source");
    require(!stringField(source, "workload_id").empty(),
            "finite spec v7 workload identity is empty");
    const std::regex digest("[0-9a-f]{64}");
    for (const char *name : {"checkpoint_sha256", "profile_sha256",
                             "builder_sha256"})
        require(std::regex_match(stringField(source, name), digest),
                std::string("finite spec v7 source digest mismatch: ") + name);

    bool invalid_seen = false;
    Addr previous_pc = 0;
    const auto &cam_array = arrayField(tree, "cam");
    require(cam_array.size() <= 16, "finite spec v7 CAM capacity exceeded");
    for (const auto &node : cam_array) {
        const auto &row = node.second;
        checkObject(row, "CAM entry");
        const bool valid = row.get_child_optional("valid") ?
            boolean(row, "valid") : true;
        if (!valid) {
            invalid_seen = true;
            continue;
        }
        const Addr pc = address(stringField(row, "pc"));
        const auto mask = integer(row, "mask");
        require(!invalid_seen && mask < 16 &&
                (cam.empty() || previous_pc < pc),
                "finite spec v7 CAM PCs must ascend before invalid rows");
        cam.push_back({pc, static_cast<unsigned>(mask)});
        previous_pc = pc;
    }
    std::set<Addr> seen_targets;
    const auto &slot_array = arrayField(tree, "slots");
    require(slot_array.size() <= 32, "finite spec v7 Slot capacity exceeded");
    for (const auto &node : slot_array) {
        const Addr target = address(stringValue(node.second));
        require(seen_targets.insert(target).second,
                "duplicate finite spec v7 Slot target");
        slots.push_back(target);
    }
    std::set<std::pair<unsigned, unsigned>> positions;
    std::set<std::pair<unsigned, uint32_t>> tags;
    const auto &context_array = arrayField(tree, "entries");
    require(context_array.size() <= 4096,
            "finite spec v7 Context capacity exceeded");
    for (const auto &node : context_array) {
        const auto &row = node.second;
        checkObject(row, "Context entry");
        const bool valid = row.get_child_optional("valid") ?
            boolean(row, "valid") : true;
        if (!valid)
            continue;
        const auto set = integer(row, "set");
        const auto way = integer(row, "way");
        const auto tag = integer(row, "tag");
        const auto slot = integer(row, "slot");
        const auto conf = integer(row, "conf");
        require(set < 2048 && way < 2 && tag < (1U << 17) &&
                slot < slots.size() && conf == 3,
                "finite spec v7 Context row is out of range or has invalid target");
        require(positions.emplace(set, way).second &&
                tags.emplace(set, tag).second,
                "duplicate finite spec v7 Context position or tag");
        entries[set * 2 + way] = {true, static_cast<uint32_t>(tag),
            static_cast<uint32_t>((tag << 11) | set),
            static_cast<size_t>(slot), 3};
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
    for (const auto &name : {"hint-profile.jsonl.gz", "hint-replay.jsonl.gz",
                             "hint-stats.json", "conf0.txt",
                             "hint-commit-history.jsonl.gz", "hint-detail.bin"}) {
        fatal_if(std::filesystem::exists(simout.resolve(outputName(name))),
                 "Hint output already exists: %s; use a fresh output directory",
                 outputName(name));
    }
    output = std::make_unique<Output>();
    output->confZero.open(simout.resolve(outputName("conf0.txt")));
    fatal_if(!output->confZero, "Cannot open Hint confidence-zero log");
    if (isCollect()) {
        if (!config.ideal) {
            output->detail.open(simout.resolve(outputName("hint-detail.bin")),
                                std::ios::binary | std::ios::trunc);
            fatal_if(!output->detail, "Cannot open SPEC Detail trace");
            if (output->detailAudit.is_open()) output->detailAudit.close();
            output->detailAudit.open(simout.resolve(outputName("hint-detail.bin.audit.jsonl")), std::ios::trunc);
            fatal_if(!output->detailAudit, "Cannot open Detail identity audit");
            output->detailAudit << R"({"kind":"header","version":1,"producer":"gem5-retired-cfi","cam_identity":"checked_pc_width_classification","record_bytes":22})" << "\n";
        }
        output->profile = gzopen(
            simout.resolve(outputName("hint-profile.jsonl.gz")).c_str(), "wb1");
        fatal_if(!output->profile, "Cannot open Hint profile output");
        fatal_if(gzbuffer(output->profile, 1U << 20) != 0,
                 "Cannot allocate Hint profile compression buffer");
        writeHeader();
        if (needsCommitProfileContext()) {
            output->commitHistory = gzopen(simout.resolve(
                outputName("hint-commit-history.jsonl.gz")).c_str(), "wb1");
            fatal_if(!output->commitHistory ||
                     gzbuffer(output->commitHistory, 1U << 20) != 0,
                     "Cannot open committed Hint history diagnostic trace");
            std::ostringstream header;
            header << "{\"kind\":\"header\",\"format\":\"h2p-hint-commit-history\","
                   << "\"version\":1,\"initial_state\":\"zero\","
                   << "\"profile_context_source\":\"commit-pre\","
                   << "\"online_context_source\":\"prediction\",\"tracked_pcs\":[";
            for (size_t index = 0; index < historyPcs.size(); ++index) {
                if (index) header << ',';
                header << jsonString(hexAddress(historyPcs[index]));
            }
            header << "]}\n";
            const std::string text = header.str();
            fatal_if(gzwrite(output->commitHistory, text.data(), text.size()) !=
                     int(text.size()), "Cannot write committed Hint trace header");
        }
    } else if (isReplay() && !config.ideal) {
        output->profile = gzopen(
            simout.resolve(outputName("hint-replay.jsonl.gz")).c_str(), "wb1");
        fatal_if(!output->profile, "Cannot open Hint replay trace");
        fatal_if(gzbuffer(output->profile, 1U << 20) != 0,
                 "Cannot allocate Hint replay trace buffer");
        output->writeProfile("{\"kind\":\"header\",\"format\":\"h2p-hint-replay\","
                             "\"version\":7,\"storage\":\"finite\",\"hash_version\":7,"
                             "\"context_encoding\":\"xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1\"}\n");
    }
}

void
HintIndirect::writeHeader()
{
    std::ostringstream line;
    line << "{\"kind\":\"header\",\"format\":\"h2p-hint-profile\","
         << "\"version\":" << (config.ideal ? 3 : 7)
         << ",\"hash_version\":" << (config.ideal ? 2 : 7)
         << ",\"profile_context_source\":" << jsonString(config.profileContext)
         << ",\"online_context_source\":\"prediction\"";
    if (config.ideal)
        line << ",\"ghr_bits\":970,\"mask_order\":" << MaskOrderJson
             << ",\"history\":" << HistoryJson;
    else
        line << ",\"storage\":\"finite\",\"context_encoding\":"
             << jsonString(ForkContextEncoding) << ",\"mask_order\":"
             << V3MaskOrderJson << ",\"history\":" << V3HistoryJson;
    if (config.ideal) {
        line << ",\"storage\":\"unlimited\",\"finite_reference\":"
             << FiniteReferenceJson
             << ",\"layout\":{\"conf_queue_depth\":"
             << config.confQueueDepth << '}';
    } else {
        line << ",\"layout\":{\"cam_entries\":" << config.camEntries
             << ",\"context_entries\":" << config.contextEntries
             << ",\"ways\":" << config.ways
             << ",\"slot_entries\":" << config.slotEntries
             << ",\"conf_queue_depth\":" << config.confQueueDepth
             << ",\"hint_meta_entries\":" << config.metaEntries
             << ",\"lhr_max_uncommitted\":8}";
    }
    line << ",\"clock_period\":" << config.clockPeriod << ",\"pcs\":[";
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
    fatal_if(lastQueryCycle && cycle <= *lastQueryCycle,
             "Hint permits at most one ordered block query per CPU cycle");
    // The CPU normally invokes tick before fetch; this is also safe for the
    // standalone production-helper driver and never drains twice in a cycle.
    tick(cycle);
    lastQueryCycle = cycle;
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
    // BPU/BTB ranges are ordered half-open intervals, not circular ranges.
    // A speculative target near the end of Addr can wrap the aligned block
    // end; the BTBs then retain no PC satisfying start <= pc < end. Preserve
    // that miss behavior, including the consumed query port and saved range.
    if (!snapshot || block_end <= block_start) {
        return result;
    }
    size_t matches = 0;
    auto match = [&](Addr pc, unsigned mask) {
        if (config.ideal ? (pc >= block_start && pc < block_end) :
                           pc == block_start) {
            ++matches;
            if (!result->hasCandidate || pc < result->pc) {
                result->hasCandidate = true;
                result->pc = pc;
                result->mask = mask;
            }
        }
    };
    if (config.ideal && isReplay()) {
        const auto compare = [](const CamEntry &entry, Addr pc) {
            return entry.pc < pc;
        };
        const auto first = std::lower_bound(cam.begin(), cam.end(),
                                             block_start, compare);
        const auto end = std::lower_bound(first, cam.end(), block_end, compare);
        matches = end - first;
        if (matches) {
            result->hasCandidate = true;
            result->pc = first->pc;
            result->mask = first->mask;
        }
    } else if (config.ideal) {
        const auto first = std::lower_bound(candidateIndex.begin(),
                                             candidateIndex.end(), block_start);
        const auto end = std::lower_bound(first, candidateIndex.end(), block_end);
        matches = end - first;
        if (matches) {
            result->hasCandidate = true;
            result->pc = *first;
        }
    } else if (isReplay()) {
        for (const auto &entry : cam) {
            if (entry.valid) {
                match(entry.pc, entry.mask);
                if (entry.pc == block_start) {
                    result->cfiPosition = entry.cfiPosition;
                    result->isCall = entry.isCall;
                }
            }
        }
    } else {
        for (const auto pc : candidates) {
            if (pc != MaxAddr) {
                match(pc, 0);
                if (pc == block_start) {
                    const auto &entry = cam[localIndex(pc)];
                    result->cfiPosition = entry.cfiPosition;
                    result->isCall = entry.isCall;
                }
            }
        }
    }
    if (measurementActive && matches) {
        ++queryStats.camHits;
        if (matches > 1) {
            ++queryStats.camContentions;
            queryStats.camExtraCandidates += matches - 1;
        }
    }
    if (!result->hasCandidate)
        return result;
    if (!config.ideal) {
        result->camIndex = localIndex(result->pc);
        fatal_if(result->camIndex < 0,
                 "Finite Hint CAM candidate has no physical LHR index");
        // A busy or full Hint path never delays the original BPU.
        if (cycle < busyUntil) {
            if (measurementActive) ++queryStats.busyFallbacks;
            return result;
        }
        const auto reason = reserveBlockReason(result->pc);
        if (reason != StallReason::None) {
            recordStall(reason);
            return result;
        }
        result->admitted = true;
        result->s3Cycle = cycle + 2;
        busyUntil = cycle + 3;
        busyQuery = result;
        // Existing BPU computes future-stage vectors in one call. Service
        // the already-determined cycle-N writes before sampling the S2 read.
        // Any resolve later in N becomes ready at N+1 and loses to this read.
        tickFinite(cycle + 1);
        if (isReplay()) {
            lastContextReadCycle = cycle + 1;
            if (measurementActive) ++queryStats.contextReads;
        }
    }
    if (!isReplay())
        return result;
    if (!config.ideal) {
        fatal_if(!snapshot->finite,
                 "Finite spec v7 Hint query lacks physical-ring snapshot");
        result->camIndex = localIndex(result->pc);
        fatal_if(result->camIndex < 0,
                 "Finite spec v7 CAM candidate has no physical LHR index");
        result->key = hintHashV3(result->pc,
            snapshot->v3ContextsFor(result->camIndex), result->mask);
    } else {
        result->key = hintHash(result->pc,
                              snapshot->contextsFor(result->pc),
                              result->mask);
    }
    result->set = result->key & (sets - 1);
    if (config.ideal) {
        const auto found = idealIndex.find(result->key);
        if (found != idealIndex.end()) {
            result->entryId = found->second;
        }
    } else {
        const uint32_t tag = result->key >> indexBits;
        for (unsigned way = 0; way < config.ways; ++way) {
            const size_t entry_id = result->set * config.ways + way;
            const auto &entry = entries[entry_id];
            if (entry.valid && entry.tag == tag) {
                result->way = way;
                result->entryId = entry_id;
                break;
            }
        }
    }
    if (result->entryId != std::numeric_limits<size_t>::max()) {
        const auto &entry = entries[result->entryId];
        result->tagHit = true;
        result->slot = entry.slot;
        result->conf = entry.conf;
        result->hintTarget = slots[entry.slot];
        if (measurementActive) {
            ++queryStats.tagHits;
            queryStats.confEnabled += result->conf > 0;
        }
    }
    return result;
}

int
HintIndirect::localIndex(Addr pc) const
{
    if (pc == MaxAddr)
        return -1;
    // Sixteen physical comparators, with stable slot identity across holes.
    for (size_t index = 0; index < historyPcs.size(); ++index)
        if (historyPcs[index] == pc)
            return int(index);
    return -1;
}

bool
HintIndirect::canReserve(Addr pc) const
{
    return reserveBlockReason(pc) == StallReason::None;
}

HintIndirect::StallReason
HintIndirect::reserveBlockReason(Addr pc) const
{
    if (!isFiniteV3() || localIndex(pc) < 0)
        return StallReason::None;
    if (finiteMeta.size() >= config.metaEntries)
        return StallReason::MetaQueue;
    const int index = localIndex(pc);
    return std::count_if(finiteMeta.begin(), finiteMeta.end(),
        [index](const auto &record) {
            return record->localIndex == index;
        }) < 8 ? StallReason::None : StallReason::Lhr;
}

void
HintIndirect::recordStall(StallReason reason)
{
    if (!measurementActive || reason == StallReason::None)
        return;
    switch (reason) {
      case StallReason::HistoryWindow:
        ++queueStats.historyWindowStalls;
        if (stats) ++stats->historyWindowStalls;
        break;
      case StallReason::MetaQueue:
        ++queueStats.metaQueueStalls;
        if (stats) ++stats->metaQueueStalls;
        break;
      case StallReason::Lhr:
        ++queueStats.lhrStalls;
        if (stats) ++stats->lhrStalls;
        break;
      case StallReason::None:
        break;
    }
}

void
HintIndirect::reserve(const std::shared_ptr<HintQuery> &query, Addr pc,
                      uint8_t local_phase, const HintFiniteHistory *history)
{
    if (!isFiniteV3() || !query || !query->admitted ||
        localIndex(query->pc) < 0)
        return;
    fatal_if(!canReserve(query->pc),
             "Finite Hint metadata/LHR capacity exceeded or rollback active");
    auto record = std::make_shared<FiniteMeta>();
    record->query = query;
    record->branchPC = pc;
    record->localIndex = localIndex(query ? query->pc : pc);
    record->localPhase = local_phase;
    if (history) {
        for (unsigned i = 0; i < 16; ++i)
            record->localPhases[i] = history->localPhase(i);
    } else if (query->snapshot && query->snapshot->finite) {
        record->localPhases = query->snapshot->finite->localPhases;
    }
    record->ordinal = nextFiniteOrdinal++;
    finiteMeta.push_back(record);

}

void
HintIndirect::bind(const std::shared_ptr<HintQuery> &query, uint64_t ftq_id,
                   unsigned ftq_capacity)
{
    if (!isFiniteV3() || finiteMeta.empty() ||
        finiteMeta.back()->query != query || finiteMeta.back()->ftqId)
        return;
    fatal_if(!ftq_id || !ftq_capacity,
             "Finite Hint cannot bind invalid FTQ identity/geometry");
    finiteMeta.back()->ftqId = ftq_id;
    finiteMeta.back()->ftqIndex = (ftq_id - 1) % ftq_capacity;
    finiteMeta.back()->ftqGeneration = (ftq_id - 1) / ftq_capacity;
    if (query)
        query->ftqId = ftq_id;
}

void
HintIndirect::drainFiniteResolved()
{
    while (!finiteScoreOrder.empty()) {
        auto record = finiteScoreOrder.front();
        finiteScoreOrder.pop_front();
        if (!record->scoreEligible)
            continue;
        if (finiteUpdates.size() >= 32) {
            record->dropped = true;
            ++queueStats.dropped;
            if (stats)
                ++stats->queueDropped;
            continue;
        }
        record->enqueued = true;
        record->readyCycle = record->resolveCycle + 1;
        finiteUpdates.push_back(record);
        ++queueStats.enqueued;
        queueStats.maxOccupancy = std::max<uint64_t>(
            queueStats.maxOccupancy, finiteUpdates.size());
        if (stats)
            ++stats->queueEnqueued;
    }
}

void
HintIndirect::releaseCommittedFinite()
{
    while (!finiteMeta.empty() && finiteMeta.front()->committed &&
           finiteMeta.front()->resolved &&
           !finiteMeta.front()->discarded) {
        // drainFiniteResolved already transferred the write request (or its
        // explicit drop decision) out of the metadata queue's age ordering.
        const auto record = finiteMeta.front();
        fatal_if(std::find(finiteScoreOrder.begin(), finiteScoreOrder.end(),
                           record) != finiteScoreOrder.end(),
                 "Finite Hint committed record has no owned write decision");
        finiteMeta.pop_front();
    }
}

void
HintIndirect::resolve(const std::shared_ptr<HintQuery> &query, Addr pc,
                      Addr actual, uint64_t sequence, uint64_t cycle,
                      std::optional<bool> measure_at_resolve,
                      bool actual_taken, bool actual_indirect,
                      bool actual_return, unsigned instruction_size)
{
    if (!isFiniteV3() || !query)
        return;
    const auto found = std::find_if(finiteMeta.begin(), finiteMeta.end(),
        [&](const auto &record) {
            return !record->discarded && record->query == query &&
                   record->branchPC == hintPathCoordinate(pc, instruction_size);
        });
    if (found == finiteMeta.end() || (*found)->resolved)
        return;
    auto &record = **found;
    record.resolved = true;
    query->resolved = true;
    record.actual = actual;
    record.sequence = sequence;
    record.resolveCycle = cycle;
    record.scoreEligible = actual_taken && actual_indirect && !actual_return &&
        measure_at_resolve.value_or(measurementActive) &&
        isReplay() &&
        query->eligible && query->tagHit && !query->canceled &&
        query->baselinePC == hintPathCoordinate(pc, instruction_size) &&
        query->candidatePC() == hintPathCoordinate(pc, instruction_size);
    record.newConf = trainedConfidence(*query, query->baselinePC, actual);
    finiteScoreOrder.push_back(*found);
    drainFiniteResolved();
    releaseCommittedFinite();
}

void
HintIndirect::resolveByFTQId(uint64_t ftq_id, Addr pc, Addr actual,
                             uint64_t sequence, uint64_t cycle,
                             bool measure_at_resolve, bool actual_taken, bool actual_indirect,
                             bool actual_return, unsigned instruction_size)
{
    if (!isFiniteV3())
        return;
    const auto found = std::find_if(finiteMeta.begin(), finiteMeta.end(),
        [&](const auto &record) {
            return !record->discarded && record->ftqId == ftq_id &&
                   record->branchPC == hintPathCoordinate(pc, instruction_size);
        });
    if (found != finiteMeta.end())
        resolve((*found)->query, pc, actual, sequence, cycle,
                measure_at_resolve, actual_taken, actual_indirect, actual_return, instruction_size);
}

size_t
HintIndirect::pendingRollbacks() const
{
    return 0;
}

void
HintIndirect::undoFinite(const std::shared_ptr<FiniteMeta> &record,
                         uint64_t cycle)
{
    // A 15-bit confidence FIFO request owns only set/way and its final value,
    // not an FTQ tag. Once accepted, it survives metadata squash independently
    // of whether its SRAM write has already completed.
    if (record->enqueued || record->applied)
        return;
    finiteScoreOrder.erase(std::remove(finiteScoreOrder.begin(),
        finiteScoreOrder.end(), record), finiteScoreOrder.end());
    record->enqueued = false;
    record->resolved = false;
    record->scoreEligible = false;
    record->dropped = false;
    if (record->query)
        record->query->resolved = false;
}

void
HintIndirect::redirect(HintFiniteHistory &history, uint64_t ftq_id,
                       Addr squash_pc, bool actual_h2p, Addr actual,
                       uint64_t sequence, uint64_t cycle)
{
    if (!isFiniteV3())
        return;
    // Bounded 101b metadata provides all sixteen pre-update local heads.
    // Already written confidence updates deliberately survive redirect.
    for (size_t position = finiteMeta.size(); position > 0; --position) {
        const auto record = finiteMeta[position - 1];
        if (record->discarded)
            continue;
        if (record->ftqId < ftq_id)
            break;
        if (record->ftqId == ftq_id && record->branchPC < squash_pc) {
            // Core recovery starts at the FTQ entry snapshot, then replays
            // the retained prefix. Keep this metadata but rewind its heads.
            for (unsigned i = 0; i < 16; ++i)
                history.rewindLocalPhase(i, record->localPhases[i]);
            break;
        }
        const bool keep = record->ftqId == ftq_id &&
            record->branchPC == squash_pc && actual_h2p;
        const bool old_resolved = record->resolved;
        const bool old_score_eligible = record->scoreEligible;
        const uint64_t old_cycle = record->resolveCycle;
        const uint64_t old_sequence = record->sequence;
        const Addr old_actual = record->actual;
        for (unsigned i = 0; i < 16; ++i)
            history.rewindLocalPhase(i, record->localPhases[i]);
        // Accepted requests retain their original final value, including
        // when a later recovery supplies a different target for this CFI.
        // The shared_ptr is a software encoding of an immutable 15b request.
        if (keep && old_resolved &&
            (old_actual == actual || record->enqueued || record->applied))
            break;
        if (!keep && record->query)
            record->query->canceled = true;
        undoFinite(record, cycle);
        if (keep) {
            if (old_resolved && !record->applied)
                finiteScoreOrder.push_back(record);
            record->resolved = old_resolved;
            record->actual = actual;
            record->sequence = old_resolved ? old_sequence : sequence;
            record->resolveCycle = old_resolved ? old_cycle : cycle;
            record->scoreEligible = old_resolved && old_score_eligible;
            const auto &query = *record->query;
            record->newConf = trainedConfidence(query, record->branchPC, actual);
            if (record->query)
                record->query->resolved = old_resolved;
            drainFiniteResolved();
            break;
        }
        record->discarded = true;
        finiteMeta.erase(finiteMeta.begin() + position - 1);
    }
    fatal_if(finiteMeta.size() > config.metaEntries,
             "Finite Hint active and rollback journal exceeded configured capacity");
}

void
HintIndirect::commitTarget(uint64_t ftq_id)
{
    if (!isFiniteV3())
        return;
    for (const auto &record : finiteMeta) {
        if (record->discarded)
            continue;
        fatal_if(!record->ftqId,
                 "Finite Hint metadata reservation was not bound");
        if (record->ftqId > ftq_id)
            break;
        record->committed = true;
        if (!record->resolved) {
            // An FTQ entry may have predicted a Hint branch that never
            // executed. Its commit retires the reservation without a score.
            record->resolved = true;
            if (record->query)
                record->query->resolved = true;
        }
    }
    drainFiniteResolved();
    releaseCommittedFinite();
}

void
HintIndirect::cancelPending()
{
    if (auto query = busyQuery.lock(); query && !query->ftqId)
        query->canceled = true;
    busyQuery.reset();
    // Already consumed/reserved Context port cycles remain consumed.
    busyUntil = 0;
}

void
HintIndirect::tickFinite(uint64_t cycle)
{
    if (cycle == 0)
        return;
    const uint64_t end = cycle - 1;
    uint64_t first = finiteServiceThrough ? *finiteServiceThrough + 1 : 0;
    if (first > end)
        return;
    // Prediction reads win over the one shared confidence write port.
    while (first <= end) {
        if (finiteUpdates.empty())
            break;
        const auto record = finiteUpdates.front();
        first = std::max(first, record->readyCycle);
        if (first > end)
            break;
        if (lastContextReadCycle && first == *lastContextReadCycle) {
            ++first;
            continue;
        }
        const auto &query = *record->query;
        fatal_if(query.entryId >= entries.size(),
                 "Finite Hint confidence write has invalid Context identity");
        auto &entry = entries[query.entryId];
        fatal_if(!entry.valid || entry.key != query.key ||
                 entry.slot != query.slot,
                 "Finite Hint Context identity changed before writeback");
        {
            finiteUpdates.pop_front();
            const unsigned old_conf = entry.conf;
            entry.conf = record->newConf;
            record->applied = true;
            ++queueStats.applied;
            if (stats)
                ++stats->queueApplied;
            if (old_conf > 0 && entry.conf == 0) {
                ++queueStats.zeroTransitions;
                if (stats)
                    ++stats->zeroTransitions;
                writeConfZero({record->query, record->actual, record->sequence,
                               record->resolveCycle, record->readyCycle}, first);
            } else if (old_conf == 0 && entry.conf > 0) {
                ++queueStats.recoveries;
                if (stats)
                    ++stats->recoveries;
            }
        }
        ++first;
    }
    finiteServiceThrough = end;
}

std::optional<Addr>
HintIndirect::complete(const std::shared_ptr<HintQuery> &query,
    Addr selected_pc, Addr selected_target, bool taken, bool indirect,
    bool is_return, bool btb_identified, unsigned selected_size)
{
    if (!query || !enabled() || finished) {
        return std::nullopt;
    }
    fatal_if(query->completed, "Hint query may be completed only once");
    query->completed = true;
    query->baselineValid = taken;
    query->baselinePC = isCollect() ? selected_pc : hintPathCoordinate(selected_pc, selected_size);
    query->baselineTarget = selected_target;
    query->baselineIndirect = indirect;
    query->baselineReturn = is_return;
    query->btbIdentified = btb_identified;
    if (isCollect()) {
        // CAM arbitration depends on the frozen image and is replayed offline.
        // Preserve later candidates for rebuilding a different CAM selection.
        query->eligible = query->snapshot && query->admitted &&
                          taken && indirect && !is_return && btb_identified;
        return std::nullopt;
    }
    if (!query->hasCandidate || !query->snapshot || query->canceled ||
        (!config.ideal && !query->admitted)) {
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
    } else if (config.ideal && selected_pc != query->pc) {
        cancel(&QueryCounts::cancelPcMismatch);
    } else if (!config.ideal && query->baselinePC != query->candidatePC()) {
        cancel(&QueryCounts::cancelPcMismatch);
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
    Addr pc, Addr actual, bool final_valid, Addr final_target, uint64_t cycle,
    const HintSnapshot *profile_snapshot, uint64_t commit_event_ordinal,
    Addr query_start_pc, unsigned instruction_size, bool actual_call, bool actual_return)
{
    if (!enabled() || !measurementActive || finished) {
        return;
    }
    const Addr matching_pc = isCollect() || config.ideal ? pc : hintPathCoordinate(pc, instruction_size);
    // A missing Hint snapshot cannot have changed the selected target.
    const bool have_baseline = query && query->completed;
    const bool baseline_valid = have_baseline ?
        query->baselineValid && query->baselinePC == matching_pc : final_valid;
    const Addr baseline_pc = have_baseline ? query->baselinePC : pc;
    const Addr baseline_target = have_baseline ?
        query->baselineTarget : final_target;
    const bool baseline_miss = !baseline_valid || baseline_target != actual;
    const bool final_miss = !final_valid || final_target != actual;
    const bool eligible = have_baseline && query->snapshot && query->eligible &&
                          query->baselinePC == matching_pc;
    const bool tag_hit = isReplay() && eligible && query->tagHit &&
                         query->baselinePC == matching_pc;
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
        counts.overridesMisses += overridden && final_miss;
        counts.tagHitMisses += tag_hit && query->hintTarget != actual;
    };
    record(global);
    const Addr query_pc = query_start_pc != MaxAddr ? query_start_pc :
                         (query ? query->blockStart : MaxAddr);
    auto entry = perPc.find(config.ideal ? pc : query_pc);
    if (entry != perPc.end()) {
        record(entry->second);
    }
    if (stats) {
        ++stats->committed;
        stats->corrected += baseline_miss && !final_miss;
        stats->harmed += !baseline_miss && final_miss;
    }
    if (isCollect() && entry != perPc.end()) {
        if (!config.ideal)
            writeDetail(query, query_pc, pc, actual, baseline_miss,
                        instruction_size, sequence, actual_call, actual_return);
        writeProfile(query, sequence, pc, actual, baseline_valid,
                     baseline_pc, baseline_target, eligible, cycle,
                     profile_snapshot, commit_event_ordinal);
    }
    if (isReplay() && !config.ideal && entry != perPc.end()) {
        std::ostringstream line;
        line << "{\"kind\":\"branch\",\"seq\":" << sequence
             << ",\"pc\":" << jsonString(hexAddress(pc))
             << ",\"query_pc\":" << jsonString(hexAddress(query ? query->blockStart : 0))
             << ",\"query_valid\":" << ((query && query->admitted && !query->canceled) ? "true" : "false")
             << ",\"actual\":" << jsonString(hexAddress(actual))
             << ",\"baseline_valid\":" << (baseline_valid ? "true" : "false")
             << ",\"baseline_pc\":" << jsonString(hexAddress(baseline_pc))
             << ",\"baseline_target\":" << jsonString(hexAddress(baseline_target))
             << ",\"final_valid\":" << (final_valid ? "true" : "false")
             << ",\"final_target\":" << jsonString(hexAddress(final_target))
             << ",\"eligible\":" << (eligible ? "true" : "false")
             << ",\"tag_hit\":" << (tag_hit ? "true" : "false")
             << ",\"override\":" << (overridden ? "true" : "false")
             << ",\"hint_target\":";
        if (tag_hit)
            line << jsonString(hexAddress(query->hintTarget));
        else
            line << "null";
        line << ",\"confidence\":";
        if (tag_hit)
            line << query->conf;
        else
            line << "null";
        line << ",\"corrected\":" << (baseline_miss && !final_miss ? "true" : "false")
             << ",\"harmed\":" << (!baseline_miss && final_miss ? "true" : "false")
             << ",\"query_cycle\":";
        if (query)
            line << query->queryCycle;
        else
            line << "null";
        line << ",\"commit_cycle\":" << cycle << "}\n";
        output->writeProfile(line.str());
    }
    // Include conf=0 shadow predictions. Only a real tag hit can recover;
    // an invalid SRAM entry never reaches this point.
    if (tag_hit && config.ideal) {
        enqueue(query, sequence, actual, cycle);
    }
}

void
HintIndirect::writeDetail(const std::shared_ptr<HintQuery> &query,
    Addr start_pc, Addr pc, Addr actual, bool baseline_miss,
    unsigned instruction_size, uint64_t sequence, bool actual_call, bool actual_return)
{
    if (actual_return) return;
    const int index = localIndex(start_pc);
    fatal_if(index < 0 || index >= 16,
             "Detail sample lacks a frozen physical CAM slot");
    const __int128 difference = __int128(pc) - __int128(start_pc);
    const bool length_valid = instruction_size == 2 || instruction_size == 4;
    // Use the BTB final-halfword coordinate for window ownership, but retain
    // the real instruction start in DELTA. P=Q-2 is valid for a 32-bit control.
    const __int128 cfi_end = __int128(pc) + instruction_size - 2;
    const __int128 fallthrough = ((__int128(start_pc) + 64) >> 5) << 5;
    const __int128 page_end = ((__int128(start_pc) >> 12) + 1) << 12;
    const __int128 effective_end = std::min(fallthrough, page_end);
    if (!traceAddress(start_pc) || !traceAddress(pc) ||
        !traceAddress(actual) || difference % 2 ||
        !length_valid || start_pc > MaxAddr - 64 || cfi_end > MaxAddr ||
        cfi_end < start_pc || cfi_end >= effective_end ||
        difference < -128 || difference > 126) {
        ++detailInvalidWindows;
        return;
    }
    if (!cam[index].valid ||
        cfi_end != __int128(start_pc & ~Addr(31)) + 2 * cam[index].cfiPosition)
        return;
    const bool valid = query && query->snapshot && query->snapshot->finite &&
        query->completed && query->hasCandidate && query->admitted &&
        !query->canceled && query->blockStart == start_pc &&
        query->pc == start_pc && query->blockEnd > query->blockStart &&
        cfi_end >= query->blockStart &&
        cfi_end < query->blockEnd;
    const HintV3Contexts contexts = valid ?
        query->snapshot->v3ContextsFor(index) : HintV3Contexts{};
    std::array<uint8_t, 22> record{};
    unsigned bit = 0;
    const auto field = [&](uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i, ++bit)
            record[bit / 8] |= ((value >> i) & 1) << (bit % 8);
    };
    field(index, 4);
    field((actual >> 1) & ((uint64_t(1) << 47) - 1), 47);
    // Trace layout differs from mask/hash order: ITH, LHR, Call, Ret.
    field(contexts[0], 28);
    field(contexts[3], 28);
    field(contexts[1], 28);
    field(contexts[2], 28);
    field(uint64_t(int64_t(difference / 2)) & 0x7f, 7);
    field(baseline_miss, 1);
    field(valid, 1);
    field(0, 1);
    output->detail.write(reinterpret_cast<const char *>(record.data()),
                         record.size());
    fatal_if(!output->detail, "Failed to write SPEC Detail trace");
    output->detailAudit << "{\"kind\":\"record\",\"index\":" << detailRecords
        << ",\"start_pc\":" << jsonString(hexAddress(start_pc))
        << ",\"pc\":" << jsonString(hexAddress(pc))
        << ",\"is_rvc\":" << (instruction_size == 2 ? "true" : "false")
        << ",\"is_call\":" << (actual_call ? "true" : "false")
        << ",\"is_return\":" << (actual_return ? "true" : "false")
        << ",\"is_indirect\":true,\"retired_ordinal\":" << sequence
        << ",\"baseline_miss\":" << (baseline_miss ? "true" : "false")
        << ",\"query_valid\":" << (valid ? "true" : "false")
        << ",\"target\":" << jsonString(hexAddress(actual)) << "}\n";
    fatal_if(!output->detailAudit, "Cannot write Detail identity audit");
    ++detailRecords;
    detailInvalidQueries += !valid;
}

void
HintIndirect::writeProfile(const std::shared_ptr<HintQuery> &query,
    uint64_t sequence, Addr pc, Addr actual, bool baseline_valid,
    Addr baseline_pc, Addr baseline_target, bool eligible, uint64_t cycle,
    const HintSnapshot *profile_snapshot, uint64_t commit_event_ordinal)
{
    std::ostringstream line;
    line << "{\"kind\":\"branch\",\"seq\":" << sequence
         << ",\"pc\":" << jsonString(hexAddress(pc))
         << ",\"query_pc\":" << jsonString(hexAddress(query ? query->blockStart : 0))
             << ",\"query_valid\":" << ((query && query->admitted && !query->canceled) ? "true" : "false")
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
    if (needsCommitProfileContext())
        line << ",\"commit_event_ordinal\":" << commit_event_ordinal;
    line << ",\"commit_cycle\":" << cycle << ",\"block_start\":"
         << jsonString(hexAddress(query ? query->blockStart : 0))
         << ",\"block_end\":"
         << jsonString(hexAddress(query ? query->blockEnd : 0))
         << ",\"eligible\":" << (eligible ? "true" : "false")
         << ",\"contexts\":";
    const HintSnapshot *prediction_snapshot =
        query && query->snapshot ? query->snapshot.get() : nullptr;
    fatal_if(needsCommitProfileContext() && !profile_snapshot,
             "Commit-pre Hint profile lacks retired history snapshot");
    const HintSnapshot *selected_snapshot = needsCommitProfileContext() ?
        profile_snapshot : prediction_snapshot;
    const int index = query ? localIndex(query->blockStart) : -1;
    const auto write_contexts = [&](const HintSnapshot *snapshot) {
        if (snapshot && config.ideal)
            writeContexts(line, snapshot->contextsFor(pc));
        else if (snapshot && index >= 0)
            writeContexts(line, snapshot->v3ContextsFor(index));
        else
            line << "null";
    };
    const auto write_history = [&](const HintSnapshot *snapshot) {
        if (snapshot && query && index >= 0)
            line << snapshot->historyJson(query->blockStart, index);
        else
            line << "null";
    };
    write_contexts(selected_snapshot);
    if (!config.ideal) {
        line << ",\"history\":";
        write_history(selected_snapshot);
        line << ",\"prediction_contexts\":";
        write_contexts(prediction_snapshot);
        line << ",\"prediction_history\":";
        write_history(prediction_snapshot);
    }
    line << "}\n";
    output->writeProfile(line.str());
}

uint64_t
HintIndirect::recordCommittedControl(const HintFiniteHistory &history,
    uint64_t sequence, uint64_t cycle, Addr start_pc, Addr actual_pc,
    Addr target, bool indirect, bool call, bool is_return, bool taken,
    int local_selector)
{
    if (!needsCommitProfileContext() || finished)
        return 0;
    fatal_if(!output || !output->commitHistory,
             "Committed Hint diagnostic output is not open");
    const uint64_t ordinal = ++commitEventOrdinal;
    const HintSnapshot snapshot(history);
    std::ostringstream line;
    line << "{\"kind\":\"control\",\"ordinal\":" << ordinal
         << ",\"measured\":" << (measurementActive ? "true" : "false")
         << ",\"cycle\":" << cycle << ",\"seq\":" << sequence
         << ",\"start_pc\":" << jsonString(hexAddress(start_pc))
         << ",\"actual_pc\":" << jsonString(hexAddress(actual_pc))
         << ",\"target\":" << jsonString(hexAddress(target))
         << ",\"indirect\":" << (indirect ? "true" : "false")
         << ",\"call\":" << (call ? "true" : "false")
         << ",\"return\":" << (is_return ? "true" : "false")
         << ",\"taken\":" << (taken ? "true" : "false")
         << ",\"local_selector\":" << local_selector
         << ",\"history\":" << snapshot.historyJson(start_pc, local_selector)
         << "}\n";
    const std::string text = line.str();
    fatal_if(gzwrite(output->commitHistory, text.data(), text.size()) !=
             int(text.size()), "Cannot write committed Hint control event");
    return ordinal;
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
    if (!config.ideal)
        tickFinite(cycle);
    if (!measurementActive) {
        return;
    }
    if (occupancyCycle) {
        occupancyCycles += cycle - *occupancyCycle;
    }
    occupancyCycle = cycle;
    queueStats.occupancySamples += cycle - next_cycle + 1;
    if (!config.ideal) {
        queueStats.occupancySum +=
            (cycle - next_cycle + 1) * finiteUpdates.size();
        return;
    }
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
        fatal_if(query.entryId >= entries.size(),
                 "Hint confidence update has no saved entry identity");
        auto &entry = entries[query.entryId];
        fatal_if(!entry.valid || entry.slot != query.slot ||
                 entry.key != query.key,
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
    if (config.ideal)
        writeContexts(line, query.snapshot->contextsFor(query.pc));
    else
        writeContexts(line, query.snapshot->v3ContextsFor(query.camIndex));
    line << ",\"history\":" << query.snapshot->historyJson(query.pc, query.camIndex)
         << ",\"hash\":" << query.key << ",\"set\":" << query.set
         << ",\"way\":" << query.way << ",\"slot\":" << query.slot;
    if (config.ideal) {
        line << ",\"entry_id\":" << query.entryId
             << ",\"key\":" << query.key;
    }
    line << ",\"predicted_target\":" << jsonString(hexAddress(query.hintTarget))
         << ",\"actual_target\":" << jsonString(hexAddress(update.actual))
         << ",\"query_cycle\":" << query.queryCycle
         << (config.ideal ? ",\"commit_cycle\":" :
                             ",\"resolve_cycle\":") << update.commitCycle
         << ",\"writeback_cycle\":" << cycle
         << ",\"query_tick\":" << query.queryCycle * config.clockPeriod
         << (config.ideal ? ",\"commit_tick\":" :
                             ",\"resolve_tick\":")
         << update.commitCycle * config.clockPeriod
         << ",\"writeback_tick\":" << cycle * config.clockPeriod << "}\n";
    fatal_if(!line, "Failed to write Hint confidence-zero log");
}

void
HintIndirect::setMeasurementActive(bool active)
{
    setMeasurementActive(active, lastTick.value_or(0));
}

void
HintIndirect::setMeasurementActive(bool active, uint64_t cycle)
{
    if (!enabled() || finished) {
        return;
    }
    tick(cycle);
    if (measurementActive == active) {
        return;
    }
    measurementActive = active;
    occupancyCycle = active ? std::optional<uint64_t>(cycle) : std::nullopt;
}

void
HintIndirect::resetStats()
{
    resetStats(lastTick.value_or(0));
}

void
HintIndirect::resetStats(uint64_t cycle)
{
    if (!enabled() || finished) {
        return;
    }
    tick(cycle);
    occupancyCycles = 0;
    occupancyCycle = measurementActive ?
        std::optional<uint64_t>(cycle) : std::nullopt;
    global = {};
    for (auto &[pc, counts] : perPc) {
        counts = {};
    }
    queryStats = {};
    queueStats = {};
    queueStats.carriedIn = pendingUpdates();
    queueStats.maxOccupancy = pendingUpdates();
    detailRecords = detailInvalidQueries = detailInvalidWindows = 0;
    if (output->detail.is_open()) {
        output->detail.close();
        output->detail.open(simout.resolve(outputName("hint-detail.bin")),
                            std::ios::binary | std::ios::trunc);
        fatal_if(!output->detail, "Cannot reset SPEC Detail trace");
        if (output->detailAudit.is_open()) output->detailAudit.close();
        output->detailAudit.open(simout.resolve(outputName("hint-detail.bin.audit.jsonl")), std::ios::trunc);
        fatal_if(!output->detailAudit, "Cannot open Detail identity audit");
        output->detailAudit << R"({"kind":"header","version":1,"producer":"gem5-retired-cfi","cam_identity":"checked_pc_width_classification","record_bytes":22})" << "\n";
    }
    // Reset is a statistics operation. Keep all trained confidence and FIFO
    // contents, and explicitly account for updates accepted before this reset.
    if (output->profile) {
        fatal_if(gzclose(output->profile) != Z_OK,
                 "Failed to close Hint trace before reset");
        output->profile = nullptr;
        output->profile = gzopen(
            simout.resolve(outputName(isCollect() ?
                "hint-profile.jsonl.gz" : "hint-replay.jsonl.gz")).c_str(), "wb1");
        fatal_if(!output->profile || gzbuffer(output->profile, 1U << 20) != 0,
                 "Cannot reopen Hint trace after reset");
        if (isCollect())
            writeHeader();
        else
            output->writeProfile("{\"kind\":\"header\",\"format\":\"h2p-hint-replay\","
                                 "\"version\":7,\"storage\":\"finite\",\"hash_version\":7,"
                             "\"context_encoding\":\"xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1\"}\n");
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
    fatal_if(config.ideal,
             "Ideal Hint confidence reads require a stable entry identity");
    fatal_if(set >= sets || way >= config.ways,
             "Hint confidence read position is out of range");
    return entries[set * config.ways + way].conf;
}

unsigned
HintIndirect::confidenceAtEntry(size_t entry_id) const
{
    fatal_if(entry_id >= entries.size(), "Hint entry identity is out of range");
    return entries[entry_id].conf;
}

HintIndirect::Occupancy
HintIndirect::occupancy(Table table) const
{
    const auto index = static_cast<size_t>(table);
    static constexpr std::array<uint64_t, 3> capacities = {16, 4096, 32};
    fatal_if(index >= capacities.size(), "Unknown Hint occupancy table");
    Occupancy result;
    result.finiteCapacity = capacities[index];
    if (isReplay()) {
        result.residentEntries = residentEntries[index];
        result.sampleCycles = occupancyCycles;
        if (occupancyCycles) {
            result.avgEntries = result.residentEntries;
            result.peakEntries = result.residentEntries;
        }
    }
    return result;
}

void
HintIndirect::writeStats()
{
    auto out = simout.create(outputName("hint-stats.json"), false, true);
    auto &line = *out->stream();
    line << std::setprecision(17)
         << "{\"format\":\"h2p-hint-stats\",\"version\":"
         << (config.ideal ? 3 : 7) << ",\"mode\":"
         << jsonString(config.mode) << ",\"storage\":"
         << jsonString(config.ideal ? "unlimited" : "finite")
         << ",\"occupancy_applicable\":" << (isReplay() ? "true" : "false")
         << ",\"occupancy\":{\"cam\":";
    writeOccupancy(line, occupancy(Table::Cam));
    line << ",\"context\":";
    writeOccupancy(line, occupancy(Table::Context));
    line << ",\"slots\":";
    writeOccupancy(line, occupancy(Table::Slots));
    if (!config.ideal)
        line << "},\"hash_version\":7,\"context_encoding\":"
             << jsonString(ForkContextEncoding) << ",\"global\":{";
    else
        line << "},\"global\":{";
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
         << ",\"pending\":" << pendingUpdates()
         << ",\"canceled\":" << queueStats.canceled
         << ",\"reverted\":" << queueStats.reverted
         << ",\"rollback_writes\":" << queueStats.rollbackWrites
         << ",\"rollback_fallbacks\":" << queueStats.rollbackFallbacks
         << ",\"rollback_pending\":" << pendingRollbacks()
         << ",\"rollback_read_conflicts\":" << queueStats.rollbackReadConflicts
         << ",\"zero_transitions\":" << queueStats.zeroTransitions
         << ",\"recoveries\":" << queueStats.recoveries
         << ",\"max_occupancy\":" << queueStats.maxOccupancy
         << ",\"occupancy_samples\":" << queueStats.occupancySamples
         << ",\"occupancy_sum\":" << queueStats.occupancySum
         << ",\"history_window_stalls\":" << queueStats.historyWindowStalls
         << ",\"meta_queue_stalls\":" << queueStats.metaQueueStalls
         << ",\"lhr_stalls\":" << queueStats.lhrStalls
         << "},\"query\":{\"lookups\":" << queryStats.lookups
         << ",\"cam_hits\":" << queryStats.camHits
         << ",\"cam_contentions\":" << queryStats.camContentions
         << ",\"cam_extra_candidates\":" << queryStats.camExtraCandidates
         << ",\"tag_hits\":" << queryStats.tagHits
         << ",\"busy_fallbacks\":" << queryStats.busyFallbacks
         << ",\"context_reads\":" << queryStats.contextReads
         << ",\"frontend_hint_blocked_cycles\":0"
         << ",\"conf_enabled\":" << queryStats.confEnabled
         << ",\"overrides\":" << queryStats.overrides
         << ",\"cancel_no_btb\":" << queryStats.cancelNoBtb
         << ",\"cancel_not_taken\":" << queryStats.cancelNotTaken
         << ",\"cancel_not_indirect\":" << queryStats.cancelNotIndirect
         << ",\"cancel_return\":" << queryStats.cancelReturn
         << ",\"cancel_pc_mismatch\":" << queryStats.cancelPcMismatch
         << "},\"detail\":{\"records\":" << detailRecords
         << ",\"query_invalid\":" << detailInvalidQueries
         << ",\"invalid_window\":" << detailInvalidWindows << "}}\n";
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
    fatal_if(config.ideal && queueStats.carriedIn + queueStats.enqueued !=
             queueStats.applied + updates.size(),
             "Hint confidence update accounting did not conserve entries");
    fatal_if(global.baselineMisses + global.harmed !=
             global.finalMisses + global.corrected,
             "Hint final-target accounting did not conserve misses");
    writeStats();
    if (output->detail.is_open()) {
        std::array<uint8_t, 22> final{};
        final[21] = 1U << 4;  // final is bit 172; every other field is zero.
        output->detail.write(reinterpret_cast<const char *>(final.data()),
                             final.size());
        output->detail.flush();
        fatal_if(!output->detail, "Failed to finalize SPEC Detail trace");
        output->detail.close();
        output->detailAudit << "{\"kind\":\"footer\",\"complete\":true,\"records\":"
            << detailRecords << ",\"overflow\":false,\"p_error\":0}\n";
        output->detailAudit.flush();
        fatal_if(!output->detailAudit, "Cannot finalize Detail identity audit");
        output->detailAudit.close();
    }
    // Do not advance the CPU or drain pending updates at measurement exit.
    if (output->profile) {
        fatal_if(gzclose(output->profile) != Z_OK,
                 "Failed to finish Hint profile output");
        output->profile = nullptr;
    }
    if (output->commitHistory) {
        fatal_if(gzclose(output->commitHistory) != Z_OK,
                 "Failed to finish committed Hint diagnostic trace");
        output->commitHistory = nullptr;
    }
    if (output->confZero.is_open()) {
        output->confZero.flush();
        fatal_if(!output->confZero, "Failed to finish Hint confidence log");
        output->confZero.close();
    }
}

} // namespace gem5::branch_prediction::btb_pred
