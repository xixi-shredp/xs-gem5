#include <gtest/gtest.h>

#include <optional>
#include <sstream>

#include "cpu/pred/btb/h2p_indirect_stats.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

namespace
{

void
expectMissConservation(const std::string &output)
{
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string field;
        ASSERT_TRUE(static_cast<bool>(std::getline(fields, field, ',')));
        ASSERT_TRUE(static_cast<bool>(std::getline(fields, field, ',')));
        const uint64_t pc_misses = std::stoull(field);
        uint64_t target_misses = 0;
        while (std::getline(fields, field, ',')) {
            const auto open = field.find('(');
            const auto close = field.find(')');
            ASSERT_NE(open, std::string::npos);
            ASSERT_NE(close, std::string::npos);
            ASSERT_GT(close, open + 1);
            target_misses += std::stoull(field.substr(open + 1, close - open - 1));
        }
        EXPECT_EQ(target_misses, pc_misses) << line;
    }
}

std::string
render(const H2PIndirectStats &stats)
{
    std::ostringstream out;
    stats.write(out);
    expectMissConservation(out.str());
    return out.str();
}

}  // namespace

TEST(H2PIndirectStatsTest, CorrectTargetDoesNotMiss)
{
    H2PIndirectStats stats;
    stats.record(0x100, 0x400, 0x400);

    EXPECT_TRUE(render(stats).empty());
}

TEST(H2PIndirectStatsTest, IncludesIndirectCallsAndJumpsButExcludesReturns)
{
    EXPECT_TRUE(H2PIndirectStats::eligibleIndirect(true, false));
    EXPECT_FALSE(H2PIndirectStats::eligibleIndirect(true, true));
    EXPECT_FALSE(H2PIndirectStats::eligibleIndirect(false, false));
}

TEST(H2PIndirectStatsTest, SelectsFinalIttageTarget)
{
    const auto target = H2PIndirectStats::selectFinalTarget(0x100, true, 0x100, 0x400);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, 0x400);

    H2PIndirectStats correct;
    correct.record(0x100, 0x400, target);
    EXPECT_TRUE(render(correct).empty());

    H2PIndirectStats wrong;
    wrong.record(0x100, 0x600, target);
    EXPECT_EQ(render(wrong), "0x100, 1, 0x600(1)\n");
}

TEST(H2PIndirectStatsTest, AcceptsFinalBtbFallbackWithoutIttageMetadata)
{
    const auto target = H2PIndirectStats::selectFinalTarget(0x100, true, 0x100, 0x500);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, 0x500);

    H2PIndirectStats correct;
    correct.record(0x100, 0x500, target);
    EXPECT_TRUE(render(correct).empty());

    H2PIndirectStats wrong;
    wrong.record(0x100, 0x600, target);
    EXPECT_EQ(render(wrong), "0x100, 1, 0x600(1)\n");
}

TEST(H2PIndirectStatsTest, RejectsNotTakenAndOtherBranchPredictions)
{
    EXPECT_FALSE(H2PIndirectStats::selectFinalTarget(
        0x100, false, 0x100, 0x400).has_value());
    EXPECT_FALSE(H2PIndirectStats::selectFinalTarget(
        0x100, true, 0x200, 0x400).has_value());
}

TEST(H2PIndirectStatsTest, WrongAndUnavailableTargetsMiss)
{
    H2PIndirectStats stats;
    stats.record(0x100, 0x400, 0x500);
    stats.record(0x200, 0x600, std::nullopt);

    EXPECT_EQ(render(stats),
              "0x100, 1, 0x400(1)\n"
              "0x200, 1, 0x600(1)\n");
}

TEST(H2PIndirectStatsTest, DeduplicatesActualTargets)
{
    H2PIndirectStats stats;
    stats.record(0x100, 0x400, std::nullopt);
    stats.record(0x100, 0x400, std::nullopt);

    EXPECT_EQ(render(stats), "0x100, 2, 0x400(2)\n");
}

TEST(H2PIndirectStatsTest, CountsMissesPerActualTarget)
{
    H2PIndirectStats stats;
    stats.record(0x100, 0x400, std::nullopt);
    stats.record(0x100, 0x400, 0x500);
    stats.record(0x100, 0x500, 0x400);
    stats.record(0x100, 0x500, 0x500);
    stats.record(0x100, 0x600, 0x600);

    EXPECT_EQ(render(stats), "0x100, 3, 0x400(2), 0x500(1), 0x600(0)\n");
}

TEST(H2PIndirectStatsTest, RetainsTargetsSeenBeforeFirstMiss)
{
    H2PIndirectStats stats;
    stats.record(0x100, 0x700, 0x700);
    stats.record(0x100, 0x500, std::nullopt);

    EXPECT_EQ(render(stats), "0x100, 1, 0x500(1), 0x700(0)\n");
}

TEST(H2PIndirectStatsTest, SortsPCsAndTargets)
{
    H2PIndirectStats stats;
    stats.record(0x300, 0x900, std::nullopt);
    stats.record(0x100, 0x700, std::nullopt);
    stats.record(0x100, 0x500, std::nullopt);

    EXPECT_EQ(render(stats),
              "0x100, 2, 0x500(1), 0x700(1)\n"
              "0x300, 1, 0x900(1)\n");
}

TEST(H2PIndirectStatsTest, ClearDropsMissesAndTargets)
{
    H2PIndirectStats stats;
    stats.record(0x100, 0x400, std::nullopt);
    stats.clear();

    EXPECT_TRUE(render(stats).empty());

    stats.record(0x100, 0x600, std::nullopt);
    EXPECT_EQ(render(stats), "0x100, 1, 0x600(1)\n");
}

TEST(H2PIndirectStatsTest, PrefixesZeroAddressesAsHexadecimal)
{
    H2PIndirectStats stats;
    stats.record(0, 0, std::nullopt);

    EXPECT_EQ(render(stats), "0x0, 1, 0x0(1)\n");
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
