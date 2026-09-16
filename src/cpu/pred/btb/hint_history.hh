#ifndef __CPU_PRED_BTB_HINT_HISTORY_HH__
#define __CPU_PRED_BTB_HINT_HISTORY_HH__

#include <array>
#include <cstddef>
#include <memory>
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

    HintContexts contextsFor(Addr pc) const;
    std::string historyJson(Addr pc) const;

    HintHistory history;
    boost::dynamic_bitset<> ghr;
    uint32_t ghrFold;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_HINT_HISTORY_HH__
