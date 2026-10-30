// v2.24 (r7-04 U-1): RemoveAllLongPath — the "tdot." incident.
// fs::remove_all on a quarantine root containing ONE Win32-illegal leaf
// name (trailing dot/space) deleted everything else, reported the whole
// drive failed, and 清空隔离区 never marked a single record released for
// 23 days. These tests pin the helper that replaced it.
#include <gtest/gtest.h>

#include "platform/Junction.h"
#include "util/PathUtils.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace minisys {
namespace {

class RemoveAllLongPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        root_ = fs::path(tmp) / L"minisys-rmtest";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // Create a file whose name only \\?\-prefixed APIs can address.
    void CreatePoisonName(const std::wstring& leaf) {
        std::wstring p = L"\\\\?\\" + (root_ / leaf).wstring();
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(h, INVALID_HANDLE_VALUE) << "leaf=" << leaf;
        CloseHandle(h);
    }

    fs::path root_;
};

TEST_F(RemoveAllLongPathTest, RemovesTrailingDotAndSpaceNames) {
    CreatePoisonName(L"tdot.");
    CreatePoisonName(L"trailing ");
    std::error_code ec;
    fs::create_directories(root_ / L"sub", ec);
    { std::ofstream f(root_ / L"sub" / L"a.txt", std::ios::binary); f << "x"; }

    std::vector<std::wstring> survivors;
    size_t failed = RemoveAllLongPath(root_.wstring(), &survivors);
    EXPECT_EQ(failed, 0u);
    EXPECT_TRUE(survivors.empty());
    EXPECT_FALSE(fs::exists(root_, ec));   // the root itself is gone too
}

TEST_F(RemoveAllLongPathTest, ReportsLockedFileAsSurvivor) {
    { std::ofstream f(root_ / L"normal.txt", std::ios::binary); f << "x"; }
    { std::ofstream f(root_ / L"locked.txt", std::ios::binary); f << "x"; }
    // Open WITHOUT FILE_SHARE_DELETE → DeleteFileW fails while held.
    HANDLE h = CreateFileW((root_ / L"locked.txt").wstring().c_str(),
                           GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);

    std::vector<std::wstring> survivors;
    size_t failed = RemoveAllLongPath(root_.wstring(), &survivors);
    CloseHandle(h);

    EXPECT_EQ(failed, 1u);
    ASSERT_EQ(survivors.size(), 1u);
    EXPECT_NE(survivors[0].find(L"locked.txt"), std::wstring::npos);
    std::error_code ec;
    EXPECT_TRUE(fs::exists(root_ / L"locked.txt", ec));   // still there
    EXPECT_FALSE(fs::exists(root_ / L"normal.txt", ec));  // the rest went
}

TEST_F(RemoveAllLongPathTest, JunctionRemovedWithoutTouchingTarget) {
    // r7-05 verified fs::remove_all does not follow junctions — the
    // replacement must keep that property (a planted junction must never
    // delete through to its target).
    std::error_code ec;
    fs::path victim = root_.parent_path() / L"minisys-rmtest-victim";
    fs::remove_all(victim, ec);
    fs::create_directories(victim, ec);
    { std::ofstream f(victim / L"precious.txt", std::ios::binary); f << "x"; }

    std::wstring jerr;
    ASSERT_TRUE(CreateDirectoryJunction(root_ / L"link", victim, jerr))
        << jerr;

    std::vector<std::wstring> survivors;
    size_t failed = RemoveAllLongPath(root_.wstring(), &survivors);
    EXPECT_EQ(failed, 0u);
    EXPECT_FALSE(fs::exists(root_ / L"link", ec));   // the link is gone
    EXPECT_TRUE(fs::exists(victim / L"precious.txt", ec));  // target intact
    fs::remove_all(victim, ec);
}

TEST_F(RemoveAllLongPathTest, MissingRootIsNoOpSuccess) {
    std::vector<std::wstring> survivors;
    size_t failed = RemoveAllLongPath(
        (root_ / L"does-not-exist").wstring(), &survivors);
    EXPECT_EQ(failed, 0u);
    EXPECT_TRUE(survivors.empty());
}

TEST_F(RemoveAllLongPathTest, EmptyFileArgumentIsSafe) {
    std::vector<std::wstring> survivors;
    EXPECT_EQ(RemoveAllLongPath(L"", &survivors), 0u);
    EXPECT_EQ(RemoveAllLongPath(L"relative\\path", &survivors), 0u);
}

// v2.25: configurable scan drives — the shared "C;D" parser behind the
// 扫描盘 picker (settings.json round-trip + scanner configs).
TEST(DriveListTest, ParsesLettersWithOrWithoutColonAndSeparators) {
    auto v = ParseDriveList(L"c;D:, e:\\ f");
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], L"C:\\");
    EXPECT_EQ(v[1], L"D:\\");
    EXPECT_EQ(v[2], L"E:\\");
    EXPECT_EQ(v[3], L"F:\\");
}

TEST(DriveListTest, DropsGarbageAndDeduplicates) {
    // "1", "CD", "C:\\x" and "" are not drive tokens; the second "c" is a dup.
    auto v = ParseDriveList(L"1;CD;C:\\x;;c;C");
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], L"C:\\");
    EXPECT_TRUE(ParseDriveList(L"").empty());
}

TEST(DriveListTest, FormatRoundTripsAndKeepsOrder) {
    auto v = ParseDriveList(L"d;c");
    EXPECT_EQ(FormatDriveList(v), L"D;C");
    EXPECT_EQ(ParseDriveList(FormatDriveList(v)), v);
    EXPECT_EQ(FormatDriveList({}), L"");
}

TEST(DriveListTest, IsOnAnyDriveMatchesCaseInsensitively) {
    auto roots = ParseDriveList(L"D;E");
    EXPECT_TRUE(IsOnAnyDrive(L"d:\\Games\\x", roots));
    EXPECT_TRUE(IsOnAnyDrive(L"E:\\", roots));
    EXPECT_FALSE(IsOnAnyDrive(L"C:\\Windows", roots));
    EXPECT_FALSE(IsOnAnyDrive(L"relative\\path", roots));
    EXPECT_FALSE(IsOnAnyDrive(L"D:\\x", {}));
}

} // namespace
} // namespace minisys
