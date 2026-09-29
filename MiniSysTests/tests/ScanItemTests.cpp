#include <gtest/gtest.h>

#include "core/Scanner.h"
#include "core/TabId.h"

namespace minisys {
namespace {

TEST(ScanItemTests, DefaultsMatchV2Contract) {
    ScanItem it;
    EXPECT_EQ(it.sizeBytes, 0u);
    EXPECT_EQ(it.createTime, 0u);
    EXPECT_TRUE(it.recommended);
    EXPECT_FALSE(it.dangerous);
    // v2 fields (M0): neutral defaults.
    EXPECT_TRUE(it.ruleId.empty());
    EXPECT_EQ(it.lastWriteFiletime, 0u);
    EXPECT_EQ(it.riskLevel, RiskLevel::Cautious);
}

TEST(ScanItemTests, RiskLevelOrdering) {
    // Safe < Cautious < Advanced < InfoOnly — used for confirmation strictness.
    EXPECT_LT(RiskLevel::Safe, RiskLevel::Cautious);
    EXPECT_LT(RiskLevel::Cautious, RiskLevel::Advanced);
    EXPECT_LT(RiskLevel::Advanced, RiskLevel::InfoOnly);
}

TEST(TabIdTests, FiveTabs) {
    EXPECT_EQ(static_cast<int>(TabId::Junk), 0);
    EXPECT_EQ(static_cast<int>(TabId::History), 4);
    EXPECT_EQ(static_cast<int>(TabId::Count), 5);
}

} // namespace
} // namespace minisys
