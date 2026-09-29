#include <gtest/gtest.h>

#include "core/VolumeIndex.h"
#include "platform/Privilege.h"

#include <windows.h>
#include <algorithm>

namespace minisys {
namespace {

class VolumeIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& idx = VolumeIndex::Instance();
        idx.ResetForTesting();
        // Synthetic tree:
        //   C:\                (frn 5, self-parented root)
        //   ├─ Users\          (10)
        //   │  └─ tjf\         (11)
        //   │     └─ file.txt  (12, file)
        //   └─ Temp\           (20)
        //      └─ a.tmp        (21, file)
        idx.AddNodeForTesting(5, 5, L"", true, 0);            // root
        idx.AddNodeForTesting(10, 5, L"Users", true, 100);
        idx.AddNodeForTesting(11, 10, L"tjf", true, 200);
        idx.AddNodeForTesting(12, 11, L"file.txt", false, 300);
        idx.AddNodeForTesting(20, 5, L"Temp", true, 400);
        idx.AddNodeForTesting(21, 20, L"a.tmp", false, 500);
        idx.FinalizeForTesting(L'C');
    }
};

TEST_F(VolumeIndexTest, HasDirIsCaseInsensitive) {
    auto& idx = VolumeIndex::Instance();
    EXPECT_TRUE(idx.HasDir(L"C:\\Users"));
    EXPECT_TRUE(idx.HasDir(L"c:\\users\\tjf"));
    EXPECT_TRUE(idx.HasDir(L"C:\\Temp\\"));
    EXPECT_FALSE(idx.HasDir(L"C:\\Windows"));
    EXPECT_FALSE(idx.HasDir(L"C:\\Users\\tjf\\file.txt"));   // it's a file
}

TEST_F(VolumeIndexTest, HasFileAndTryGetEntry) {
    auto& idx = VolumeIndex::Instance();
    EXPECT_TRUE(idx.HasFile(L"C:\\Users\\tjf\\file.txt"));
    EXPECT_TRUE(idx.HasFile(L"c:\\TEMP\\A.TMP"));
    EXPECT_FALSE(idx.HasFile(L"C:\\Users\\tjf\\nope.txt"));

    VolumeIndex::FileEntry e;
    ASSERT_TRUE(idx.TryGetEntry(L"C:\\Users\\TJF", e));
    EXPECT_TRUE(e.isDirectory);
    EXPECT_EQ(e.lastWrite, 200u);
    ASSERT_TRUE(idx.TryGetEntry(L"C:\\Temp\\a.tmp", e));
    EXPECT_FALSE(e.isDirectory);
    EXPECT_EQ(e.lastWrite, 500u);
    EXPECT_FALSE(idx.TryGetEntry(L"C:\\Windows", e));
}

TEST_F(VolumeIndexTest, CollectChildren) {
    auto& idx = VolumeIndex::Instance();
    std::vector<VolumeIndex::FileEntry> kids;
    bool known = idx.CollectChildren(L"C:\\", [&](const VolumeIndex::FileEntry& e) {
        kids.push_back(e);
    });
    ASSERT_TRUE(known);
    ASSERT_EQ(kids.size(), 2u);   // Users, Temp
    EXPECT_TRUE(kids[0].isDirectory);
    EXPECT_TRUE(kids[1].isDirectory);

    kids.clear();
    EXPECT_FALSE(idx.CollectChildren(L"C:\\NotThere", [&](const VolumeIndex::FileEntry&) {}));
    EXPECT_TRUE(kids.empty());
}

TEST_F(VolumeIndexTest, CollectSubtreeWalksAll) {
    auto& idx = VolumeIndex::Instance();
    std::vector<VolumeIndex::FileEntry> all;
    ASSERT_TRUE(idx.CollectSubtree(L"C:\\Users", [&](const VolumeIndex::FileEntry& e) {
        all.push_back(e);
    }));
    ASSERT_EQ(all.size(), 2u);   // descendants only (root itself excluded)
    // Path casing: original index names preserved.
    EXPECT_EQ(all[0].path, L"C:\\Users\\tjf");
    EXPECT_EQ(all[1].path, L"C:\\Users\\tjf\\file.txt");
    EXPECT_FALSE(all[1].isDirectory);
    EXPECT_EQ(all[1].lastWrite, 300u);
}

TEST_F(VolumeIndexTest, SyntheticIndexIsNotVolumeValid) {
    // Test-built indexes are queryable but not volume-valid (no USN backing).
    EXPECT_FALSE(VolumeIndex::Instance().IsValid());
}

TEST(VolumeIndexIntegration, RealBuildRequiresAdmin) {
    // FSCTL_ENUM_USN_DATA needs an elevated process. Non-elevated test runs
    // must gracefully fail (the FastWalk fallback path in JunkScanner).
    auto& idx = VolumeIndex::Instance();
    idx.ResetForTesting();
    std::atomic<bool> cancel{false};
    bool built = idx.EnsureBuilt(L'C', nullptr, cancel);
    if (!IsElevated()) {
        EXPECT_FALSE(built) << "USN enumeration should fail without elevation";
    } else {
        // Elevated: the build must succeed and answer a known dir.
        EXPECT_TRUE(built);
        EXPECT_TRUE(idx.HasDir(L"C:\\Windows"));
        EXPECT_GT(idx.EntryCount(), 1000u);
    }
}

TEST_F(VolumeIndexTest, IncrementalChangesApply) {
    auto& idx = VolumeIndex::Instance();
    using Change = VolumeIndex::Change;

    // Rename Temp → Temp2 (same FRN, new name): an upsert.
    Change rename;
    rename.frn = 20;
    rename.parentFrn = 5;
    rename.name = L"Temp2";
    rename.attrs = FILE_ATTRIBUTE_DIRECTORY;
    rename.lastWrite = 999;
    // Create a new file under Users\tjf.
    Change create;
    create.frn = 30;
    create.parentFrn = 11;
    create.name = L"new.log";
    create.lastWrite = 888;
    // Delete a.tmp.
    Change del;
    del.frn = 21;
    del.deleted = true;

    ASSERT_TRUE(idx.ApplyChangesForTesting({rename, create, del}));

    EXPECT_FALSE(idx.HasDir(L"C:\\Temp"));
    EXPECT_TRUE(idx.HasDir(L"C:\\Temp2"));
    EXPECT_FALSE(idx.HasFile(L"C:\\Temp\\a.tmp"));
    EXPECT_TRUE(idx.HasFile(L"C:\\Users\\tjf\\new.log"));

    VolumeIndex::FileEntry e;
    ASSERT_TRUE(idx.TryGetEntry(L"C:\\Temp2", e));
    EXPECT_EQ(e.lastWrite, 999u);

    // Subtree under the renamed dir still walks.
    std::vector<VolumeIndex::FileEntry> kids;
    ASSERT_TRUE(idx.CollectChildren(L"C:\\Temp2",
        [&](const VolumeIndex::FileEntry& k) { kids.push_back(k); }));
    EXPECT_TRUE(kids.empty());   // a.tmp was deleted
}

TEST_F(VolumeIndexTest, HugeChangeSetRejected) {
    auto& idx = VolumeIndex::Instance();
    std::vector<VolumeIndex::Change> many(20001);
    for (auto& c : many) {
        c.frn = 90000 + &c - many.data();
        c.parentFrn = 5;
        c.name = L"x";
    }
    EXPECT_FALSE(idx.ApplyChangesForTesting(many));   // full rebuild is cheaper
}

} // namespace
} // namespace minisys
