#include <stdexcept>

#include "cpu/o3/mop_cache.hh"
#include "gtest/gtest.h"

namespace gem5
{
namespace o3
{
namespace
{

MopContext
context(ThreadID tid = 0, uint64_t satp = 1)
{
    MopContext context;
    context.tid = tid;
    context.satp = satp;
    context.vsatp = 2;
    context.hgatp = 3;
    context.privilege = 1;
    context.virt = false;
    context.vtypeReady = true;
    context.vtype = 4;
    return context;
}

TEST(MopCacheTest, RejectsInvalidParameters)
{
    EXPECT_THROW(MopCache({0, 1, 1, 1, 1}), std::invalid_argument);
    EXPECT_THROW(MopCache({4, 0, 1, 1, 1}), std::invalid_argument);
    EXPECT_THROW(MopCache({5, 2, 1, 1, 1}), std::invalid_argument);
    EXPECT_THROW(MopCache({4, 2, 0, 1, 1}), std::invalid_argument);
    EXPECT_THROW(MopCache({4, 2, 1, 0, 1}), std::invalid_argument);
    EXPECT_THROW(MopCache({4, 2, 1, 1, 0}), std::invalid_argument);
}

TEST(MopCacheTest, ReturnsSequentialBundleAndStopsAtExclusiveEnd)
{
    MopCache cache({16, 2, 8, 1, 8});
    const auto ctx = context();
    EXPECT_EQ(cache.fill(0x1000, 0x1002, nullptr, true, ctx),
              MopCache::FillResult::Filled);
    EXPECT_EQ(cache.fill(0x1002, 0x1006, nullptr, false, ctx),
              MopCache::FillResult::Filled);
    EXPECT_EQ(cache.fill(0x1006, 0x1008, nullptr, true, ctx),
              MopCache::FillResult::Filled);

    auto result = cache.lookupBundle(0x1000, ctx, 0x1006);
    ASSERT_TRUE(result.portAvailable);
    ASSERT_EQ(result.entries.size(), 2);
    EXPECT_EQ(result.entries[0].pc, 0x1000);
    EXPECT_TRUE(result.entries[0].compressed);
    EXPECT_EQ(result.entries[1].pc, 0x1002);
    EXPECT_FALSE(result.entries[1].compressed);
    EXPECT_EQ(result.nextPC, 0x1006);
    EXPECT_TRUE(result.reachedEnd);
    EXPECT_FALSE(result.missed);
}

TEST(MopCacheTest, ReportsPartialHitAndFirstMiss)
{
    MopCache cache({16, 2, 8, 1, 8});
    const auto ctx = context();
    cache.fill(0x2000, 0x2004, nullptr, false, ctx);

    auto result = cache.lookupBundle(0x2000, ctx);
    ASSERT_EQ(result.entries.size(), 1);
    EXPECT_TRUE(result.missed);
    EXPECT_EQ(result.missPC, 0x2004);
    EXPECT_EQ(result.nextPC, 0x2004);
}

TEST(MopCacheTest, ContextUsesEveryArchitecturalField)
{
    MopCache cache({32, 4, 1, 1, 8});
    const auto base = context();
    cache.fill(0x3000, 0x3004, nullptr, false, base);

    auto expect_miss = [&](MopContext changed) {
        cache.beginCycle();
        EXPECT_TRUE(cache.lookupBundle(0x3000, changed).missed);
    };

    auto changed = base;
    changed.tid++;
    expect_miss(changed);
    changed = base;
    changed.satp++;
    expect_miss(changed);
    changed = base;
    changed.vsatp++;
    expect_miss(changed);
    changed = base;
    changed.hgatp++;
    expect_miss(changed);
    changed = base;
    changed.privilege++;
    expect_miss(changed);
    changed = base;
    changed.virt = !changed.virt;
    expect_miss(changed);
    changed = base;
    changed.vtypeReady = !changed.vtypeReady;
    expect_miss(changed);
    changed = base;
    changed.vtype++;
    expect_miss(changed);
}

TEST(MopCacheTest, EnforcesReadAndFillTokensPerCycle)
{
    MopCache cache({8, 2, 1, 1, 2});
    const auto ctx = context();
    EXPECT_EQ(cache.fill(0x4000, 0x4004, nullptr, false, ctx),
              MopCache::FillResult::Filled);
    EXPECT_EQ(cache.fill(0x4004, 0x4008, nullptr, false, ctx),
              MopCache::FillResult::Filled);
    EXPECT_EQ(cache.fill(0x4008, 0x400c, nullptr, false, ctx),
              MopCache::FillResult::NoBandwidth);

    EXPECT_TRUE(cache.lookupBundle(0x4000, ctx).portAvailable);
    EXPECT_FALSE(cache.lookupBundle(0x4000, ctx).portAvailable);
    cache.beginCycle();
    EXPECT_TRUE(cache.lookupBundle(0x4000, ctx).portAvailable);
    EXPECT_EQ(cache.fillSlotsAvailable(), 2);
}

TEST(MopCacheTest, UsesTrueLruReplacementWithinSet)
{
    // Four entries, two ways: 0x0, 0x4, and 0x8 all map to set zero.
    MopCache cache({4, 2, 1, 1, 4});
    const auto ctx = context();
    cache.fill(0x0, 0x2, nullptr, true, ctx);
    cache.fill(0x4, 0x6, nullptr, true, ctx);
    EXPECT_EQ(cache.size(), 2);

    cache.lookupBundle(0x0, ctx);
    EXPECT_EQ(cache.fill(0x8, 0xa, nullptr, true, ctx),
              MopCache::FillResult::Evicted);
    EXPECT_EQ(cache.size(), 2);

    cache.beginCycle();
    EXPECT_FALSE(cache.lookupBundle(0x0, ctx).missed);
    cache.beginCycle();
    EXPECT_TRUE(cache.lookupBundle(0x4, ctx).missed);
    cache.beginCycle();
    EXPECT_FALSE(cache.lookupBundle(0x8, ctx).missed);
}

TEST(MopCacheTest, BoundsLookupByConfiguredWidth)
{
    MopCache cache({16, 2, 2, 1, 4});
    const auto ctx = context();
    cache.fill(0x5000, 0x5002, nullptr, true, ctx);
    cache.fill(0x5002, 0x5004, nullptr, true, ctx);
    cache.fill(0x5004, 0x5006, nullptr, true, ctx);

    auto result = cache.lookupBundle(0x5000, ctx);
    EXPECT_EQ(result.entries.size(), 2);
    EXPECT_EQ(result.nextPC, 0x5004);
    EXPECT_TRUE(result.widthLimited);
    EXPECT_FALSE(result.missed);
}

TEST(MopCacheTest, InvalidateDropsAllContextsAndPayloads)
{
    MopCache cache({8, 2, 1, 1, 4});
    cache.fill(0x6000, 0x6004, nullptr, false, context(0));
    cache.fill(0x6000, 0x6004, nullptr, false, context(1));
    EXPECT_EQ(cache.size(), 2);
    EXPECT_EQ(cache.invalidate(), 2);
    EXPECT_EQ(cache.size(), 0);

    cache.beginCycle();
    EXPECT_TRUE(cache.lookupBundle(0x6000, context(0)).missed);
    EXPECT_EQ(cache.invalidate(), 0);
}

} // anonymous namespace
} // namespace o3
} // namespace gem5
