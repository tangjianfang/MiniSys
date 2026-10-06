#include <gtest/gtest.h>

// P0 regression tests for REVIEW-2026-10-06:
//   P0-1 GuardRails canonicalization + expanded blacklist + exemptions
//   P0-2 delegate command whitelist
//   P0-3 Json whitespace/depth/number strictness
// (Split from GuardRailsTests/JsonTests to keep the original files stable.)

#include "core/GuardRails.h"
#include "core/JunkRules.h"
#include "util/Json.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace minisys {
namespace {

std::wstring RealShortName(const std::wstring& path) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetShortPathNameW(path.c_str(), buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? buf : path;
}

// ---- P0-1: the 05-T-B1 adversarial corpus (was: 5-way full-chain bypass).
TEST(GuardRailsP0, PathVariantCorpusAllDenied) {
    std::wstring shortSys = RealShortName(L"C:\\Program Files\\WindowsApps");
    EXPECT_FALSE(shortSys == L"C:\\Program Files\\WindowsApps")
        << "short name not available on this machine?";

    const std::wstring variants[] = {
        L"C:\\Windows\\System32\\cmd.exe",                 // baseline
        L"c:\\windows\\system32\\cmd.exe",                 // case
        L"C:\\\\Windows\\System32\\cmd.exe",               // doubled separator
        L"\\Windows\\System32\\cmd.exe",                   // no drive letter
        L"\\\\?\\C:\\Windows\\System32\\cmd.exe",          // \\?\ prefix
        L"\\\\localhost\\C$\\Windows\\System32\\cmd.exe",  // UNC admin share
        shortSys,                                          // real 8.3 short name
        L"C:\\Users\\..\\Windows\\System32\\cmd.exe",      // ..
        L"C:\\Windows.\\System32\\cmd.exe",                // trailing dot
        L"C:/Windows/System32/cmd.exe",                    // forward slashes
        L"C:\\Windows\\System32\\.",                       // trailing dot on dir
    };
    for (const auto& v : variants) {
        EXPECT_TRUE(GuardRails::IsProtectedPath(v))
            << "variant bypassed the gate: " << v;
    }
}

TEST(GuardRailsP0, BlacklistCoversUsersAndProgramFilesAndProgramData) {
    // 05-T-B2: these were unprotected — one misclick in the folder tree
    // could quarantine them.
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Users"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Program Files"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Program Files (x86)"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\ProgramData"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\PerfLogs"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Windows.old\\something"));
    // Subdirectories of Program Files are file-op protected too…
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Program Files\\SomeApp"));
    // …but the user profile TREE stays cleanable (the whole point).
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Users\\tjf\\AppData\\Local\\Temp"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Users\\tjf\\some-folder"));
}

TEST(GuardRailsP0, QuarantineRootsProtectedOnAllDrives) {
    // 03-B15: the old list hardcoded C/D/E.
    for (wchar_t d = L'D'; d <= L'Z'; ++d) {
        std::wstring root{ d, L':', L'\\' };
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        EXPECT_TRUE(GuardRails::IsProtectedPath(root + L"MiniSys.Quarantine\\x"))
            << root;
    }
}

TEST(GuardRailsP0, BuiltInExemptedSystemCleanupSubtreesAllowed) {
    // T-B9: the six built-in Cautious rules + do-cache must be EXECUTABLE —
    // protected C:\Windows, but punched through by the exemption table.
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Windows\\Temp"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Windows\\SoftwareDistribution\\Download"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Windows\\Logs\\CBS"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Windows\\Logs\\DISM"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(L"C:\\Windows\\Minidump"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(
        L"C:\\Windows\\ServiceProfiles\\LocalService\\AppData\\Local\\FontCache"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(
        L"C:\\ProgramData\\Microsoft\\Windows\\DeliveryOptimization\\Cache"));
    // Neighbouring system paths stay protected.
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Windows\\System32\\config"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Windows\\Temp\\..\\System32"));
}

TEST(GuardRailsP0, MigrationContextAllowsProgramFilesSources) {
    // P0-1: file ops and app migration have different protected sets.
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\Program Files\\SomeApp"));   // file op: deny
    EXPECT_FALSE(GuardRails::IsProtectedMigrationSource(L"C:\\Program Files\\SomeApp"));
    EXPECT_TRUE(GuardRails::IsProtectedMigrationSource(L"C:\\Windows"));
    EXPECT_TRUE(GuardRails::IsProtectedMigrationSource(L"C:\\Users"));
    EXPECT_TRUE(GuardRails::IsProtectedMigrationSource(L"C:\\Program Files\\WindowsApps"));
}

TEST(GuardRailsP0, CanonicalizeResolvesVariants) {
    std::wstring out;
    ASSERT_TRUE(GuardRails::Canonicalize(L"C:\\\\Windows\\\\System32", out));
    EXPECT_TRUE(out == L"C:\\Windows\\System32" || out == L"c:\\Windows\\System32")
        << out;
    ASSERT_TRUE(GuardRails::Canonicalize(RealShortName(L"C:\\Program Files"), out));
    EXPECT_NE(out.find(L"Program Files"), std::wstring::npos) << out;
    // Non-existing garbage still canonicalizes (string-level fallback).
    ASSERT_TRUE(GuardRails::Canonicalize(L"C:\\definitely\\not\\here", out));
}

// ---- P0-2: delegate command whitelist.
TEST(JunkRulesP0, DelegateCommandWhitelist) {
    std::wstring reason;
    const wchar_t* sysRoot = L"C:\\Windows";
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        sysRoot + std::wstring(L"\\System32\\Dism.exe /Online /Cleanup-Image /StartComponentCleanup"),
        reason)) << reason;
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        sysRoot + std::wstring(L"\\System32\\powercfg.exe /h off"), reason)) << reason;
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        sysRoot + std::wstring(L"\\System32\\cleanmgr.exe /VERYLOWDISK /d C:"), reason)) << reason;

    // Rejected: arbitrary exe, hijackable relative name, bad args.
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\cmd.exe /c echo pwned", reason));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"Dism.exe /Online /Cleanup-Image", reason));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        sysRoot + std::wstring(L"\\System32\\Dism.exe /Apply-Image /ImageFile:x"), reason));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        sysRoot + std::wstring(L"\\System32\\cleanmgr.exe /EVERYTHING"), reason));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"", reason));
}

TEST(JunkRulesP0, BuiltinAndShippedRulesValidateClean) {
    // All built-in rules (incl. the 6 exempted system ones and 3 delegate
    // commands with absolute paths) must survive load-time validation.
    auto result = JunkRules::LoadValidated();
    EXPECT_TRUE(result.rejected.empty())
        << (result.rejected.empty() ? L"" : result.rejected[0].first + L": " +
                                             result.rejected[0].second);
    EXPECT_GE(result.rules.size(), 27u);
}

// ---- P0-3: Json whitespace / depth / number strictness.
TEST(JsonP0, WhitespaceAfterColonAndComma) {
    Json j;
    std::wstring err;
    EXPECT_TRUE(Json::Parse(L"{ \"a\": 1, \"b\": [1, 2], \"c\": { \"x\": \"y\" } }", j, err)) << err;
    EXPECT_EQ(j.Get(L"a").AsInt(), 1);
    EXPECT_EQ(j.Get(L"b").Size(), 2u);
    // The exact shipped-rule shape: "version": 2 used to fail at offset 14.
    EXPECT_TRUE(Json::Parse(L"{\n  \"version\": 2,\n  \"rules\": [ { \"id\": \"a\", \"path\": \"C:\" } ]\n}", j, err)) << err;
}

TEST(JsonP0, ShippedRulesJsonParses) {
    // Works both from the repo root and from build\Release.
    std::ifstream f;
    f.open(L"MiniSys/rules.json", std::ios::binary);
    if (!f) f.open(L"../../MiniSys/rules.json", std::ios::binary);
    if (!f) GTEST_SKIP() << "rules.json not reachable from test cwd";
    std::string utf8((std::istreambuf_iterator<char>(f)), {});
    Json j;
    std::wstring err;
    ASSERT_TRUE(Json::Parse(Utf8ToWide(utf8), j, err)) << err;
    EXPECT_GE(j.Get(L"rules").Size(), 27u);
}

TEST(JsonP0, DepthLimitRejectsInsteadOfCrashing) {
    // 05-T-C2a: 10k nesting used to overflow the stack (0xC00000FD).
    Json j;
    std::wstring err;
    EXPECT_FALSE(Json::Parse(std::wstring(100000, L'['), j, err));
    EXPECT_FALSE(Json::Parse(std::wstring(100000, L'{'), j, err));
}

TEST(JsonP0, StrictNumbers) {
    Json j;
    std::wstring err;
    EXPECT_FALSE(Json::Parse(L"{\"a\":1.2.3}", j, err));   // was silently 1.2
    EXPECT_FALSE(Json::Parse(L"{\"a\":+1}", j, err));      // JSON forbids leading +
    EXPECT_FALSE(Json::Parse(L"{\"a\":01}", j, err));      // leading zero
    EXPECT_TRUE(Json::Parse(L"{\"a\":-1.5e-3}", j, err));
    EXPECT_EQ(j.Get(L"a").AsNumber(), -1.5e-3);
}

TEST(JsonP0, BomStripped) {
    Json j;
    std::wstring err;
    std::wstring withBom;
    withBom += static_cast<wchar_t>(0xFEFF);
    withBom += L"{\"a\":1}";
    EXPECT_TRUE(Json::Parse(withBom, j, err)) << err;
}

} // namespace
} // namespace minisys
