#ifndef __CPU_PRED_BTB_HINT_HISTORY_HH__
#define __CPU_PRED_BTB_HINT_HISTORY_HH__

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "cpu/pred/btb/hint_types.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

struct FetchTarget;

using HintV3Contexts = std::array<uint32_t, 4>;

uint16_t hintFold14(Addr pc);
uint32_t hintFold128(const std::array<uint64_t, 2> &value);
uint32_t hintHashV3(Addr pc, const HintV3Contexts &contexts, uint8_t mask);
// Directed production-helper checks, invoked by the final gem5.opt test
// SimObject. This does not allocate a guest or use a separate gtest binary.
void verifyHintContextForkHistory();

// The finite fork keeps the full history in physical rings. A block snapshot holds
// only the heads, the six cells touched by a path XOR, and the hash read
// windows; it never copies a 128-cell ring into every FTQ entry.
class HintFiniteHistory
{
  public:
    struct Ring
    {
        std::array<uint8_t, 128> cells{};
        // Logical 8-bit phase; physical cell index is head & 127.
        uint8_t head = 255;
        uint64_t writes = 0;
    };
    struct LocalRing
    {
        std::array<uint8_t, 16> cells{};
        uint8_t head = 15;
        uint8_t phase = 31;
        uint64_t writes = 0;
    };
    struct Snapshot
    {
        std::array<uint8_t, 3> heads{};
        std::array<uint64_t, 3> writes{};
        std::array<uint16_t, 2> pathLow12{};
        // Read snapshots are simulation observers, not physical rollback data.
        std::array<std::array<uint64_t, 2>, 3> low128{};
        std::array<uint16_t, 16> localLow16{};
        std::array<uint8_t, 16> localPhases{};
    };

    void configure(const std::vector<Addr> &pcs);
    Snapshot snapshot() const;
    void restore(const Snapshot &snapshot);
    bool canUpdateAfter(const Snapshot &oldest, bool indirect,
                        bool call, bool is_return, bool taken) const;
    void update(Addr pc, Addr target, bool indirect, bool call,
                bool is_return, bool taken);
    int localIndex(Addr pc) const;
    uint8_t localHead(unsigned index) const;
    uint8_t localPhase(unsigned index) const;
    void restoreLocalPhase(unsigned index, uint8_t phase);
    uint16_t localLow16(unsigned index) const;
    std::array<uint64_t, 2> low128(const Ring &ring) const;
    // Legacy test observers. Production fork hashing uses only 128/16 reads.
    uint8_t localLow8(unsigned index) const;
    uint64_t low64(const Ring &ring) const;
    uint16_t low12(const Ring &ring) const;
    HintV3Contexts contexts(const Snapshot &snapshot, int local_index) const;
    const std::vector<Addr> &trackedPCs() const { return pcs; }

    Ring ith;
    Ring calls;
    Ring returns;
    std::array<LocalRing, 16> locals{};

  private:
    std::vector<Addr> pcs;
    static void push(Ring &ring, uint8_t bits);
    static void pushPath(Ring &ring, Addr pc);
};

// Exactly 64 events, newest at age zero. The incremental fold removes the
// oldest event before rotating, rather than retaining an unbounded XOR tail.
struct HintPCWindow
{
    std::array<uint64_t, HintPCEvents> words{};
    unsigned head = 0;
    unsigned count = 0;
    uint32_t folded = 0;

    void push(Addr pc);
    uint64_t atAge(unsigned age) const;
};

// Finite mode copies its fixed local-history array at prediction. Ideal mode
// shares an immutable balanced index root and copies only an update's path;
// neither snapshots nor rollback copy a candidate-sized mutable table.
class HintHistory
{
  public:
    void configure(const std::vector<Addr> &pcs, bool ideal = false);
    void update(Addr pc, Addr target, bool indirect, bool call,
                bool is_return, bool taken);
    void recoverForSquash(const FetchTarget &target, Addr squash_pc,
                           bool control_squash);
    uint8_t local(Addr pc) const;
    uint32_t globalFold() const;

    uint64_t globalLow = 0;
    uint64_t globalHigh = 0;
    HintPCWindow calls;
    HintPCWindow returns;
    std::array<uint8_t, HintMaxProfilePCs> locals{};
    std::shared_ptr<const std::vector<Addr>> localPCs;

  private:
    struct LocalNode;
    using LocalRoot = std::shared_ptr<const LocalNode>;

    // Candidate rank determines a fixed balanced tree. A null subtree denotes
    // all-zero histories, so configure() does not allocate per-PC tree nodes.
    static uint8_t localValue(const LocalRoot &root, size_t first, size_t last,
                             size_t index);
    static LocalRoot appendLocal(const LocalRoot &root, size_t first,
                                 size_t last, size_t index, uint8_t bits);

    bool idealLocals = false;
    LocalRoot localRoot;
};

// Shared only as const by queries and FTQ entries. This is the block-entry
// context, before this block contributes any speculative history update.
struct HintSnapshot
{
    HintSnapshot(const HintHistory &state,
                 const boost::dynamic_bitset<> &global_history);
    explicit HintSnapshot(const HintFiniteHistory &state,
                          bool collect_full_copy = false);

    HintContexts contextsFor(Addr pc) const;
    HintV3Contexts v3ContextsFor(int local_index) const;
    std::string historyJson(Addr pc, int local_index = -1) const;

    std::unique_ptr<HintHistory> history;
    boost::dynamic_bitset<> ghr;
    uint32_t ghrFold;
    std::optional<HintFiniteHistory::Snapshot> finite;
    // Simulation-only collect observer: exact rollback without replay stalls.
    std::unique_ptr<HintFiniteHistory> collectHistory;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_HINT_HISTORY_HH__
