#ifndef __CPU_PRED_BTB_HINT_TYPES_HH__
#define __CPU_PRED_BTB_HINT_TYPES_HH__

#include <array>
#include <cstdint>

#include "base/types.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

constexpr unsigned HintHashBits = 28;
constexpr uint32_t HintHashMask = (1U << HintHashBits) - 1;
constexpr unsigned HintGHRBits = 970;
constexpr unsigned HintGlobalTargets = 64;
constexpr unsigned HintLocalTargets = 4;
constexpr unsigned HintPCEvents = 64;
constexpr unsigned HintMaxProfilePCs = 20;
constexpr unsigned HintS3Stage = 2;
constexpr uint64_t HintAddressWordMask = (1ULL << 47) - 1;

// Components are unrotated folds in mask-bit order: GHR, GITHR, LITHR,
// Call and Return. Keeping this type separate prevents profile/replay drift.
using HintContexts = std::array<uint32_t, 5>;

uint32_t hintFold28(uint64_t value);
uint32_t hintRotate28(uint32_t value, unsigned amount);
uint32_t hintHash(Addr pc, const HintContexts &contexts, uint8_t mask);

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_HINT_TYPES_HH__
