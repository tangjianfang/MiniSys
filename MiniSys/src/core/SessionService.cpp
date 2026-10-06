#include "core/SessionService.h"

#include "core/DeleteOp.h"
#include "core/DelegateOp.h"
#include "core/GuardRails.h"
#include "core/JunkRules.h"
#include "core/MoveJunctionOp.h"
#include "core/OperationLog.h"
#include "core/QuarantineOp.h"
#include "core/VolumeIndex.h"
#include "platform/SystemRestore.h"
#include "res/resource.h"
#include "util/Json.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <objbase.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace fs = std::filesystem;

namespace minisys {

namespace {

// v2.8: per-task-kind completion message (04-5 contract — every task kind
// has exactly one DONE message, exceptions included).
UINT DoneMessageFor(SessionService::TaskKind kind) {
    switch (kind) {
        case SessionService::TaskKind::Scanning:   return WM_APP_SCAN_DONE;
        case SessionService::TaskKind::Searching:  return WM_APP_SEARCH_DONE;
        case SessionService::TaskKind::Verifying:  return WM_APP_VERIFY_DONE;
        case SessionService::TaskKind::Previewing: return WM_APP_PREVIEW_DONE;
        case SessionService::TaskKind::Executing:
        case SessionService::TaskKind::None:
        default:                                   return WM_APP_OP_DONE;
    }
}

} // namespace

SessionService& SessionService::Instance() {
    static SessionService inst;
    return inst;
}

SessionService::SessionService() {
    results_.resize(static_cast<size_t>(TabId::Count));
}

SessionService::~SessionService() {
    Shutdown();
}

void SessionService::SetProgress(const std::wstring& text) {
    std::lock_guard<std::mutex> g(progressMu_);
    progressText_ = text;
}

void SessionService::Post(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!hwnd) return;
    // review-04 R-4 (v2.10): completion messages carry the task generation
    // in wParam so the UI can drop stale ones (posted by a task that
    // finished after a NEWER task already started). PROGRESS messages keep
    // their payload in wp/lp — staleness there is cosmetic and self-heals
    // on the next tick.
    if (wp == 0 &&
        (msg == WM_APP_SCAN_DONE || msg == WM_APP_OP_DONE ||
         msg == WM_APP_SEARCH_DONE || msg == WM_APP_VERIFY_DONE ||
         msg == WM_APP_PREVIEW_DONE || msg == WM_APP_TASK_STARTED)) {
        wp = static_cast<WPARAM>(taskGen_.load());
    }
    PostMessageW(hwnd, msg, wp, lp);
}

std::wstring SessionService::ProgressText() const {
    std::lock_guard<std::mutex> g(progressMu_);
    return progressText_;
}

const SessionService::ExecuteReport& SessionService::LastReport() const {
    std::lock_guard<std::mutex> g(reportMu_);
    return lastReport_;
}

std::vector<ScanItem> SessionService::Results(TabId tab) const {
    // REVIEW P1-1: locked copy — the UI must never hold a live reference
    // into the worker-owned storage.
    std::lock_guard<std::mutex> g(resultsMu_);
    return results_[static_cast<size_t>(tab)];
}

void SessionService::StoreResults(TabId tab, std::vector<ScanItem> items) {
    std::lock_guard<std::mutex> g(resultsMu_);
    results_[static_cast<size_t>(tab)] = std::move(items);
}

// ---- v2.5: persisted scan results -------------------------------------------

namespace {

// %LOCALAPPDATA%\MiniSys\cache\results-<tab>.json — one file per scan tab so
// each tab's staleness is judged (and refreshed) separately. Capped at
// kMaxCachedItems rows to bound the file size.
constexpr size_t kMaxCachedItems = 5000;

// Path override for unit tests (empty = default location).
std::filesystem::path g_cacheDirOverride;

std::filesystem::path ResultsCachePath(TabId tab) {
    auto base = g_cacheDirOverride.empty()
        ? std::filesystem::path(AppDataDir())
        : g_cacheDirOverride;
    return base / L"cache" /
           (L"results-" + std::to_wstring(static_cast<int>(tab)) + L".json");
}

Json ScanItemToJson(const ScanItem& it) {
    Json j = Json::Object();
    j.Set(L"category", Json(it.category));
    j.Set(L"title", Json(it.title));
    j.Set(L"path", Json(it.path.wstring()));
    j.Set(L"sizeBytes", Json(static_cast<double>(it.sizeBytes)));
    j.Set(L"createTime", Json(static_cast<double>(it.createTime)));
    j.Set(L"detail", Json(it.detail));
    j.Set(L"recommended", Json(it.recommended));
    j.Set(L"dangerous", Json(it.dangerous));
    j.Set(L"groupKey", Json(it.groupKey));
    j.Set(L"ruleId", Json(it.ruleId));
    j.Set(L"lastWriteFiletime", Json(static_cast<double>(it.lastWriteFiletime)));
    j.Set(L"riskLevel", Json(static_cast<double>(
        static_cast<int>(it.riskLevel))));
    j.Set(L"strategy", Json(static_cast<double>(
        static_cast<int>(it.strategy))));
    j.Set(L"command", Json(it.command));
    return j;
}

bool JsonToScanItem(const Json& j, ScanItem& it) {
    if (!j.IsObject()) return false;
    it.category = j.Get(L"category").AsString();
    it.title    = j.Get(L"title").AsString();
    it.path     = j.Get(L"path").AsString();
    it.sizeBytes   = static_cast<unsigned long long>(
        j.Get(L"sizeBytes").AsNumber());
    it.createTime  = static_cast<uint64_t>(j.Get(L"createTime").AsNumber());
    it.detail      = j.Get(L"detail").AsString();
    it.recommended = j.Get(L"recommended").AsBool(true);
    it.dangerous   = j.Get(L"dangerous").AsBool(false);
    it.groupKey    = j.Get(L"groupKey").AsString();
    it.ruleId      = j.Get(L"ruleId").AsString();
    it.lastWriteFiletime = static_cast<uint64_t>(
        j.Get(L"lastWriteFiletime").AsNumber());
    int risk = static_cast<int>(j.Get(L"riskLevel").AsNumber(1));
    if (risk < 0 || risk > 3) return false;
    it.riskLevel = static_cast<RiskLevel>(risk);
    int strat = static_cast<int>(j.Get(L"strategy").AsNumber(0));
    if (strat < 0 || strat > 2) return false;
    it.strategy = static_cast<CleanStrategy>(strat);
    it.command  = j.Get(L"command").AsString();
    return !it.path.empty();
}

} // namespace

void SessionService::SaveResultsCache(TabId tab, bool allowEmpty) {
    if (tab == TabId::History) return;   // OperationLog is the store
    std::vector<ScanItem> items;
    uint64_t scanAt = 0;
    {
        std::lock_guard<std::mutex> g(resultsMu_);
        items  = results_[static_cast<size_t>(tab)];
        scanAt = scanAt_[static_cast<size_t>(tab)];
    }
    // v2.9: an explicit empty write invalidates a stale cache (cleared
    // search); otherwise empty lists are simply not persisted.
    if ((items.empty() && !allowEmpty) || (scanAt == 0 && !items.empty())) return;
    if (items.size() > kMaxCachedItems) items.resize(kMaxCachedItems);

    try {
        auto path = ResultsCachePath(tab);
        std::filesystem::create_directories(path.parent_path());
        Json root = Json::Object();
        root.Set(L"tab", Json(static_cast<double>(static_cast<int>(tab))));
        root.Set(L"scanAt", Json(static_cast<double>(scanAt)));
        Json arr = Json::Array();
        for (const auto& it : items) arr.Push(ScanItemToJson(it));
        root.Set(L"items", arr);

        auto tmp = path;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << WideToUtf8(root.Dump());
        }
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    } catch (const std::exception& ex) {
        MS_LOG_WARN(L"Results cache save failed (tab %d): %hs",
                    static_cast<int>(tab), ex.what());
    }
}

void SessionService::NoteScanTime(TabId tab) {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    std::lock_guard<std::mutex> g(resultsMu_);
    scanAt_[static_cast<size_t>(tab)] =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void SessionService::SetCacheDirForTesting(const std::filesystem::path& p) {
    g_cacheDirOverride = p;
}

uint64_t SessionService::LastScanAt(TabId tab) const {
    std::lock_guard<std::mutex> g(resultsMu_);
    return scanAt_[static_cast<size_t>(tab)];
}

bool SessionService::ResultsFromCache(TabId tab) const {
    std::lock_guard<std::mutex> g(resultsMu_);
    return fromCache_[static_cast<size_t>(tab)];
}

bool SessionService::TryLoadCachedResults(TabId tab) {
    if (tab == TabId::History) return false;   // OperationLog is the store
    {
        std::lock_guard<std::mutex> g(resultsMu_);
        if (!results_[static_cast<size_t>(tab)].empty()) return false;  // fresh data wins
    }
    try {
        auto path = ResultsCachePath(tab);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::string utf8((std::istreambuf_iterator<char>(f)), {});
        Json j;
        std::wstring err;
        if (!Json::Parse(Utf8ToWide(utf8), j, err) || !j.IsObject()) {
            MS_LOG_WARN(L"Results cache parse failed (tab %d): %s",
                        static_cast<int>(tab), err.c_str());
            return false;
        }
        std::vector<ScanItem> items;
        const Json& arr = j.Get(L"items");
        if (arr.IsArray()) {
            items.reserve(arr.Size());
            for (size_t i = 0; i < arr.Size(); ++i) {
                ScanItem it;
                if (JsonToScanItem(arr.At(i), it)) items.push_back(std::move(it));
            }
        }
        if (items.empty()) return false;
        uint64_t scanAt = static_cast<uint64_t>(j.Get(L"scanAt").AsNumber());

        // review-05 (T-B1, CONFIRMED): the cache lives in user-writable
        // LOCALAPPDATA and this process runs elevated — sanitize everything
        // executable on load. Delegate items whose command does not pass
        // the built-in whitelist are dropped outright (the execution-side
        // re-check in RunPlan is the second layer).
        {
            std::vector<ScanItem> safe;
            safe.reserve(items.size());
            for (auto& it : items) {
                if (it.strategy == CleanStrategy::Delegate) {
                    std::wstring denyReason;
                    if (!JunkRules::DelegateCommandAllowed(it.command, denyReason)) {
                        MS_LOG_WARN(L"Cached Delegate item dropped (tab %d): %s — %s",
                                    static_cast<int>(tab), it.title.c_str(),
                                    denyReason.c_str());
                        continue;
                    }
                }
                safe.push_back(std::move(it));
            }
            items.swap(safe);
            if (items.empty()) return false;
        }

        std::lock_guard<std::mutex> g(resultsMu_);
        if (!results_[static_cast<size_t>(tab)].empty()) return false;
        results_[static_cast<size_t>(tab)]    = std::move(items);
        scanAt_[static_cast<size_t>(tab)]     = scanAt;
        fromCache_[static_cast<size_t>(tab)]  = true;
        return true;
    } catch (const std::exception& ex) {
        MS_LOG_WARN(L"Results cache load failed (tab %d): %hs",
                    static_cast<int>(tab), ex.what());
        return false;
    }
}

bool SessionService::StartTask(TaskKind kind, std::function<void()> body) {
    int expected = static_cast<int>(TaskKind::None);
    if (!taskKind_.compare_exchange_strong(expected, static_cast<int>(kind))) {
        return false;   // busy
    }
    taskGen_.fetch_add(1);   // R-4: this task's completion messages carry this gen
    if (worker_.joinable()) worker_.join();
    cancelScan_.store(false);
    worker_ = std::thread([this, kind, body = std::move(body)]() mutable {        // Shell operations (IFileOperation) need COM on this thread.
        bool com = SUCCEEDED(CoInitializeEx(nullptr,
                          COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        bool threw = false;
        try {
            body();
        } catch (const std::exception& ex) {
            MS_LOG_ERROR(L"Task threw: %hs", ex.what());
            threw = true;
        } catch (...) {
            MS_LOG_ERROR(L"Task threw unknown exception");
            threw = true;
        }
        // REVIEW-UI P1 (04-5): an exception used to skip the task's own
        // DONE post, leaving the UI's task matrix locked forever. Always
        // post the completion message for the task kind.
        if (threw && hwnd_) {
            SetProgress(L"⚠ 任务异常中断（详见日志）");
            Post(hwnd_, DoneMessageFor(kind));   // R-4: gen-stamped
        }
        if (com) CoUninitialize();
        taskKind_.store(static_cast<int>(TaskKind::None));
    });
    return true;
}

bool SessionService::StartScan(TabId tab, std::unique_ptr<Scanner> scanner) {
    SetProgress(L"扫描中…");
    HWND hwnd = hwnd_;
    // std::function requires copyable targets — hold the move-only scanner
    // via shared_ptr.
    auto holder = std::shared_ptr<Scanner>(std::move(scanner));
    return StartTask(TaskKind::Scanning, [this, tab, holder, hwnd]() {
        RunScan(tab, holder, hwnd);
    });
}

bool SessionService::BuildIndexAsync(bool forceRebuild) {
    if (IsBusy()) return false;
    SetProgress(forceRebuild ? L"重建文件索引（全量，所有固定磁盘）…"
                             : L"构建文件索引（所有固定磁盘）…");
    HWND hwnd = hwnd_;
    bool ok = StartTask(TaskKind::Scanning, [this, forceRebuild, hwnd]() {
        // v2.11 (磁盘瘦身助手): index EVERY fixed drive, not just the
        // system volume — search covers all disks now.
        int built = 0, failed = 0;
        for (const auto& root : EnumerateDrives()) {
            if (cancelScan_.load()) break;
            wchar_t d = ::towupper(root.empty() ? L'C' : root[0]);
            auto& vi = VolumeIndex::For(d);
            if (forceRebuild) vi.Invalidate();   // 重建 must actually rebuild
            vi.EnsureBuilt(d,
                [this, hwnd, d](const std::wstring& msg) {
                    SetProgress(FormatW(L"索引 %c: %s", d, msg.c_str()));
                    Post(hwnd, WM_APP_SCAN_PROGRESS);
                },
                cancelScan_);
            if (vi.IsValid()) ++built;
            else ++failed;
        }
        size_t total = VolumeIndex::TotalEntries();
        bool cancelled = cancelScan_.load();
        // REVIEW-UI P1 (L-5): distinguish cancelled / partial / ok.
        if (cancelled && built == 0) {
            SetProgress(L"ℹ 已取消索引构建，下次进入文件搜索将继续");
        } else if (total > 0) {
            SetProgress(FormatW(L"索引就绪: %s 项（%d 个磁盘%s）",
                                FormatCountSimple(total).c_str(), built,
                                failed ? L"，部分磁盘不支持已回退遍历" : L""));
        } else {
            SetProgress(L"索引不可用（磁盘不支持或被策略限制），垃圾扫描仍可用（较慢）");
        }
        Post(hwnd, WM_APP_SCAN_DONE);
    });
    return ok;
}

// REVIEW-UI P1: thousands separator for index counts ("1,234,567").
std::wstring SessionService::FormatCountSimple(size_t n) {
    std::wstring raw = std::to_wstring(n);
    std::wstring out;
    for (size_t i = 0; i < raw.size(); ++i) {
        size_t fromEnd = raw.size() - i;
        out += raw[i];
        if (fromEnd > 1 && (fromEnd - 1) % 3 == 0) out += L',';
    }
    return out;
}

// REVIEW-UI P1 (04-1 + U-1): the whole search pipeline runs on the worker.
// v2.11: see header. Runs as TaskKind::Verifying — read-only, so the UI
// matrix stays usable while it works (same reasoning as verify/preview).
bool SessionService::IdleMaintenanceAsync() {
    if (IsBusy()) return false;
    SetProgress(L"空闲维护：增量刷新索引…");
    HWND hwnd = hwnd_;
    return StartTask(TaskKind::Verifying, [this, hwnd]() {
        // 1) journal deltas for every built volume (cheap by design —
        // EnsureBuilt revalidates serial + reads only new USN records).
        size_t refreshed = 0;
        for (const auto& root : EnumerateDrives()) {
            if (cancelScan_.load()) break;
            wchar_t d = ::towupper(root.empty() ? L'C' : root[0]);
            auto& vi = VolumeIndex::For(d);
            if (!vi.IsValid()) continue;
            vi.EnsureBuilt(d, nullptr, cancelScan_);
            ++refreshed;
        }

        // 2) ghost-row prune for every cached tab, existence checked
        // THROUGH the index (no per-path disk hits). Posts one
        // WM_APP_VERIFY_DONE per pruned tab so the UI applies them in order.
        size_t prunedTabs = 0, prunedRows = 0;
        for (int t = 0; t < static_cast<int>(TabId::Count); ++t) {
            auto tab = static_cast<TabId>(t);
            if (tab == TabId::History || tab == TabId::Search) continue;
            auto items = Results(tab);
            if (items.empty()) continue;
            std::vector<std::wstring> dead;
            for (const auto& it : items) {
                if (it.path.empty() || it.path == L"$RECYCLE.BIN") continue;
                auto p = it.path.wstring();
                if (p.size() < 2 || p[1] != L':') continue;
                auto& vi = VolumeIndex::For(::towupper(p[0]));
                if (!vi.IsValid()) continue;   // volume unknown → leave it
                VolumeIndex::FileEntry fe;
                if (!vi.TryGetEntry(p, fe)) dead.push_back(ToLower(p));
            }
            if (dead.empty()) continue;
            // Prune the service-side list (the presenter's VERIFY_DONE
            // application covers the visible copy) AND persist it — v2.11
            // fix: ghosts used to resurface from the disk cache after a
            // restart until the next full scan overwrote the file.
            std::vector<ScanItem> kept;
            kept.reserve(items.size());
            size_t removed = 0;
            for (auto& it : items) {
                if (it.path == L"$RECYCLE.BIN" ||
                    std::find(dead.begin(), dead.end(),
                              ToLower(it.path.wstring())) == dead.end()) {
                    kept.push_back(std::move(it));
                } else {
                    ++removed;
                }
            }
            StoreResults(tab, kept);   // copy in (SaveResultsCache reads results_)
            SaveResultsCache(tab);     // persist the pruned list
            {
                std::lock_guard<std::mutex> g(verifyMu_);
                lastDead_ = std::move(dead);
                lastVerifyTab_ = tab;
            }
            ++prunedTabs;
            prunedRows += removed;
            Post(hwnd, WM_APP_VERIFY_DONE);
        }

        SetProgress(prunedTabs
            ? FormatW(L"空闲维护完成: %zu 个磁盘已增量刷新，清理 %zu 个失效项",
                      refreshed, prunedRows)
            : FormatW(L"空闲维护完成: %zu 个磁盘已增量刷新", refreshed));
        Post(hwnd, WM_APP_SCAN_PROGRESS);
    });
}

bool SessionService::SearchAsync(const std::wstring& query, bool matchPath) {
    if (IsBusy()) return false;
    SetProgress(query.empty() ? L"搜索" : L"搜索: " + query);
    HWND hwnd = hwnd_;
    return StartTask(TaskKind::Searching, [this, query, matchPath, hwnd]() {
        std::vector<ScanItem> items;
        if (!query.empty()) {
            // v2.5: Everything-style in-query filters ("folder:", "file:",
            // "ext:cpp;h") are matched inside the index scan.
            std::wstring freeQuery;
            auto filter = VolumeIndex::ParseFilterTerms(query, freeQuery);
            constexpr size_t kMaxResults = 1000;
            constexpr size_t kSizeFetchRows = 150;
            items.reserve(256);
            // v2.11 (磁盘瘦身助手): merged search over EVERY indexed
            // volume, one shared result budget.
            auto sink = [&](const VolumeIndex::SearchHit& hit) {
                ScanItem it;
                std::filesystem::path p(hit.path);
                it.category    = hit.isDirectory ? L"文件夹" : L"文件";
                it.title       = hit.name;
                it.path        = p;
                it.sizeBytes   = 0;      // lazy pass below
                it.lastWriteFiletime = hit.lastWrite;
                it.createTime  = hit.lastWrite;
                it.detail      = p.parent_path().wstring();
                it.recommended = false;
                it.riskLevel   = RiskLevel::Cautious;   // unclassified
                items.push_back(std::move(it));
                return !cancelScan_.load();   // stop on new keystroke
            };
            for (VolumeIndex* vi : VolumeIndex::ValidVolumes()) {
                if (items.size() >= kMaxResults) break;
                vi->Search(freeQuery, matchPath, kMaxResults - items.size(),
                           sink, filter);
            }
            // Lazy size fetch for the top rows (ADR-004: the index carries
            // no sizes). Cancellation checked per row.
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            for (size_t i = 0; i < items.size() && i < kSizeFetchRows; ++i) {
                if (cancelScan_.load()) break;
                if (GetFileAttributesExW(LongPath(items[i].path).c_str(),
                                         GetFileExInfoStandard, &fad)) {
                    items[i].sizeBytes =
                        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
                        static_cast<unsigned long long>(fad.nFileSizeLow);
                }
            }
        }
        size_t total = VolumeIndex::TotalEntries();
        StoreResults(TabId::Search, std::move(items));
        // v2.9: persist the search results (throttled — a search runs per
        // keystroke) so the next session reopens straight into them, query
        // and all. Clearing the box invalidates the cache explicitly.
        if (!cancelScan_.load()) {
            NoteScanTime(TabId::Search);
            static auto lastSave = std::chrono::steady_clock::now() -
                                   std::chrono::hours(1);   // shared throttle
            if (query.empty()) {
                SaveResultsCache(TabId::Search, /*allowEmpty=*/true);
            } else if (std::chrono::steady_clock::now() - lastSave >
                       std::chrono::milliseconds(1500)) {
                lastSave = std::chrono::steady_clock::now();
                SaveResultsCache(TabId::Search);
            }
        }
        SetProgress(cancelScan_.load()
            ? L"搜索已取消（有新输入）"
            : FormatW(L"匹配 %s 项（索引共 %s 项）",
                      FormatCountSimple(
                          Results(TabId::Search).size()).c_str(),
                      FormatCountSimple(total).c_str()));
        Post(hwnd, WM_APP_SEARCH_DONE);
    });
    return true;
}

void SessionService::RunScan(TabId tab, std::shared_ptr<Scanner> scanner, HWND hwnd) {    std::vector<ScanItem> buffer;
    auto t0 = std::chrono::steady_clock::now();
    try {
        scanner->Scan(buffer,
            [this, hwnd](unsigned long long done, unsigned long long total,
                         const std::wstring& msg) {
                // REVIEW-UI P2 (L-13): when a scanner phase knows its
                // done/total, the progress bar switches from marquee to a
                // determinate percentage (wp=done, lp=total).
                SetProgress(total
                    ? FormatW(L"扫描 %llu/%llu: %s", done, total, msg.c_str())
                    : L"扫描: " + msg);
                Post(hwnd, WM_APP_SCAN_PROGRESS,
                     static_cast<WPARAM>(done), static_cast<LPARAM>(total));
            },
            cancelScan_);
    } catch (const std::exception& ex) {
        MS_LOG_ERROR(L"Scanner threw: %hs", ex.what());
    } catch (...) {
        MS_LOG_ERROR(L"Scanner threw unknown exception");
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> g(resultsMu_);   // REVIEW P1-1
        results_[static_cast<size_t>(tab)] = std::move(buffer);
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        scanAt_[static_cast<size_t>(tab)] =
            (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        fromCache_[static_cast<size_t>(tab)] = false;
        // M4: benchmark record (perf numbers come from the log).
        MS_LOG_INFO(L"Scan tab=%d done: %zu items in %lld ms",
                    static_cast<int>(tab),
                    results_[static_cast<size_t>(tab)].size(),
                    static_cast<long long>(ms));
        SetProgress(FormatW(L"扫描完成: %zu 项",
                            results_[static_cast<size_t>(tab)].size()));
    }
    if (!cancelScan_.load()) {
        SaveResultsCache(tab);   // v2.5: persist for the next session
    }
    Post(hwnd, WM_APP_SCAN_DONE);
}

SessionService::PlanStart SessionService::ExecutePlan(const CleanPlan& plan) {
    if (IsBusy()) return PlanStart::Busy;
    // Quick pre-check gives the UI immediate feedback for the common case;
    // the worker re-checks authoritatively.
    if (!PlanBuilder::PlanMatches(plan, Results(plan.tab))) {
        return PlanStart::PlanStale;
    }
    SetProgress(L"执行中…");
    HWND hwnd = hwnd_;
    if (!StartTask(TaskKind::Executing, [this, plan, hwnd]() {
            RunPlan(plan, hwnd);
        })) {
        return PlanStart::Busy;
    }
    return PlanStart::Started;
}

void SessionService::RunPlan(const CleanPlan plan, HWND hwnd) {
    ExecuteReport rpt;
    // REVIEW P1-1: the plan executes against a worker-private SNAPSHOT —
    // the UI never shares this vector with the worker anymore.
    std::vector<ScanItem> items;
    {
        std::lock_guard<std::mutex> g(resultsMu_);
        items = results_[static_cast<size_t>(plan.tab)];
    }

    // Whole-plan staleness: the result list changed since confirmation.
    if (!PlanBuilder::PlanMatches(plan, items)) {
        rpt.skipped = static_cast<int>(plan.items.size());
        rpt.details = L"⚠ 计划已过期（扫描结果已变化），请重新扫描后再执行。\n";
        std::lock_guard<std::mutex> g(reportMu_);
        lastReport_ = rpt;
        SetProgress(L"计划已过期");
        Post(hwnd, WM_APP_OP_DONE);
        return;
    }

    std::vector<size_t> succeededIdx;
    succeededIdx.reserve(plan.items.size());
    int done = 0;
    const int total = static_cast<int>(plan.items.size());

    // REVIEW P3 (08-F8): optional system restore point before the first
    // migration — outcome lands in the report, failures degrade silently.
    if (plan.tab == TabId::Apps && plan.createRestorePoint && total > 0) {
        SetProgress(L"创建系统还原点…");
        Post(hwnd, WM_APP_OP_PROGRESS, 0);
        std::wstring rpNote;
        if (CreateRestorePoint(L"MiniSys 应用迁移")) {
            rpNote = L"✓ 已创建系统还原点（可在系统恢复中使用）\n";
        } else {
            rpNote = L"ℹ 未能创建系统还原点（系统策略限制或服务未启用），迁移继续。\n";
        }
        rpt.details += rpNote;
    }

    for (const auto& pi : plan.items) {
        if (pi.itemIdx >= items.size()) continue;
        const auto& si = items[pi.itemIdx];
        ++done;
        SetProgress(FormatW(L"执行 %d/%d: %s", done, total, si.title.c_str()));
        Post(hwnd, WM_APP_OP_PROGRESS,
             total > 0 ? static_cast<WPARAM>((done - 1) * 100 / total) : 0);

        std::wstring err;
        bool ok = false;

        if (si.path == L"$RECYCLE.BIN") {
            // Recycle bin is not a file operation — no GuardRails file checks.
            EmptyRecycleOp op;
            ok = op.Execute(err);
            if (ok) rpt.freedBytes += si.sizeBytes;
        } else if (si.strategy == CleanStrategy::Delegate && !si.command.empty()) {
            // Delegated system command (WinSxS/hiberfil — ADR-008). Not a
            // file operation. review-05 (T-B1, CONFIRMED): the "command
            // comes from the rule table only" assumption broke with the
            // v2.5 results cache (user-writable JSON restores strategy +
            // command verbatim). The whitelist is re-checked HERE — the
            // execution choke point — regardless of where the item came
            // from (rule table, cache, or any future source).
            {
                std::wstring denyReason;
                if (!JunkRules::DelegateCommandAllowed(si.command, denyReason)) {
                    ++rpt.skipped;
                    rpt.details += FormatW(
                        L"⊘ %s: 委派命令未过白名单（%s）——已拒绝\n",
                        si.title.c_str(), denyReason.c_str());
                    MS_LOG_WARN(L"Delegate command rejected at execution: %s — %s",
                                si.title.c_str(), denyReason.c_str());
                    continue;
                }
            }
            SetProgress(FormatW(L"委派 %d/%d: %s", done, total, si.title.c_str()));
            Post(hwnd, WM_APP_OP_PROGRESS,
                 total > 0 ? static_cast<WPARAM>((done - 1) * 100 / total) : 0);
            DelegateOp op(si.title, si.command);
            ok = op.Execute(err);
            if (ok) rpt.freedBytes += si.sizeBytes;
        } else {
            // Every file-level operation passes the safety gate first.
            auto verdict = GuardRails::Instance().Validate(pi, si);
            if (!verdict.allow) {
                ++rpt.skipped;
                rpt.details += FormatW(L"⊘ %s: %s\n", si.title.c_str(),
                                       verdict.reason.c_str());
                continue;
            }
            if (plan.tab == TabId::Apps) {
                // Target root is validated by MoveJunctionOp's preflight;
                // the GuardRails gate runs in Migration context (Program
                // Files sources are legitimate — REVIEW P0-1).
                auto verdict = GuardRails::Instance().Validate(
                    pi, si, GuardRails::Context::Migration);
                if (!verdict.allow) {
                    ++rpt.skipped;
                    rpt.details += FormatW(L"⊘ %s: %s\n", si.title.c_str(),
                                           verdict.reason.c_str());
                    continue;
                }
                fs::path target = fs::path(plan.migrateTargetRoot) / si.path.filename();
                MoveJunctionOp op(si.path, target, plan.useSymlink);
                ok = op.Execute(err);
                if (ok) rpt.freedBytes += si.sizeBytes;
            } else {
                QuarantineOp op(pi.path, si.sizeBytes);
                ok = op.Execute(err);
                if (ok) rpt.quarantinedBytes += si.sizeBytes;
            }
        }

        if (ok) {
            ++rpt.succeeded;
            succeededIdx.push_back(pi.itemIdx);
        } else {
            ++rpt.failed;
            rpt.details += FormatW(L"× %s: %s\n", si.title.c_str(), err.c_str());
        }
    }

    // Drop succeeded items from the live result list (by path — REVIEW
    // P1-1: indices refer to the snapshot, the live list may differ).
    if (!succeededIdx.empty()) {
        std::lock_guard<std::mutex> g(resultsMu_);
        auto& live = results_[static_cast<size_t>(plan.tab)];
        std::vector<ScanItem> kept;
        kept.reserve(live.size());
        for (auto& it : live) {
            bool done = false;
            for (size_t i : succeededIdx) {
                if (i < items.size() && it.path == items[i].path) { done = true; break; }
            }
            if (!done) kept.push_back(std::move(it));
        }
        live = std::move(kept);
    }

    {
        std::lock_guard<std::mutex> g(reportMu_);
        lastReport_ = rpt;
    }
    SetProgress(FormatW(L"执行完成: 成功 %d · 跳过 %d · 失败 %d",
                        rpt.succeeded, rpt.skipped, rpt.failed));
    Post(hwnd, WM_APP_OP_DONE);
}

bool SessionService::EmptyQuarantine() {
    SetProgress(L"清空隔离区…");
    HWND hwnd = hwnd_;
    return StartTask(TaskKind::Executing, [this, hwnd]() {
        RunEmptyQuarantine(hwnd);
    });
}

void SessionService::RunEmptyQuarantine(HWND hwnd) {
    // REVIEW P1-7 (03-B7 / 05-T-B10): the old flow counted bytes even for
    // drives that failed to empty, then marked EVERY record as released —
    // the history lied while files were still on disk. Per-drive verdicts
    // now decide which records get marked, and only their recorded sizes
    // count as freed.
    ExecuteReport rpt;
    unsigned long long bytes = 0;
    std::vector<wchar_t> emptiedDrives;

    for (const auto& drive : EnumerateDrives()) {
        fs::path root = fs::path(drive) / L"MiniSys.Quarantine";
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        fs::remove_all(root, ec);
        if (ec) {
            ++rpt.failed;
            rpt.details += FormatW(L"× 清空 %s 失败: %hs（文件仍在，可稍后重试）\n",
                                   root.wstring().c_str(), ec.message().c_str());
            continue;
        }
        ++rpt.succeeded;
        emptiedDrives.push_back(drive.empty() ? L'\0' : drive[0]);
    }

    // Mark only records whose quarantine target lived on an emptied drive;
    // their recorded sizes are the actually-freed bytes.
    if (!emptiedDrives.empty()) {
        std::vector<std::wstring> releasedIds;
        unsigned long long freed = 0;
        for (const auto& r : OperationLog::Instance().LoadAll()) {
            if (r.type != OpType::Quarantine || r.status != OpStatus::Success) continue;
            if (r.note.rfind(L"[已释放]", 0) == 0) continue;   // already purged
            if (r.target.empty() || r.target[1] != L':') continue;
            wchar_t d = static_cast<wchar_t>(::towupper(r.target[0]));
            bool onEmptied = false;
            for (wchar_t e : emptiedDrives) {
                if (::towupper(e) == d) { onEmptied = true; break; }
            }
            if (onEmptied) {
                releasedIds.push_back(r.id);
                freed += r.sizeBytes;
            }
        }
        OperationLog::Instance().UpdateStatusBulk(
            releasedIds, OpStatus::Success,
            L"[已释放] 隔离区已清空，文件已永久删除");
        bytes = freed;
    }

    rpt.freedBytes = bytes;
    {
        std::lock_guard<std::mutex> g(reportMu_);
        lastReport_ = rpt;
    }
    SetProgress(FormatW(L"隔离区已清空: 释放 %s",
                        FormatSize(rpt.freedBytes).c_str()));
    Post(hwnd, WM_APP_OP_DONE);
}

std::wstring SessionService::QuarantineUsageText() const {
    // REVIEW-UI P2 (U-5): this runs on every status-bar tick (progress
    // updates fire several times a second) — recompute only when the
    // operation log generation changed, not by re-parsing history.jsonl.
    uint64_t gen = OperationLog::Instance().Generation();
    {
        std::lock_guard<std::mutex> g(qCacheMu_);
        if (gen == qCacheGen_) return qCacheText_;
    }
    // REVIEW P1-7 (03-B7): purged records used to keep counting — the bar
    // never returned to zero after "清空隔离区".
    unsigned long long bytes = 0;
    unsigned long long count = 0;
    for (const auto& r : OperationLog::Instance().LoadAll()) {
        if (r.type != OpType::Quarantine || r.status != OpStatus::Success) continue;
        if (r.note.rfind(L"[已释放]", 0) == 0) continue;   // already purged
        ++count;
        bytes += r.sizeBytes;
    }
    std::wstring text;
    if (count != 0) {
        text = FormatW(L"隔离区: %llu 项 / %s (清空后释放)", count,
                       FormatSize(bytes).c_str());
    }
    {
        std::lock_guard<std::mutex> g(qCacheMu_);
        qCacheGen_  = gen;
        qCacheText_ = text;
    }
    return text;
}

bool SessionService::UndoRecordsAsync(const std::vector<OpRecord>& records) {
    if (records.empty()) return false;
    SetProgress(FormatW(L"撤销中（%zu 项）…", records.size()));
    HWND hwnd = hwnd_;
    return StartTask(TaskKind::Executing, [this, records, hwnd]() {
        ExecuteReport rpt;
        int done = 0;
        for (const auto& r : records) {
            ++done;
            SetProgress(FormatW(L"撤销 %d/%zu: %s", done, records.size(),
                                r.source.c_str()));
            Post(hwnd, WM_APP_OP_PROGRESS,
                 records.empty() ? 0
                     : static_cast<WPARAM>((done - 1) * 100 / records.size()));
            std::wstring err;
            switch (UndoRecord(r, err)) {
                case UndoResult::Ok:
                    ++rpt.succeeded;
                    break;
                case UndoResult::NotReversible:
                    ++rpt.skipped;
                    rpt.details += FormatW(L"⊘ %s: 该记录不可撤销\n", r.source.c_str());
                    break;
                case UndoResult::DeleteType:
                    ++rpt.skipped;
                    rpt.details += FormatW(L"⊘ %s: 回收站删除请手动还原\n",
                                           r.source.c_str());
                    break;
                case UndoResult::Failed:
                    ++rpt.failed;
                    rpt.details += FormatW(L"× %s: %s\n", r.source.c_str(),
                                           err.c_str());
                    break;
            }
        }
        {
            std::lock_guard<std::mutex> g(reportMu_);
            lastReport_ = rpt;
        }
        SetProgress(FormatW(L"撤销完成: 成功 %d · 跳过 %d · 失败 %d",
                            rpt.succeeded, rpt.skipped, rpt.failed));
        Post(hwnd, WM_APP_OP_DONE);
    });
}

SessionService::UndoResult SessionService::UndoRecord(const OpRecord& rec,
                                                       std::wstring& errOut) {    if (!rec.isReversible || rec.status != OpStatus::Success) {
        return UndoResult::NotReversible;
    }
    if (rec.type == OpType::Quarantine) {
        return QuarantineOp::UndoPaths(rec.id, rec.source, rec.target, errOut)
                   ? UndoResult::Ok
                   : UndoResult::Failed;
    }
    if (rec.type != OpType::MoveAndJunction) {
        return UndoResult::DeleteType;
    }
    MoveJunctionOp op(rec.source, rec.target, false);
    op.MutableRecord().id = rec.id;
    return op.Undo(errOut) ? UndoResult::Ok : UndoResult::Failed;
}

// ---- v2.8 read-only helpers on the worker -----------------------------------

bool SessionService::VerifyPathsAsync(TabId tab, std::vector<std::wstring> paths) {
    if (paths.empty()) return false;
    SetProgress(FormatW(L"校验列表（%zu 项）…", paths.size()));
    HWND hwnd = hwnd_;
    return StartTask(TaskKind::Verifying, [this, tab, paths = std::move(paths), hwnd]() {
        std::vector<std::wstring> dead;
        size_t done = 0;
        for (const auto& p : paths) {
            ++done;
            if ((done & 63) == 0) {
                SetProgress(FormatW(L"校验 %zu/%zu…", done, paths.size()));
                Post(hwnd, WM_APP_SCAN_PROGRESS);
            }
            if (p.empty() || p == L"$RECYCLE.BIN") continue;   // pseudo-item
            DWORD attrs = GetFileAttributesW(LongPath(fs::path(p)).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) dead.push_back(ToLower(p));
        }
        size_t deadCount = dead.size();
        // v2.11: prune + persist worker-side — the disk cache must not be
        // able to resurrect the ghosts after a restart (the UI-side
        // ApplyDeadPaths still refreshes the visible snapshot; its
        // StoreResults is idempotent with this one).
        if (!dead.empty()) {
            auto items = Results(tab);
            std::vector<ScanItem> kept;
            kept.reserve(items.size());
            for (auto& it : items) {
                if (it.path == L"$RECYCLE.BIN" ||
                    std::find(dead.begin(), dead.end(),
                              ToLower(it.path.wstring())) == dead.end()) {
                    kept.push_back(std::move(it));
                }
            }
            if (kept.size() != items.size()) {
                StoreResults(tab, kept);
                SaveResultsCache(tab);
            }
        }
        {
            std::lock_guard<std::mutex> g(verifyMu_);
            lastDead_ = std::move(dead);
            lastVerifyTab_ = tab;
        }
        SetProgress(deadCount == 0
            ? L"校验完成：列表均为最新"
            : FormatW(L"校验完成：%zu 项已在磁盘上不存在", deadCount));
        Post(hwnd, WM_APP_VERIFY_DONE);
    });
}

std::vector<std::wstring> SessionService::LastDeadPaths() const {
    std::lock_guard<std::mutex> g(verifyMu_);
    return lastDead_;
}

TabId SessionService::LastVerifyTab() const {
    std::lock_guard<std::mutex> g(verifyMu_);
    return lastVerifyTab_;
}

bool SessionService::PreviewAsync(TabId tab, std::vector<ScanItem> items,
                                  std::vector<size_t> selected) {
    if (selected.empty()) return false;
    SetProgress(FormatW(L"预览安全闸（%zu 项）…", selected.size()));
    HWND hwnd = hwnd_;
    return StartTask(TaskKind::Previewing,
        [this, tab, items = std::move(items), selected = std::move(selected), hwnd]() {
        PreviewReport rpt;
        size_t done = 0;
        auto ctx = (tab == TabId::Apps) ? GuardRails::Context::Migration
                                        : GuardRails::Context::FileOp;
        for (size_t idx : selected) {
            ++done;
            if ((done & 15) == 0) {
                SetProgress(FormatW(L"预览 %zu/%zu…", done, selected.size()));
                Post(hwnd, WM_APP_SCAN_PROGRESS);
            }
            if (idx >= items.size()) continue;
            const auto& it = items[idx];
            PlanItem pi;
            pi.itemIdx = idx;
            pi.path = it.path;
            pi.sizeAtScan = it.sizeBytes;
            pi.lastWriteAtScan = it.lastWriteFiletime;
            auto verdict = GuardRails::Instance().Validate(pi, it, ctx);
            if (verdict.allow) {
                ++rpt.pass;
            } else {
                ++rpt.denied;
                rpt.details += FormatW(L"⊘ %s — %s\n", it.title.c_str(),
                                       verdict.reason.c_str());
            }
        }
        {
            std::lock_guard<std::mutex> g(previewMu_);
            lastPreview_ = rpt;
        }
        SetProgress(FormatW(L"预览完成：通过 %d · 被拒 %d", rpt.pass, rpt.denied));
        Post(hwnd, WM_APP_PREVIEW_DONE);
    });
}

const SessionService::PreviewReport& SessionService::LastPreview() const {
    std::lock_guard<std::mutex> g(previewMu_);
    return lastPreview_;
}

void SessionService::Shutdown() {
    cancelScan_.store(true);
    if (worker_.joinable()) worker_.join();
}

} // namespace minisys
