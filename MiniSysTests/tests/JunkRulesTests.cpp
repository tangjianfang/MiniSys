#include <gtest/gtest.h>

#include "core/JunkRules.h"
#include "util/Json.h"

namespace minisys {
namespace {

TEST(JunkRulesTests, BuiltinRulesLoadWhenNoJsonFile) {
    // The test exe directory has no rules.json → built-in fallback (R-005).
    auto rules = JunkRules::Load();
    ASSERT_FALSE(rules.empty());
    // Spot checks for the v2 rule set (multi-profile browsers, dev caches,
    // delegate strategy for WinSxS).
    bool hasProfiles = false, hasDevCache = false, hasWinSxs = false, hasInfo = false;
    for (const auto& r : rules) {
        if (r.mode == RuleMode::Profiles) hasProfiles = true;
        if (r.category == L"Dev Cache") hasDevCache = true;
        if (r.id == L"winsxs") {
            hasWinSxs = true;
            EXPECT_EQ(r.strategy, CleanStrategy::Delegate);
            EXPECT_EQ(r.riskLevel, RiskLevel::Advanced);
        }
        if (r.id == L"pagefile") {
            hasInfo = true;
            EXPECT_EQ(r.strategy, CleanStrategy::InfoOnly);
        }
    }
    EXPECT_TRUE(hasProfiles);
    EXPECT_TRUE(hasDevCache);
    EXPECT_TRUE(hasWinSxs);
    EXPECT_TRUE(hasInfo);
}

TEST(JunkRulesTests, SafeRulesAreRecommended) {
    auto rules = JunkRules::Load();
    for (const auto& r : rules) {
        if (r.riskLevel == RiskLevel::Safe && r.minAgeDays == 0) {
            EXPECT_TRUE(r.recommended) << r.id;
        }
        if (r.riskLevel >= RiskLevel::Advanced) {
            EXPECT_FALSE(r.recommended) << r.id;
        }
    }
}

TEST(JunkRulesTests, WildcardMatching) {
    EXPECT_TRUE(JunkRules::MatchWildcard(L"thumbcache_*.db", L"thumbcache_16.db"));
    EXPECT_TRUE(JunkRules::MatchWildcard(L"thumbcache_*.db", L"THUMBCACHE_16.DB"));  // case-insensitive
    EXPECT_FALSE(JunkRules::MatchWildcard(L"thumbcache_*.db", L"iconcache_16.db"));
    EXPECT_TRUE(JunkRules::MatchWildcard(L"", L""));
    EXPECT_TRUE(JunkRules::MatchWildcard(L"*", L"anything"));
    EXPECT_TRUE(JunkRules::MatchWildcard(L"a?c", L"abc"));
    EXPECT_FALSE(JunkRules::MatchWildcard(L"a?c", L"abbc"));
}

TEST(JunkRulesTests, EnumParsing) {
    EXPECT_EQ(JunkRules::ParseRiskLevel(L"safe"), RiskLevel::Safe);
    EXPECT_EQ(JunkRules::ParseRiskLevel(L"CAUTIOUS"), RiskLevel::Cautious);
    EXPECT_EQ(JunkRules::ParseRiskLevel(L"advanced"), RiskLevel::Advanced);
    EXPECT_EQ(JunkRules::ParseRiskLevel(L"info"), RiskLevel::InfoOnly);
    EXPECT_EQ(JunkRules::ParseRiskLevel(L"junk"), RiskLevel::Cautious);

    EXPECT_EQ(JunkRules::ParseStrategy(L"delegate"), CleanStrategy::Delegate);
    EXPECT_EQ(JunkRules::ParseStrategy(L"info"), CleanStrategy::InfoOnly);
    EXPECT_EQ(JunkRules::ParseStrategy(L"quarantine"), CleanStrategy::Quarantine);

    EXPECT_EQ(JunkRules::ParseMode(L"children"), RuleMode::Children);
    EXPECT_EQ(JunkRules::ParseMode(L"profiles"), RuleMode::Profiles);
    EXPECT_EQ(JunkRules::ParseMode(L"subtree"), RuleMode::Subtree);
}

TEST(JunkRulesTests, ExpandEnvResolvesKnownVars) {
    auto expanded = JunkRules::ExpandEnv(L"%TEMP%\\sub");
    EXPECT_NE(expanded.find(L"\\sub"), std::wstring::npos);
    EXPECT_EQ(expanded.find(L'%'), std::wstring::npos);
    // No placeholder → unchanged.
    EXPECT_EQ(JunkRules::ExpandEnv(L"C:\\plain"), L"C:\\plain");
}

} // namespace
} // namespace minisys
