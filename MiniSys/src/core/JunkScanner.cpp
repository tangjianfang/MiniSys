#include "core/JunkScanner.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_set>

namespace fs = std::filesystem;

namespace minisys {

namespace {

struct Rule {
    const wchar_t* category;
    const wchar_t* title;
    fs::path       path;          // resolved at runtime
    bool           recurseChildrenOnly = false; // if true, scan immediate sub-dirs
    bool           dangerous = false;
};

fs::path Env(const wchar_t* var) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(var, buf, _countof(buf));
    if (n == 0 || n >= _countof(buf)) return {};
    return buf;
}

void AddIfExists(std::vector<Rule>& rules, Rule r) {
    if (!r.path.empty() && (DirExists(r.path) || FileExists(r.path))) {
        rules.push_back(std::move(r));
    }
}

std::vector<Rule> BuildRules() {
    std::vector<Rule> rules;
    auto sysRoot = Env(L"SystemRoot");
    auto userTemp = Env(L"TEMP");
    auto localApp = Env(L"LOCALAPPDATA");
    auto roamingApp = Env(L"APPDATA");

    AddIfExists(rules, {L"System Temp", L"User Temp Folder",  userTemp, false, false});
    if (!sysRoot.empty()) {
        AddIfExists(rules, {L"System Temp", L"Windows Temp",  sysRoot / L"Temp", false, false});
        AddIfExists(rules, {L"Windows Update", L"SoftwareDistribution Download Cache",
                            sysRoot / L"SoftwareDistribution" / L"Download", false, false});
        AddIfExists(rules, {L"Windows Logs", L"CBS Logs",
                            sysRoot / L"Logs" / L"CBS", false, false});
        AddIfExists(rules, {L"Windows Logs", L"DISM Logs",
                            sysRoot / L"Logs" / L"DISM", false, false});
        AddIfExists(rules, {L"Windows Logs", L"Minidump",
                            sysRoot / L"Minidump", false, false});
    }

    // Browser caches
    if (!localApp.empty()) {
        AddIfExists(rules, {L"Browser Cache", L"Edge - Cache",
                            localApp / L"Microsoft" / L"Edge" / L"User Data" / L"Default" / L"Cache", false, false});
        AddIfExists(rules, {L"Browser Cache", L"Edge - Code Cache",
                            localApp / L"Microsoft" / L"Edge" / L"User Data" / L"Default" / L"Code Cache", false, false});
        AddIfExists(rules, {L"Browser Cache", L"Edge - GPUCache",
                            localApp / L"Microsoft" / L"Edge" / L"User Data" / L"Default" / L"GPUCache", false, false});

        AddIfExists(rules, {L"Browser Cache", L"Chrome - Cache",
                            localApp / L"Google" / L"Chrome" / L"User Data" / L"Default" / L"Cache", false, false});
        AddIfExists(rules, {L"Browser Cache", L"Chrome - Code Cache",
                            localApp / L"Google" / L"Chrome" / L"User Data" / L"Default" / L"Code Cache", false, false});
        AddIfExists(rules, {L"Browser Cache", L"Chrome - GPUCache",
                            localApp / L"Google" / L"Chrome" / L"User Data" / L"Default" / L"GPUCache", false, false});
    }
    if (!roamingApp.empty()) {
        // Firefox caches live under %LOCALAPPDATA%\Mozilla\Firefox\Profiles\<id>\cache2
        if (!localApp.empty()) {
            auto base = fs::path(localApp) / L"Mozilla" / L"Firefox" / L"Profiles";
            std::error_code ec;
            if (DirExists(base)) {
                for (auto& e : fs::directory_iterator(base, ec)) {
                    if (e.is_directory()) {
                        auto cache = e.path() / L"cache2";
                        if (DirExists(cache)) {
                            rules.push_back({L"Browser Cache",
                                (L"Firefox - " + e.path().filename().wstring()).c_str(),
                                cache, false, false});
                        }
                    }
                }
            }
        }
    }
    return rules;
}

void AddInfoOnlyItems(std::vector<ScanItem>& out) {
    auto sysDrive = SystemDriveRoot();
    fs::path hib = fs::path(sysDrive) / L"hiberfil.sys";
    fs::path pg  = fs::path(sysDrive) / L"pagefile.sys";
    fs::path sw  = fs::path(sysDrive) / L"swapfile.sys";
    for (auto& p : { hib, pg, sw }) {
        if (FileExists(p)) {
            std::error_code ec;
            auto sz = fs::file_size(p, ec);
            if (ec) sz = 0;
            ScanItem it;
            it.category = L"System Reserved (Info only)";
            it.title    = p.filename().wstring();
            it.path     = p;
            it.sizeBytes = sz;
            it.detail = L"Cannot delete directly. Use 'powercfg /h off' (hibernate) or System Properties to relocate page file.";
            it.recommended = false;
            it.dangerous = true;
            out.push_back(std::move(it));
        }
    }
}

} // namespace

void JunkScanner::Scan(std::vector<ScanItem>& out,
                       ProgressFn progress,
                       const std::atomic<bool>& cancel) {
    auto rules = BuildRules();
    std::atomic<unsigned long long> doneCount{0};
    unsigned long long total = static_cast<unsigned long long>(rules.size()) + 1; // +1 for recycle bin

    // Pre-compute sizes in parallel (each DirectorySize call is independent).
    std::vector<unsigned long long> sizes(rules.size(), 0);
    {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int n = static_cast<int>(hw);
        if (n > 8) n = 8;
        if (n < 2) n = 2;
        if ((int)rules.size() < n) n = (int)rules.size();
        if (n < 1) n = 1;
        std::atomic<size_t> next{0};
        std::vector<std::thread> ts;
        ts.reserve(n);
        std::mutex pmu;
        for (int t = 0; t < n; ++t) {
            ts.emplace_back([&] {
                for (;;) {
                    if (cancel.load()) return;
                    size_t i = next.fetch_add(1);
                    if (i >= rules.size()) return;
                    const auto& r = rules[i];
                    {
                        std::lock_guard<std::mutex> g(pmu);
                        if (progress) progress(doneCount.load(), total, r.title);
                    }
                    sizes[i] = DirExists(r.path) ? DirectorySize(r.path)
                              : (FileExists(r.path) ? fs::file_size(r.path) : 0);
                    doneCount.fetch_add(1);
                }
            });
        }
        for (auto& t : ts) t.join();
    }
    if (cancel.load()) return;

    for (size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        if (sizes[i] == 0) continue;
        ScanItem it;
        it.category    = r.category;
        it.title       = r.title;
        it.path        = r.path;
        it.sizeBytes   = sizes[i];
        it.detail      = r.path.wstring();
        it.recommended = true;
        it.dangerous   = r.dangerous;
        out.push_back(std::move(it));
    }

    // Recycle Bin (single shell call, no need to parallelize).
    if (!cancel.load()) {
        if (progress) progress(doneCount.load(), total, L"Recycle Bin");
        RecycleBinInfo info;
        if (QueryRecycleBin(info) && info.sizeBytes > 0) {
            ScanItem it;
            it.category    = L"Recycle Bin";
            it.title       = L"Empty Recycle Bin (all volumes)";
            it.path        = L"$RECYCLE.BIN";
            it.sizeBytes   = info.sizeBytes;
            it.detail      = FormatW(L"%llu items — irreversible", info.itemCount);
            it.recommended = false;
            it.dangerous   = true;
            out.push_back(std::move(it));
        }
        doneCount.fetch_add(1);
    }

    AddInfoOnlyItems(out);
    if (progress) progress(doneCount.load(), total, L"Done");
}

} // namespace minisys
