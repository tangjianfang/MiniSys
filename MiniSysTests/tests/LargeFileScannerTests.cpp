// v2.26: the 大文件/去重 tab listed .git\objects\pack\*.pack as ordinary
// candidates (风险 column "—"), and two clones of one repo hold byte-identical
// packs — the duplicate pass pre-selected the older copy for deletion, i.e.
// "clean up" would have corrupted that clone. These tests pin the fix.
#include <gtest/gtest.h>

#include "core/LargeFileScanner.h"

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace minisys {
namespace {

using FC = LargeFileScanner::FileClass;

TEST(LargeFileClassifyTest, EverythingBelowAVcsDirectoryIsInternal) {
    EXPECT_EQ(LargeFileScanner::Classify(
                  L"D:\\Razer\\Anne\\ThxV3Native\\.git\\objects\\pack\\pack-1.pack"),
              FC::VcsInternal);
    EXPECT_EQ(LargeFileScanner::Classify(L"d:/repo/.GIT/objects/x"),   // case + '/'
              FC::VcsInternal);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\r\\.svn\\pristine\\ab\\x.svn-base"),
              FC::VcsInternal);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\r\\.hg\\store\\data\\x.d"),
              FC::VcsInternal);
}

TEST(LargeFileClassifyTest, LookAlikesAreNotInternal) {
    // The directory itself: removing a whole repo is a deliberate act on the folder.
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\repo\\.git"), FC::Normal);
    // Substring traps.
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\repo\\.github\\workflows\\ci.yml"),
              FC::Normal);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\my.git.backup\\movie.mkv"), FC::Normal);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\git\\objects\\pack\\x.pack"), FC::Normal);
}

TEST(LargeFileClassifyTest, RebuildableCachesAreRegenerable) {
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\p\\.vs\\p\\v17\\.ipch\\a\\LAMP.ipch"),
              FC::Regenerable);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\build\\CMAKE_PCH.PCH"), FC::Regenerable);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\build\\x64\\a.obj"), FC::Regenerable);
    // A dot in a DIRECTORY name is not an extension.
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\a.obj\\readme.txt"), FC::Normal);
    EXPECT_EQ(LargeFileScanner::Classify(L"D:\\movies\\film.mkv"), FC::Normal);
}

class LargeFileScannerGitTest : public ::testing::Test {
protected:
    void SetUp() override {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        root_ = fs::path(tmp) / L"minisys-lfs-git-test";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    void Put(const fs::path& rel, char fill) {
        fs::path p = root_ / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << std::string(4096, fill);
    }
    fs::path root_;
};

TEST_F(LargeFileScannerGitTest, IdenticalPacksAreNeverDuplicatesAndAreMarkedDangerous) {
    // Two "clones" with byte-identical packs, and two identical ordinary files.
    Put(L"a\\.git\\objects\\pack\\pack-x.pack", 'G');
    Put(L"b\\.git\\objects\\pack\\pack-x.pack", 'G');
    Put(L"a\\data\\big.bin", 'N');
    Put(L"b\\data\\big.bin", 'N');

    LargeFileScanner::Config cfg;
    cfg.roots = { root_ };
    cfg.minBytes = 1024;
    LargeFileScanner scanner(cfg);

    std::vector<ScanItem> out;
    std::atomic<bool> cancel{false};
    scanner.Scan(out, ProgressFn{}, cancel);

    int dupRows = 0, dupPreselected = 0;
    bool sawPackRows = false;
    for (const auto& it : out) {
        const bool inGit = it.path.wstring().find(L"\\.git\\") != std::wstring::npos;
        if (!it.groupKey.empty()) {                       // duplicate-group row
            ++dupRows;
            if (it.recommended) ++dupPreselected;
            EXPECT_FALSE(inGit) << "repo internals must not be offered as duplicates: "
                                << it.path.string();
        } else if (inGit) {                               // plain top-N row
            sawPackRows = true;
            EXPECT_TRUE(it.dangerous) << "pack rows need the extra confirmation";
            EXPECT_FALSE(it.recommended);
        } else {
            EXPECT_FALSE(it.dangerous);
        }
    }
    EXPECT_EQ(dupRows, 2);          // the two big.bin copies — the control case
    EXPECT_EQ(dupPreselected, 1);   // one kept, one pre-selected: dedup still works
    EXPECT_TRUE(sawPackRows);       // still LISTED (they do take space) — just flagged
}

} // namespace
} // namespace minisys
