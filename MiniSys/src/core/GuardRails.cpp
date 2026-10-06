#include "core/GuardRails.h"

#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <mutex>
#include <shared_mutex>

#ifndef FILE_VOLUME_NAME_DOS
#define FILE_VOLUME_NAME_DOS 0x0
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_DATA_OPEN 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_OFFLINE
#define FILE_ATTRIBUTE_OFFLINE 0x00001000
#endif

namespace fs = std::filesystem;

namespace minisys {

namespace {

// ---- protected-path policy tables (v2.2, REVIEW P0-1) -------------------
// All entries are stored canonicalized + lowercased; subtree entries keep a
// trailing backslash so "c:\windows\" matches "c:\windows\system32" but not
// "c:\windows.old" or "c:\windowsapps".

// Minimal env expansion without pulling JunkRules.h into GuardRails
// (the full loader lives in JunkRules.cpp).
std::wstring JunkRulesExpand(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, MAX_PATH * 4);
    if (n == 0 || n > MAX_PATH * 4) return s;
    return buf;
}

struct PathPolicy {
    // File operations (quarantine / delete).
    std::vector<std::wstring> fileExact;      // e.g. "c:\users"
    std::vector<std::wstring> fileSubtree;    // e.g. "c:\windows\"
    // App migration.
    std::vector<std::wstring> migExact;
    std::vector<std::wstring> migSubtree;
    // Built-in exempted cleanup subtrees (punch through fileSubtree).
    std::vector<std::wstring> exemptSubtree;  // canonical, trailing '\'
    std::vector<std::wstring> exemptExact;    // for rule validation
};

PathPolicy& Policy() {
    static PathPolicy p = [] {
        PathPolicy pol;
        auto sd = ToLower(SystemDriveRoot());          // "c:\"
        auto root = SystemDriveRoot();

        auto sub = [&pol](std::vector<std::wstring>& list,
                          const std::wstring& base, const wchar_t* rel) {
            std::wstring s = rel
                ? (fs::path(base) / rel).wstring()
                : fs::path(base).wstring();
            s = ToLower(s);
            if (!s.empty() && s.back() != L'\\') s += L'\\';
            list.push_back(s);
        };
        auto exact = [](std::vector<std::wstring>& list, const std::wstring& p) {
            std::wstring s = ToLower(p);
            while (!s.empty() && s.back() == L'\\') s.pop_back();
            list.push_back(s);
        };

        // --- file operations ---
        exact(pol.fileExact, fs::path(root) / L"Users");   // root itself only
        sub(pol.fileSubtree, root, L"Windows");
        sub(pol.fileSubtree, root, L"Windows.old");
        sub(pol.fileSubtree, root, L"Program Files");
        sub(pol.fileSubtree, root, L"Program Files (x86)");
        sub(pol.fileSubtree, root, L"ProgramData");
        sub(pol.fileSubtree, root, L"PerfLogs");
        sub(pol.fileSubtree, root, L"$Recycle.Bin");
        sub(pol.fileSubtree, root, L"System Volume Information");
        sub(pol.fileSubtree, root, L"$WinREAgent");
        sub(pol.fileSubtree, root, L"Recovery");
        for (const auto& drive : EnumerateDrives()) {
            sub(pol.fileSubtree, drive, L"MiniSys.Quarantine");
        }

        // --- app migration ---
        exact(pol.migExact, fs::path(root) / L"Users");
        sub(pol.migSubtree, root, L"Windows");
        sub(pol.migSubtree, root, L"Windows.old");
        sub(pol.migSubtree, root, L"Program Files\\WindowsApps");
        sub(pol.migSubtree, root, L"ProgramData\\Microsoft");
        sub(pol.migSubtree, root, L"$Recycle.Bin");
        sub(pol.migSubtree, root, L"System Volume Information");
        sub(pol.migSubtree, root, L"$WinREAgent");
        sub(pol.migSubtree, root, L"Recovery");
        for (const auto& drive : EnumerateDrives()) {
            sub(pol.migSubtree, drive, L"MiniSys.Quarantine");
        }

        // --- built-in exempted system cleanup subtrees (T-B9 fix) ---
        // Rules pointing exactly here are executable; canonical form via
        // GetFullPathNameW (they may not exist yet on a given machine).
        auto exempt = [&pol](const wchar_t* envPath) {
            std::wstring expanded = JunkRulesExpand(envPath);
            wchar_t buf[MAX_PATH * 2];
            DWORD n = GetFullPathNameW(expanded.c_str(), MAX_PATH * 2, buf, nullptr);
            std::wstring s = (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n)
                                                         : expanded;
            s = ToLower(s);
            std::wstring withSlash = s;
            if (!withSlash.empty() && withSlash.back() != L'\\') withSlash += L'\\';
            pol.exemptSubtree.push_back(withSlash);
            pol.exemptExact.push_back(s);
        };
        exempt(L"%SystemRoot%\\Temp");
        exempt(L"%SystemRoot%\\SoftwareDistribution\\Download");
        exempt(L"%SystemRoot%\\Logs\\CBS");
        exempt(L"%SystemRoot%\\Logs\\DISM");
        exempt(L"%SystemRoot%\\Minidump");
        exempt(L"%SystemRoot%\\ServiceProfiles\\LocalService\\AppData\\Local\\FontCache");
        exempt(L"%ProgramData%\\Microsoft\\Windows\\DeliveryOptimization\\Cache");

        return pol;
    }();
    return p;
}

// REVIEW P2: user exclusion list ("永不动这个文件夹"), canonicalized on set.
std::vector<std::wstring>& UserExclusions() {
    static std::vector<std::wstring> v;
    return v;
}
std::shared_mutex& UserExclusionsMu() {
    static std::shared_mutex m;
    return m;
}

void StripPrefix(std::wstring& s) {
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        s = L"\\\\" + s.substr(8);       // keep UNC form visible
    } else if (s.rfind(L"\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }
}

// `low` equals subtree root (minus trailing '\') or starts with it.
bool UnderDir(const std::wstring& low, const std::wstring& subWithSlash) {
    if (low.size() < subWithSlash.size() - 1) return false;
    if (low.compare(0, subWithSlash.size(), subWithSlash) == 0) return true;
    // exact dir itself: "c:\windows" vs "c:\windows\"
    return low.size() == subWithSlash.size() - 1 &&
           low.compare(0, low.size(), subWithSlash, 0, low.size()) == 0;
}

bool MatchAny(const std::wstring& low, const std::vector<std::wstring>& exact) {
    for (const auto& e : exact) {
        if (low == e) return true;
    }
    return false;
}

bool MatchAnySub(const std::wstring& low, const std::vector<std::wstring>& subs) {
    for (const auto& s : subs) {
        if (UnderDir(low, s)) return true;
    }
    return false;
}

// FILETIME helpers --------------------------------------------------------
bool GetFileTimes(const fs::path& p, unsigned long long& sizeOut,
                  unsigned long long& mtimeOut) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    sizeOut = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
              static_cast<unsigned long long>(fad.nFileSizeLow);
    mtimeOut = (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
               static_cast<unsigned long long>(fad.ftLastWriteTime.dwLowDateTime);
    return true;
}

} // namespace

GuardRails& GuardRails::Instance() {
    static GuardRails inst;
    return inst;
}

bool GuardRails::Canonicalize(const std::wstring& in, std::wstring& out) {
    if (in.empty()) return false;
    out.clear();

    // v2.12 (adversarial corpus): a bare drive letter ("C:") is
    // drive-relative (CWD), which CreateFileW happily resolves to some
    // arbitrary directory — read it CONSERVATIVELY as the volume root so
    // the gate sees it as protected instead.
    if (in.size() == 2 && in[1] == L':' &&
        ((in[0] >= L'a' && in[0] <= L'z') ||
         (in[0] >= L'A' && in[0] <= L'Z'))) {
        out = std::wstring(1, in[0]) + L":\\";
        return true;
    }

    // 1) Final-path resolution for existing paths: expands 8.3 short names,
    //    "..", doubled separators, and strips \\?\-style prefixes. Open the
    //    reparse point itself (never traverse into a junction target).
    HANDLE h = CreateFileW(in.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 2,
                                            FILE_VOLUME_NAME_DOS);
        CloseHandle(h);
        if (n > 0 && n < MAX_PATH * 2) {
            out = buf;
            StripPrefix(out);
            return true;
        }
        if (n >= MAX_PATH * 2) {
            // Path longer than the stack buffer — retry on the heap.
            std::vector<wchar_t> big(n + 1);
            HANDLE h2 = CreateFileW(in.c_str(), 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
            if (h2 != INVALID_HANDLE_VALUE) {
                DWORD n2 = GetFinalPathNameByHandleW(h2, big.data(),
                                                     static_cast<DWORD>(big.size()),
                                                     FILE_VOLUME_NAME_DOS);
                CloseHandle(h2);
                if (n2 > 0 && n2 < big.size()) {
                    out = big.data();
                    StripPrefix(out);
                    return true;
                }
            }
        }
    }

    // 2) Fallback for non-existing paths: pure string normalization, then
    //    expand the LONGEST EXISTING ANCESTOR to its long form — otherwise
    //    an 8.3 prefix ("C:\PROGRA~1\...") with a nonexistent tail smuggles
    //    a protected target past the string comparison (v2.12 adversarial
    //    corpus finding).
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(in.c_str(), MAX_PATH * 2, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) return false;
    out = buf;
    {
        fs::path cur = out;
        std::wstring tail;   // nonexistent components below the ancestor
        std::error_code ec;
        for (; cur != cur.root_path(); cur = cur.parent_path()) {
            if (fs::exists(cur, ec)) {
                wchar_t lb[MAX_PATH * 2];
                DWORD ln = GetLongPathNameW(cur.wstring().c_str(), lb,
                                            MAX_PATH * 2);
                if (ln > 0 && ln < MAX_PATH * 2 &&
                    _wcsicmp(lb, cur.wstring().c_str()) != 0) {
                    out = std::wstring(lb) + tail;
                }
                break;
            }
            std::wstring name = cur.filename().wstring();
            if (!name.empty()) tail = L"\\" + name + tail;
        }
    }
    StripPrefix(out);
    return !out.empty();
}

// v2.12 (adversarial corpus): the volume root itself is never a legal
// target — "C:\" / "D:\" (e.g. from a tampered results cache) used to pass
// the subtree lists untouched.
static bool IsDriveRootLower(const std::wstring& low) {
    return low.size() >= 2 && low[1] == L':' &&
           (low.size() == 2 ||
            (low.size() == 3 && (low[2] == L'\\' || low[2] == L'/')));
}

bool GuardRails::IsProtectedPath(const fs::path& p) {
    const auto& pol = Policy();
    std::wstring canon;
    if (!Canonicalize(p.wstring(), canon)) return true;   // deny-by-default
    if (canon.size() >= 2 && canon[0] == L'\\' && canon[1] == L'\\') {
        return true;   // UNC: no network file operations in this product
    }
    std::wstring low = ToLower(canon);
    if (IsDriveRootLower(low)) return true;

    if (MatchAny(low, pol.fileExact)) return true;
    if (MatchAnySub(low, pol.fileSubtree)) {
        // Built-in exempted cleanup subtrees punch through.
        if (MatchAnySub(low, pol.exemptSubtree)) return false;
        return true;
    }
    return false;
}

bool GuardRails::IsProtectedMigrationSource(const fs::path& p) {
    const auto& pol = Policy();
    std::wstring canon;
    if (!Canonicalize(p.wstring(), canon)) return true;
    if (canon.size() >= 2 && canon[0] == L'\\' && canon[1] == L'\\') return true;
    std::wstring low = ToLower(canon);
    if (IsDriveRootLower(low)) return true;
    return MatchAny(low, pol.migExact) || MatchAnySub(low, pol.migSubtree);
}

bool GuardRails::IsExemptSystemCleanupPath(const fs::path& p) {
    const auto& pol = Policy();
    std::wstring canon;
    if (!Canonicalize(p.wstring(), canon)) return false;
    std::wstring low = ToLower(canon);
    return MatchAny(low, pol.exemptExact) || MatchAnySub(low, pol.exemptSubtree);
}

bool GuardRails::IsExemptSystemCleanupRoot(const fs::path& p) {
    const auto& pol = Policy();
    std::wstring canon;
    if (!Canonicalize(p.wstring(), canon)) return false;
    return MatchAny(ToLower(canon), pol.exemptExact);
}

void GuardRails::SetUserExclusions(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> canon;
    for (const auto& p : paths) {
        std::wstring c;
        if (Canonicalize(p, c)) canon.push_back(ToLower(c));
    }
    std::unique_lock<std::shared_mutex> g(UserExclusionsMu());
    UserExclusions() = std::move(canon);
}

bool GuardRails::IsUserExcluded(const fs::path& p) {
    std::wstring canon;
    if (!Canonicalize(p.wstring(), canon)) return false;
    std::wstring low = ToLower(canon);
    std::shared_lock<std::shared_mutex> g(UserExclusionsMu());
    for (const auto& e : UserExclusions()) {
        if (low == e) return true;
        // Excluding a folder protects its whole subtree.
        if (low.size() > e.size() && low.compare(0, e.size(), e) == 0 &&
            (e.back() == L'\\' || low[e.size()] == L'\\')) {
            return true;
        }
    }
    return false;
}

DWORD GuardRails::FileAttributesOf(const fs::path& p) {
    return GetFileAttributesW(LongPath(p).c_str());
}

GuardRails::Verdict GuardRails::Validate(const PlanItem& pi,
                                         const ScanItem& si,
                                         Context ctx) const {
    Verdict v;
    const auto& pol = Policy();

    // 1. Protected locations — canonicalized, context-dependent list.
    {
        std::wstring canon;
        bool denied = false;
        if (!Canonicalize(pi.path.wstring(), canon)) {
            denied = true;   // deny-by-default
        } else if (canon.size() >= 2 && canon[0] == L'\\' && canon[1] == L'\\') {
            denied = true;   // UNC
        } else {
            std::wstring low = ToLower(canon);
            if (ctx == Context::FileOp) {
                denied = MatchAny(low, pol.fileExact) ||
                         (MatchAnySub(low, pol.fileSubtree) &&
                          !MatchAnySub(low, pol.exemptSubtree));
            } else {
                denied = MatchAny(low, pol.migExact) ||
                         MatchAnySub(low, pol.migSubtree);
            }
        }
        if (denied) {
            v.allow = false;
            v.reason = L"受保护系统路径，拒绝操作";
            return v;
        }
    }

    // 1b. REVIEW P2 (08-F9): the user's own exclusion list.
    if (ctx == Context::FileOp && IsUserExcluded(pi.path)) {
        v.allow = false;
        v.reason = L"该项目在您的排除清单中（永不清理）";
        return v;
    }

    // 2. Strategy/risk constraints (file-op context): InfoOnly items are
    //    display-only and Advanced items are delegate-only (ADR-008).
    if (ctx == Context::FileOp) {
        if (si.riskLevel == RiskLevel::InfoOnly) {
            v.allow = false;
            v.reason = L"仅提示项，不可执行（请参考详情中的指引）";
            return v;
        }
        if (si.riskLevel == RiskLevel::Advanced &&
            si.strategy != CleanStrategy::Delegate) {
            v.allow = false;
            v.reason = L"该级别项目仅支持系统命令委派，不可直接删除";
            return v;
        }
    }

    // 3. Attributes: exists, not a reparse point, not a cloud placeholder.
    DWORD attr = FileAttributesOf(pi.path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        v.allow = false;
        v.reason = L"路径已不存在（可能已被处理）";
        return v;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        v.allow = false;
        v.reason = L"路径是 Junction/符号链接，拒绝操作";
        return v;
    }
    if (attr & (FILE_ATTRIBUTE_RECALL_ON_DATA_OPEN | FILE_ATTRIBUTE_OFFLINE)) {
        v.allow = false;
        v.reason = L"云文件占位符（OneDrive 等），拒绝操作";
        return v;
    }

    // 4. TOCTOU re-verify against the scan snapshot.
    //    - Files: size and mtime must match.
    //    - Directories: only mtime (direct children added/removed) — the
    //      directory entry's own size says nothing about the subtree.
    //    - Safe-level items skip the mtime check: temp/cache directories
    //      churn constantly and the misfire rate on exactly the most-cleaned
    //      targets was unacceptable (review 05-T-B5).
    unsigned long long sizeNow = 0, mtimeNow = 0;
    if (!GetFileTimes(pi.path, sizeNow, mtimeNow)) {
        v.allow = false;
        v.reason = L"无法读取文件属性";
        return v;
    }
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!isDir && pi.sizeAtScan != 0 && sizeNow != pi.sizeAtScan) {
        v.allow = false;
        v.reason = FormatW(L"扫描后大小已变化（%s → %s）",
                           FormatSize(pi.sizeAtScan).c_str(),
                           FormatSize(sizeNow).c_str());
        return v;
    }
    if (pi.lastWriteAtScan != 0 && mtimeNow != pi.lastWriteAtScan &&
        si.riskLevel != RiskLevel::Safe) {
        v.allow = false;
        v.reason = L"扫描后目录内容已被修改，请重新扫描";
        return v;
    }
    return v;
}

} // namespace minisys
