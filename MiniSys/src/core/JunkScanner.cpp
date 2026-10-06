#include "core/JunkScanner.h"

#include "core/DevBuildCache.h"
#include "core/JunkRules.h"
#include "core/VolumeIndex.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace minisys {

namespace {

struct Candidate {
    const JunkRule* rule = nullptr;
    fs::path path;
    unsigned long long sizeBytes = 0;   // known for files, computed for dirs
    uint64_t lastWriteFiletime = 0;
    bool isFile = false;
};

// Raw FILETIME now (100-ns ticks).
uint64_t NowFiletime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool QueryCandidate(const fs::path& p, Candidate& c) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    c.isFile = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    c.sizeBytes = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
                  static_cast<unsigned long long>(fad.nFileSizeLow);
    c.lastWriteFiletime =
        (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
    return true;
}

bool PassesAgeFilter(const JunkRule& rule, const Candidate& c) {
    if (rule.minAgeDays <= 0 || c.lastWriteFiletime == 0) return true;
    uint64_t now = NowFiletime();
    if (c.lastWriteFiletime > now) return true;   // future mtime: keep
    uint64_t age100ns = now - c.lastWriteFiletime;
    uint64_t ageDays = age100ns / (24ULL * 3600ULL * 10000000ULL);
    return ageDays >= static_cast<uint64_t>(rule.minAgeDays);
}

std::vector<std::wstring> SplitNames(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t ch : s) {
        if (ch == L';' || ch == L',') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else if (ch != L' ') {
            cur += ch;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Expand one rule into concrete candidate paths.
void ExpandRule(const JunkRule& rule, const fs::path& base,
                std::vector<Candidate>& out) {
    switch (rule.mode) {
        case RuleMode::Subtree: {
            Candidate c{&rule, base};
            if (QueryCandidate(base, c) && PassesAgeFilter(rule, c)) {
                out.push_back(std::move(c));
            }
            break;
        }
        case RuleMode::Children: {
            std::wstring search = base.wstring();
            if (!search.empty() && search.back() != L'\\') search += L'\\';
            search += L'*';
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileExW(LongPath(search).c_str(),
                                        FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (!rule.childPattern.empty() &&
                    !JunkRules::MatchWildcard(rule.childPattern, n)) {
                    continue;
                }
                fs::path full = base / n;
                Candidate c{&rule, full};
                if (QueryCandidate(full, c) && PassesAgeFilter(rule, c)) {
                    out.push_back(std::move(c));
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            break;
        }
        case RuleMode::Profiles: {
            // For each direct child dir P of base, for each configured name N,
            // P\N becomes a candidate (browser multi-profile support).
            std::wstring search = base.wstring();
            if (!search.empty() && search.back() != L'\\') search += L'\\';
            search += L'*';
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileExW(LongPath(search).c_str(),
                                        FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) return;
            std::vector<fs::path> profileDirs;
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                profileDirs.push_back(base / n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);

            for (const auto& name : SplitNames(rule.profileNames)) {
                for (const auto& p : profileDirs) {
                    fs::path full = p / name;
                    Candidate c{&rule, full};
                    if (QueryCandidate(full, c) && PassesAgeFilter(rule, c)) {
                        out.push_back(std::move(c));
                    }
                }
            }
            break;
        }
    }
}

// Indexed rule expansion (M2). Returns true when the index authoritatively
// handled `base` (including "known absent"); false → caller falls back to the
// filesystem walk. Directory candidates need no disk I/O at all — file
// candidates still need one attribute query for their size (ADR-004).
bool ExpandRuleIndexed(const JunkRule& rule, const fs::path& base,
                       VolumeIndex& idx, std::vector<Candidate>& out) {
    auto baseStr = base.wstring();

    auto addEntry = [&](const VolumeIndex::FileEntry& e) {
        Candidate c{&rule, e.path};
        if (e.isDirectory) {
            c.isFile = false;
            c.lastWriteFiletime = e.lastWrite;
        } else {
            // Size is not in USN records — one attribute query per file item.
            if (!QueryCandidate(e.path, c)) return;
        }
        if (PassesAgeFilter(rule, c)) out.push_back(std::move(c));
    };

    switch (rule.mode) {
        case RuleMode::Subtree: {
            VolumeIndex::FileEntry e;
            if (!idx.TryGetEntry(baseStr, e)) return true;   // known absent
            addEntry(e);
            return true;
        }
        case RuleMode::Children: {
            bool known = idx.CollectChildren(baseStr, [&](const VolumeIndex::FileEntry& e) {
                if (!rule.childPattern.empty() &&
                    !JunkRules::MatchWildcard(rule.childPattern,
                                              fs::path(e.path).filename().wstring())) {
                    return;
                }
                addEntry(e);
            });
            return known;   // false → base unknown to the index
        }
        case RuleMode::Profiles: {
            std::vector<VolumeIndex::FileEntry> profileDirs;
            bool known = idx.CollectChildren(baseStr,
                [&](const VolumeIndex::FileEntry& e) {
                    if (e.isDirectory) profileDirs.push_back(e);
                });
            if (!known) return false;
            for (const auto& name : SplitNames(rule.profileNames)) {
                for (const auto& p : profileDirs) {
                    VolumeIndex::FileEntry e;
                    if (idx.TryGetEntry(p.path + L"\\" + name, e)) {
                        addEntry(e);
                    }
                }
            }
            return true;
        }
    }
    return false;
}

} // namespace

void JunkScanner::Scan(std::vector<ScanItem>& out,
                       ProgressFn progress,
                       const std::atomic<bool>& cancel) {
    auto rules = JunkRules::LoadValidated().rules;
    if (progress) progress(0, 0, L"加载清理规则");

    // ---- 0. Shared volume index (M2): build once per session, reuse ----
    VolumeIndex* idx = nullptr;
    {
        auto sd = SystemDriveRoot();
        if (sd.size() >= 2 && sd[1] == L':') {
            auto& vi = VolumeIndex::Instance();
            if (vi.EnsureBuilt(sd[0],
                    [&](const std::wstring& msg) {
                        if (progress) progress(0, 0, msg);
                    },
                    cancel)) {
                idx = &vi;
            }
        }
    }

    // ---- 1. Expand rules into candidates (existence + age filter) ----
    std::vector<Candidate> candidates;
    candidates.reserve(64);
    for (const auto& rule : rules) {
        if (cancel.load()) return;
        auto base = fs::path(JunkRules::ExpandEnv(rule.path));
        if (base.empty()) continue;

        // Index path first (rules on the indexed volume); fall back to the
        // filesystem walk for other drives / unknown paths / no index.
        bool handled = false;
        if (idx && !base.empty()) {
            auto bs = base.wstring();
            if (bs.size() >= 2 && bs[1] == L':' &&
                ::towupper(bs[0]) == idx->Drive()) {
                handled = ExpandRuleIndexed(rule, base, *idx, candidates);
            }
        }
        if (!handled) {
            ExpandRule(rule, base, candidates);
        }
    }
    if (cancel.load()) return;

    // ---- 2. Parallel size computation for directory candidates ----
    // REVIEW P1-3 (03-B11): sizes for Delegate/InfoOnly candidates (WinSxS
    // alone is ~300k entries on this machine) are display-only and were the
    // dominant scan cost — skipped entirely now; parallel subtree sizing
    // covers the rest.
    std::vector<Candidate*> dirs;
    for (auto& c : candidates) {
        if (!c.isFile && c.rule->strategy == CleanStrategy::Quarantine) {
            dirs.push_back(&c);
        }
    }
    if (!dirs.empty()) {
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int n = static_cast<int>(hw);
        if (n > 8) n = 8;
        if (n < 2) n = 2;
        if (static_cast<int>(dirs.size()) < n) n = static_cast<int>(dirs.size());
        std::vector<std::thread> ts;
        std::mutex pmu;
        ts.reserve(n);
        for (int t = 0; t < n; ++t) {
            ts.emplace_back([&] {
                for (;;) {
                    if (cancel.load()) return;
                    size_t i = next.fetch_add(1);
                    if (i >= dirs.size()) return;
                    dirs[i]->sizeBytes =
                        DirectorySizeParallel(dirs[i]->path, /*numThreads=*/2);
                    size_t d = done.fetch_add(1) + 1;
                    {
                        std::lock_guard<std::mutex> g(pmu);
                        if (progress) {
                            progress(d, dirs.size(), dirs[i]->path.filename().wstring());
                        }
                    }
                }
            });
        }
        for (auto& t : ts) t.join();
    }
    if (cancel.load()) return;

    // ---- 3. Emit items ----
    for (auto& c : candidates) {
        const JunkRule& rule = *c.rule;
        // REVIEW P1-3: Delegate/InfoOnly candidates skipped the size pass —
        // emit them without a size instead of dropping them.
        if (rule.strategy != CleanStrategy::Quarantine && c.sizeBytes == 0) {
            ScanItem it;
            it.category = rule.category;
            it.title = (rule.mode == RuleMode::Subtree)
                         ? rule.title
                         : rule.title + L" — " + c.path.filename().wstring();
            it.path = c.path;
            it.sizeBytes = 0;
            it.lastWriteFiletime = c.lastWriteFiletime;
            it.createTime = c.lastWriteFiletime;
            it.ruleId = rule.id;
            it.riskLevel = rule.riskLevel;
            it.recommended = rule.recommended;
            it.dangerous = (rule.riskLevel >= RiskLevel::Advanced) ||
                           (rule.strategy != CleanStrategy::Quarantine);
            it.detail = c.path.wstring();
            if (!rule.detailHint.empty()) it.detail += L"\n" + rule.detailHint;
            it.strategy = rule.strategy;
            it.command = rule.command;
            out.push_back(std::move(it));
            continue;
        }
        if (c.sizeBytes == 0) continue;   // nothing to gain
        ScanItem it;
        it.category = rule.category;
        switch (rule.mode) {
            case RuleMode::Subtree:
                it.title = rule.title;
                break;
            case RuleMode::Children:
                it.title = rule.title + L" — " + c.path.filename().wstring();
                break;
            case RuleMode::Profiles:
                // Distinguish "<profile>\<cache>" pairs.
                it.title = rule.title + L" — " +
                           c.path.parent_path().filename().wstring() + L"\\" +
                           c.path.filename().wstring();
                break;
        }
        it.path        = c.path;
        it.sizeBytes   = c.sizeBytes;
        it.lastWriteFiletime = c.lastWriteFiletime;
        it.createTime  = c.lastWriteFiletime;   // REVIEW P0-6 (07-X14): time
                                                // sort was a no-op on this tab
        it.ruleId      = rule.id;
        it.riskLevel   = rule.riskLevel;
        it.recommended = rule.recommended;
        it.dangerous   = (rule.riskLevel >= RiskLevel::Advanced) ||
                         (rule.strategy != CleanStrategy::Quarantine);
        it.detail      = c.path.wstring();
        if (!rule.detailHint.empty()) it.detail += L"\n" + rule.detailHint;
        if (rule.minAgeDays > 0) {
            it.detail += FormatW(L"\n(仅列出 %d 天未使用的项目)", rule.minAgeDays);
        }
        it.strategy    = rule.strategy;
        it.command     = rule.command;
        out.push_back(std::move(it));
    }

    // ---- 4. Dev build caches (v2.5): VS/C++/CMake artifacts next to a
    // project marker — quarantined like everything else, and dirs touched
    // in the last 24 h are skipped so active builds stay untouched. ----
    if (!cancel.load()) {
        DevBuildCache::Scan(out, progress, cancel);
    }

    // ---- 5. Recycle bin (special item; executed via EmptyRecycleOp) ----
    if (!cancel.load()) {
        if (progress) progress(0, 0, L"Recycle Bin");
        RecycleBinInfo info;
        if (QueryRecycleBin(info) && info.sizeBytes > 0) {
            ScanItem it;
            it.category    = L"Recycle Bin";
            it.title       = L"清空回收站（所有磁盘）";
            it.path        = L"$RECYCLE.BIN";
            it.sizeBytes   = info.sizeBytes;
            it.detail      = FormatW(L"%llu 项 — 不可逆", info.itemCount);
            it.recommended = false;
            it.dangerous   = true;
            it.riskLevel   = RiskLevel::Advanced;
            out.push_back(std::move(it));
        }
    }
    if (progress) progress(0, 0, L"Done");
}

} // namespace minisys
