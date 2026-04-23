#include "core/AppScanner.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <vector>
#include <unordered_set>

namespace fs = std::filesystem;

namespace minisys {

namespace {

struct HiveSpec {
    HKEY        root;
    const wchar_t* sub;
    bool        wow64_64Key; // pass KEY_WOW64_64KEY for 64-bit view
};

const HiveSpec kHives[] = {
    { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",            true  },
    { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", true },
    { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",            false },
};

std::wstring RegStr(HKEY k, const wchar_t* name) {
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    std::wstring s(cb / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(s.data()), &cb) != ERROR_SUCCESS) return {};
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    if (type == REG_EXPAND_SZ) {
        wchar_t buf[MAX_PATH * 4];
        DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, _countof(buf));
        if (n > 0 && n < _countof(buf)) s.assign(buf);
    }
    return s;
}

DWORD RegDword(HKEY k, const wchar_t* name) {
    DWORD type = 0, cb = sizeof(DWORD), v = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &cb) != ERROR_SUCCESS) return 0;
    if (type != REG_DWORD) return 0;
    return v;
}

void EnumerateHive(const HiveSpec& spec, std::vector<AppInfo>& outApps) {
    HKEY h = nullptr;
    REGSAM sam = KEY_READ | (spec.wow64_64Key ? KEY_WOW64_64KEY : 0);
    if (RegOpenKeyExW(spec.root, spec.sub, 0, sam, &h) != ERROR_SUCCESS) return;
    DWORD i = 0;
    wchar_t name[256];
    while (true) {
        DWORD nameLen = _countof(name);
        LSTATUS st = RegEnumKeyExW(h, i++, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(h, name, 0, sam, &sub) != ERROR_SUCCESS) continue;
        AppInfo app;
        app.displayName     = RegStr(sub, L"DisplayName");
        app.publisher       = RegStr(sub, L"Publisher");
        app.installLocation = RegStr(sub, L"InstallLocation");
        app.uninstallString = RegStr(sub, L"UninstallString");
        DWORD est = RegDword(sub, L"EstimatedSize");
        app.sizeBytesEstimated = static_cast<unsigned long long>(est) * 1024ULL;
        DWORD systemComponent = RegDword(sub, L"SystemComponent");
        std::wstring releaseType = RegStr(sub, L"ReleaseType");
        std::wstring parentKey = RegStr(sub, L"ParentKeyName");
        RegCloseKey(sub);

        if (app.displayName.empty()) continue;
        if (systemComponent == 1) continue;             // hide system components
        if (!releaseType.empty() && releaseType != L"") {
            if (releaseType == L"Update" || releaseType == L"Hotfix" ||
                releaseType == L"Security Update") continue;
        }
        if (!parentKey.empty()) continue;               // child updates

        if (app.installLocation.empty()) continue;       // skip if no clear location
        // Trim trailing quotes/spaces.
        while (!app.installLocation.empty() &&
               (app.installLocation.back() == L'\\' || app.installLocation.back() == L' ')) {
            app.installLocation.pop_back();
        }
        if (!DirExists(app.installLocation)) continue;
        app.isOnSystemDrive = IsOnSystemDrive(app.installLocation);
        outApps.push_back(std::move(app));
    }
    RegCloseKey(h);
}

} // namespace

void AppScanner::Scan(std::vector<ScanItem>& out,
                      ProgressFn progress,
                      const std::atomic<bool>& cancel) {
    std::vector<AppInfo> apps;
    apps.reserve(256);
    for (auto& h : kHives) {
        if (cancel.load()) return;
        if (progress) progress(0, 0, h.sub);
        EnumerateHive(h, apps);
    }

    // De-duplicate by install location (case-insensitive)
    std::unordered_set<std::wstring> seen;
    std::vector<AppInfo> unique;
    unique.reserve(apps.size());
    for (auto& a : apps) {
        auto key = ToLower(a.installLocation);
        if (seen.insert(key).second) unique.push_back(std::move(a));
    }

    unsigned long long total = unique.size();
    unsigned long long done = 0;
    for (auto& a : unique) {
        if (cancel.load()) return;
        if (!a.isOnSystemDrive) { ++done; continue; }  // only show C-drive apps
        if (progress) progress(done++, total, a.displayName);
        a.sizeBytesActual = DirectorySize(a.installLocation);
        if (a.sizeBytesActual == 0) a.sizeBytesActual = a.sizeBytesEstimated;
        if (a.sizeBytesActual < 50ULL * 1024 * 1024) continue;  // skip tiny apps (<50MB)

        ScanItem it;
        it.category = L"App";
        it.title    = a.displayName + (a.publisher.empty() ? L"" : (L" — " + a.publisher));
        it.path     = a.installLocation;
        it.sizeBytes = a.sizeBytesActual;
        it.detail   = L"InstallLocation: " + a.installLocation;
        if (!a.uninstallString.empty()) {
            it.detail += L"\nUninstall: " + a.uninstallString;
        }
        it.recommended = false;
        out.push_back(std::move(it));
    }
}

} // namespace minisys
