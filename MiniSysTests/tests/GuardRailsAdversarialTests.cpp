// review-05 follow-up (v2.12): adversarial corpus against the GuardRails
// gate and the Delegate whitelist — the security-critical paths of an
// elevated cleaner. Each case is a bypass ATTEMPT: it must be denied.
#include "core/GuardRails.h"
#include "core/JunkRules.h"
#include "core/PlanBuilder.h"
#include "platform/Junction.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace minisys {
namespace {

fs::path TempBase() {
    static fs::path base = fs::temp_directory_path() /
        (L"minisys-adv-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

// A benign item in user space (must PASS validation when untouched).
void MakeItem(const fs::path& p, unsigned long long size, uint64_t mtime,
              RiskLevel risk, PlanItem& pi, ScanItem& si) {
    pi = PlanItem{};
    pi.path = p;
    pi.sizeAtScan = size;
    pi.lastWriteAtScan = mtime;
    si = ScanItem{};
    si.path = p;
    si.sizeBytes = size;
    si.lastWriteFiletime = mtime;
    si.riskLevel = risk;
    si.strategy = CleanStrategy::Quarantine;
    si.title = p.filename().wstring();
}

uint64_t MtimeOf(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
}

} // namespace

// ---- 8.3 short names ---------------------------------------------------------

TEST(GuardRailsAdv, ShortNameExistingDenied) {
    // PROGRA~1 exists on every NTFS system volume → the final-path
    // canonicalization must expand it into the protected Program Files tree.
    EXPECT_TRUE(GuardRails::Instance().IsProtectedPath(
        L"C:\\PROGRA~1\\evil.dll"));
}

TEST(GuardRailsAdv, ShortNameNonExistingDenied) {
    // The tail does not exist — canonicalization falls back to string
    // normalization; the EXISTING short-name prefix must still expand
    // (GetLongPathNameW), or a crafted rule could smuggle system targets.
    EXPECT_TRUE(GuardRails::Instance().IsProtectedPath(
        L"C:\\PROGRA~1\\no_such_dir_zz\\evil.dll"));
}

// ---- path grammar tricks -----------------------------------------------------

TEST(GuardRailsAdv, PathGrammarVariantsDenied) {
    auto& g = GuardRails::Instance();
    EXPECT_TRUE(g.IsProtectedPath(L"C:/Windows/System32/cmd.exe"));       // fwd slashes
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\Windows\\\\System32\\cmd.exe"));  // doubled sep
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\Windows\\System32."));            // trailing dot
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\Windows\\System32 "));            // trailing space
    EXPECT_TRUE(g.IsProtectedPath(L"\\\\?\\C:\\Windows\\System32"));      // long prefix
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\Users\\Public\\..\\..\\Windows\\System32"));  // ..
    EXPECT_TRUE(g.IsProtectedPath(L"c:\\WiNdOwS\\sYsTeM32"));             // case
    EXPECT_TRUE(g.IsProtectedPath(L"\\\\server\\share\\file"));           // UNC
}

TEST(GuardRailsAdv, DriveRootDenied) {
    // A tampered results cache can carry path "C:\" / "D:\" — moving a
    // volume root must be denied by the gate, not by the filesystem.
    auto& g = GuardRails::Instance();
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\"));
    EXPECT_TRUE(g.IsProtectedPath(L"C:"));
    // Only test other roots that actually exist on this machine.
    for (const auto& root : EnumerateDrives()) {
        EXPECT_TRUE(g.IsProtectedPath(fs::path(root)))
            << "root not protected: " << root;
    }
}

TEST(GuardRailsAdv, WindowsSuffixNotProtected) {
    // "c:\windowsabc" is NOT under "c:\windows\" — trailing-backslash
    // subtree matching must not over-block user dirs.
    EXPECT_FALSE(GuardRails::Instance().IsProtectedPath(L"C:\\windowsabc-mydir"));
    // Windows.old IS its own protected subtree.
    EXPECT_TRUE(GuardRails::Instance().IsProtectedPath(L"C:\\Windows.old\\x"));
}

// ---- junctions / reparse points ----------------------------------------------

TEST(GuardRailsAdv, JunctionToWindowsDeniedAtValidate) {
    auto link = TempBase() / L"j_to_windows";
    std::wstring err;
    std::error_code ec;
    fs::remove_all(link, ec);
    if (!CreateDirectoryJunction(link, L"C:\\Windows", err)) {
        GTEST_SKIP() << "junction creation failed: " << err.c_str();
    }
    // The junction itself is not a protected PATH (it's in user temp) —
    // but Validate must refuse reparse points outright, so a quarantine
    // through the junction can never touch C:\Windows.
    PlanItem pi; ScanItem si;
    MakeItem(link, 0, MtimeOf(link), RiskLevel::Cautious, pi, si);
    auto v = GuardRails::Instance().Validate(pi, si);
    EXPECT_FALSE(v.allow);
    RemoveDirectoryReparsePoint(link, err);
}

// ---- exempt punch-throughs must not over-reach -------------------------------

TEST(GuardRailsAdv, ExemptPunchThroughScoped) {
    auto& g = GuardRails::Instance();
    EXPECT_FALSE(g.IsProtectedPath(L"C:\\Windows\\Temp\\some_cache"));      // exempt
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\Windows\\System32\\cmd.exe"));      // sibling
    EXPECT_TRUE(g.IsProtectedPath(L"C:\\Windows\\Temp\\..\\System32"));     // dot-dot back in
}

// ---- user exclusion list ------------------------------------------------------

TEST(GuardRailsAdv, UserExclusionBlocksSubtree) {
    auto dir = TempBase() / L"excluded";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto f = dir / L"a.txt";
    { std::ofstream o(f, std::ios::binary); o << "x"; }

    GuardRails::SetUserExclusions({ dir.wstring() });
    PlanItem pi; ScanItem si;
    MakeItem(f, 1, MtimeOf(f), RiskLevel::Cautious, pi, si);
    auto v = GuardRails::Instance().Validate(pi, si);
    EXPECT_FALSE(v.allow);   // inside the excluded subtree

    auto other = TempBase() / L"not_excluded_zz.txt";
    { std::ofstream o(other, std::ios::binary); o << "x"; }
    MakeItem(other, 1, MtimeOf(other), RiskLevel::Cautious, pi, si);
    EXPECT_TRUE(GuardRails::Instance().Validate(pi, si).allow);

    GuardRails::SetUserExclusions({});
}

// ---- TOCTOU re-verification ----------------------------------------------------

TEST(GuardRailsAdv, TocTouReverification) {
    auto f = TempBase() / L"toc.txt";
    { std::ofstream o(f, std::ios::binary); o << "12345"; }   // 5 bytes
    PlanItem pi; ScanItem si;

    // size changed since scan → deny
    MakeItem(f, 4, MtimeOf(f), RiskLevel::Cautious, pi, si);
    EXPECT_FALSE(GuardRails::Instance().Validate(pi, si).allow);

    // untouched → allow
    MakeItem(f, 5, MtimeOf(f), RiskLevel::Cautious, pi, si);
    EXPECT_TRUE(GuardRails::Instance().Validate(pi, si).allow);

    // mtime mismatch on a CAUTIOUS item → deny
    MakeItem(f, 5, MtimeOf(f) - 10000000ULL, RiskLevel::Cautious, pi, si);
    EXPECT_FALSE(GuardRails::Instance().Validate(pi, si).allow);

    // nonexistent → deny
    MakeItem(TempBase() / L"ghost_zz.txt", 5, 0, RiskLevel::Cautious, pi, si);
    EXPECT_FALSE(GuardRails::Instance().Validate(pi, si).allow);
}

// ---- Delegate command whitelist (cache poisoning entry point) ------------------

TEST(DelegateWhitelist, LegitimateCommandsAllowed) {
    std::wstring why;
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\Dism.exe /Online /Cleanup-Image /StartComponentCleanup", why));
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        L"c:\\windows\\system32\\dism.exe /Online /Cleanup-Image", why));
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\powercfg.exe /h off", why));
    EXPECT_TRUE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\cleanmgr.exe /AUTOCLEAN", why));
}

TEST(DelegateWhitelist, BypassAttemptsDenied) {
    std::wstring why;
    // bare name (CWD/PATH hijack)
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"Dism.exe /Online /Cleanup-Image", why));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"dism /Online /Cleanup-Image", why));
    // wrong binary
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\cmd.exe /c calc.exe", why));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"cmd /c calc.exe", why));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -c x", why));
    // copycat path outside System32
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Users\\x\\dism.exe /Online /Cleanup-Image", why));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"D:\\tools\\dism.exe /Online /Cleanup-Image", why));
    // argument-prefix abuse: powercfg only allows /h|-h|/hibernate
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\powercfg.exe /deleteacing jz", why));
    // dism with non-cleanup args
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\Dism.exe /Apply-Image /ImageFile:x", why));
    // newline / chaining smuggle
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\Dism.exe /Online /Cleanup-Image\ncmd /c calc", why));
    // empty / garbage
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"", why));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(L"   ", why));
}

} // namespace minisys
