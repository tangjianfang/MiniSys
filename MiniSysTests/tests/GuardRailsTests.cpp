#include <gtest/gtest.h>

#include "core/GuardRails.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <fstream>

namespace minisys {
namespace {

struct ItemPair {
    PlanItem pi;
    ScanItem si;
};

ItemPair MakePair(const std::wstring& path, unsigned long long size,
                  uint64_t mtime, RiskLevel risk = RiskLevel::Cautious) {
    ItemPair p;
    p.pi.path = path;
    p.pi.sizeAtScan = size;
    p.pi.lastWriteAtScan = mtime;
    p.si.path = path;
    p.si.sizeBytes = size;
    p.si.lastWriteFiletime = mtime;
    p.si.riskLevel = risk;
    return p;
}

std::filesystem::path MakeTempFile(const std::wstring& name, const char* content) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

uint64_t MtimeOf(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
}

unsigned long long SizeOf(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
           static_cast<unsigned long long>(fad.nFileSizeLow);
}

// ---- M1 acceptance: injection test — system paths entering a plan are denied.
TEST(GuardRailsTests, InjectionSystem32IsDenied) {
    auto& g = GuardRails::Instance();
    for (const wchar_t* path : {
            L"C:\\Windows\\System32\\cmd.exe",
            L"C:\\Windows\\System32\\drivers\\etc\\hosts",
            L"C:\\Windows\\explorer.exe",
            L"C:\\Windows\\WinSxS\\somefile.dll",
            L"C:\\Program Files\\WindowsApps\\SomeApp",
            L"C:\\ProgramData\\Microsoft\\Crypto\\x",
            L"C:\\System Volume Information\\x",
            L"C:\\MiniSys.Quarantine\\stolen",
        }) {
        auto p = MakePair(path, 100, 12345);
        auto v = g.Validate(p.pi, p.si);
        EXPECT_FALSE(v.allow) << "expected deny for " << path;
        // The deny must come from the protected-path gate itself.
        EXPECT_NE(v.reason.find(L"保护"), std::wstring::npos)
            << "expected protected-path deny for " << path << ", got: " << v.reason;
    }
}

TEST(GuardRailsTests, InfoOnlyAndAdvancedRiskDenied) {
    auto& g = GuardRails::Instance();
    auto path = (std::filesystem::temp_directory_path() / L"minisys_test_gr_info.txt").wstring();
    MakeTempFile(L"minisys_test_gr_info.txt", "x");

    auto info = MakePair(path, 1, 0, RiskLevel::InfoOnly);
    EXPECT_FALSE(g.Validate(info.pi, info.si).allow);

    auto adv = MakePair(path, 1, 0, RiskLevel::Advanced);
    EXPECT_FALSE(g.Validate(adv.pi, adv.si).allow);
}

TEST(GuardRailsTests, FreshFilePasses) {
    auto& g = GuardRails::Instance();
    auto p = MakeTempFile(L"minisys_test_gr_ok.txt", "hello");
    auto pair = MakePair(p.wstring(), SizeOf(p), MtimeOf(p));
    auto v = g.Validate(pair.pi, pair.si);
    EXPECT_TRUE(v.allow) << v.reason;
}

TEST(GuardRailsTests, SizeChangeIsDenied) {
    auto& g = GuardRails::Instance();
    auto p = MakeTempFile(L"minisys_test_gr_size.txt", "hello");
    auto pair = MakePair(p.wstring(), SizeOf(p) + 999, MtimeOf(p));   // stale size
    auto v = g.Validate(pair.pi, pair.si);
    EXPECT_FALSE(v.allow);
    EXPECT_NE(v.reason.find(L"大小"), std::wstring::npos);
}

TEST(GuardRailsTests, MtimeChangeIsDenied) {
    auto& g = GuardRails::Instance();
    auto p = MakeTempFile(L"minisys_test_gr_mtime.txt", "hello");
    auto pair = MakePair(p.wstring(), SizeOf(p), MtimeOf(p) + 10000000);
    auto v = g.Validate(pair.pi, pair.si);
    EXPECT_FALSE(v.allow);
    EXPECT_NE(v.reason.find(L"修改"), std::wstring::npos);
}

TEST(GuardRailsTests, MissingPathIsDenied) {
    auto& g = GuardRails::Instance();
    auto pair = MakePair(L"C:\\definitely\\not\\here\\minisys.txt", 5, 1);
    auto v = g.Validate(pair.pi, pair.si);
    EXPECT_FALSE(v.allow);
}

TEST(GuardRailsTests, DirectorySkipsSizeComparison) {
    // Directory entries report ~0 size, so the subtree size snapshot must not
    // be compared; mtime still applies.
    auto& g = GuardRails::Instance();
    auto dir = std::filesystem::temp_directory_path() / L"minisys_test_gr_dir";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    MakeTempFile(L"minisys_test_gr_dir\\inner.txt", "xxxxxxxx");
    auto pair = MakePair(dir.wstring(), 12345678 /* subtree size */, 0 /* no mtime */);
    auto v = g.Validate(pair.pi, pair.si);
    EXPECT_TRUE(v.allow) << v.reason;
}

} // namespace
} // namespace minisys
