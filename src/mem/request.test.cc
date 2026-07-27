#include <gtest/gtest.h>

#include "mem/request.hh"

namespace gem5
{

TEST(XsMetadataTest, CrossPageDefaultsToFalse)
{
    const Request::XsMetadata metadata;

    EXPECT_FALSE(metadata.crossPagePrefetch);
}

TEST(XsMetadataTest, CrossPagePrefetchConstructor)
{
    const Request::XsMetadata metadata(
        SStride, 2, 65, 2, 3, true);

    EXPECT_EQ(metadata.prefetchSource, SStride);
    EXPECT_EQ(metadata.prefetchDepth, 2);
    EXPECT_EQ(metadata.prefetchDistance, 65);
    EXPECT_EQ(metadata.preferredPrefetchLevel, 2);
    EXPECT_EQ(metadata.issuedPrefetchLevel, 3);
    EXPECT_TRUE(metadata.crossPagePrefetch);
}

TEST(XsMetadataTest, InvalidateClearsCrossPage)
{
    Request::XsMetadata metadata(
        SStride, 2, 65, 2, 3, true);

    metadata.invalidate();

    EXPECT_FALSE(metadata.validXsMetadata);
    EXPECT_FALSE(metadata.crossPagePrefetch);
}

} // namespace gem5
