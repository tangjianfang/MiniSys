#include <gtest/gtest.h>

#include "core/PlanBuilder.h"

namespace minisys {
namespace {

ScanItem MakeItem(const wchar_t* path, unsigned long long size,
                  uint64_t mtime, const wchar_t* title = L"t") {
    ScanItem it;
    it.path = path;
    it.sizeBytes = size;
    it.lastWriteFiletime = mtime;
    it.title = title;
    return it;
}

TEST(PlanBuilderTests, BuildDeduplicatesIdenticalPaths) {
    std::vector<ScanItem> items{MakeItem(L"C:\\a", 10, 1), MakeItem(L"C:\\b", 20, 2)};
    // Same path selected twice (R-006-style duplicate rows) → single plan item.
    auto plan = PlanBuilder::Build(TabId::Junk, items, {0, 1, 0});
    ASSERT_EQ(plan.items.size(), 2u);
    EXPECT_EQ(plan.items[0].path.wstring(), L"C:\\a");
    EXPECT_EQ(plan.items[1].path.wstring(), L"C:\\b");
    EXPECT_NE(plan.planHash, 0u);
}

TEST(PlanBuilderTests, PlanMatchesSameList) {
    std::vector<ScanItem> items{MakeItem(L"C:\\a", 10, 1), MakeItem(L"C:\\b", 20, 2)};
    auto plan = PlanBuilder::Build(TabId::Junk, items, {0, 1});
    EXPECT_TRUE(PlanBuilder::PlanMatches(plan, items));
}

TEST(PlanBuilderTests, PlanStaleAfterRescan) {
    std::vector<ScanItem> items{MakeItem(L"C:\\a", 10, 1), MakeItem(L"C:\\b", 20, 2)};
    auto plan = PlanBuilder::Build(TabId::Junk, items, {0, 1});
    // A rescan replaced the results — index 1 now points at a different item.
    std::vector<ScanItem> rescanned{MakeItem(L"C:\\a", 10, 1),
                                    MakeItem(L"C:\\OTHER", 99, 9)};
    EXPECT_FALSE(PlanBuilder::PlanMatches(plan, rescanned));

    // No rescan, same content → still matches.
    EXPECT_TRUE(PlanBuilder::PlanMatches(plan, items));
}

TEST(PlanBuilderTests, OutOfRangeIndicesIgnored) {
    std::vector<ScanItem> items{MakeItem(L"C:\\a", 10, 1)};
    auto plan = PlanBuilder::Build(TabId::Junk, items, {0, 5, 99});
    ASSERT_EQ(plan.items.size(), 1u);
    EXPECT_EQ(plan.items[0].itemIdx, 0u);
    EXPECT_EQ(plan.items[0].sizeAtScan, 10u);
    EXPECT_EQ(plan.items[0].lastWriteAtScan, 1u);
}

TEST(PlanBuilderTests, MigrationContextCarried) {
    std::vector<ScanItem> items{MakeItem(L"C:\\App", 10, 1)};
    auto plan = PlanBuilder::Build(TabId::Apps, items, {0}, L"D:\\Migrated", true);
    EXPECT_EQ(plan.migrateTargetRoot, L"D:\\Migrated");
    EXPECT_TRUE(plan.useSymlink);
    EXPECT_EQ(plan.tab, TabId::Apps);
}

} // namespace
} // namespace minisys
