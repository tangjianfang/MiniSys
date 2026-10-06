#pragma once
#include "core/TabId.h"
#include "core/Scanner.h"
#include "core/Operation.h"
#include "core/PlanBuilder.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace minisys {

// Owns the background machinery (M0: scan thread; M1: one serial worker for
// scan / plan execution / quarantine maintenance). The UI interacts
// exclusively via StartScan / ExecutePlan / PostMessage — the service never
// touches controls.
//
// Execution invariant (DESIGN-v2 §3): every Operation is constructed only
// behind PlanBuilder + GuardRails; the UI never builds operations itself.
class SessionService {
public:
    static SessionService& Instance();

    // Window receiving WM_APP_SCAN_* / WM_APP_OP_* messages.
    // nullptr (tests) disables posting.
    void SetWindow(HWND hwnd) { hwnd_ = hwnd; }

    enum class TaskKind { None = 0, Scanning = 1, Executing = 2, Searching = 3,
                          Verifying = 4, Previewing = 5 };

    bool IsBusy() const { return taskKind_.load() != 0; }
    bool IsScanning() const { return taskKind_.load() == static_cast<int>(TaskKind::Scanning); }
    bool IsSearching() const { return taskKind_.load() == static_cast<int>(TaskKind::Searching); }
    void CancelScan() { cancelScan_.store(true); }

    // ---- scanning --------------------------------------------------------
    // Returns false when another task is running.
    bool StartScan(TabId tab, std::unique_ptr<Scanner> scanner);

    // v2.3: build/refresh the shared volume index on the worker thread
    // (powers the instant-search tab). Posts WM_APP_SCAN_* like a scan.
    bool BuildIndexAsync();

    // REVIEW-UI P1 (04-1/U-1): instant search runs ON THE WORKER — the
    // UI thread never touches the index containers. Cancels/queues behind
    // an in-flight search via CancelScan(); completion posts
    // WM_APP_SEARCH_DONE, results land in Results(TabId::Search).
    bool SearchAsync(const std::wstring& query, bool matchPath);

    // ---- execution -------------------------------------------------------
    struct ExecuteReport {
        int succeeded = 0;
        int failed = 0;
        int skipped = 0;              // GuardRails denials
        unsigned long long quarantinedBytes = 0;  // movable to quarantine
        unsigned long long freedBytes = 0;        // actually freed (recycle bin emptied, ...)
        std::wstring details;         // per-item failure/skip lines
    };

    enum class PlanStart { Started, Busy, PlanStale };

    // Execute a confirmed plan on the worker thread. Completion is posted as
    // WM_APP_OP_DONE; the report is then available via LastReport().
    PlanStart ExecutePlan(const CleanPlan& plan);

    // Permanently delete all quarantine roots and mark their records as
    // purged (async; the real space release step). Completion: WM_APP_OP_DONE.
    bool EmptyQuarantine();

    const ExecuteReport& LastReport() const;

    // "Quarantine: N items / X" computed from history (for the status bar).
    std::wstring QuarantineUsageText() const;

    // ---- results ---------------------------------------------------------
    // REVIEW P1-1 (04-R1): returns a COPY under a lock — the UI used to hold
    // a live reference to the worker-owned vector across message loops.
    std::vector<ScanItem> Results(TabId tab) const;
    // REVIEW-UI P1 (04-2): worker-side locked store (replaces the unlocked
    // MutableResults escape hatch that raced RunPlan).
    void StoreResults(TabId tab, std::vector<ScanItem> items);

    // v2.5 cached results: when was this tab last scanned (FILETIME ticks),
    // and did the current list come from the on-disk cache? Per-tab — each
    // tab's staleness is judged separately. Execution safety does not depend
    // on freshness (planHash + per-item size/mtime re-verification).
    uint64_t LastScanAt(TabId tab) const;
    bool     ResultsFromCache(TabId tab) const;
    // Load a persisted result list from %LOCALAPPDATA%\MiniSys\cache (UI
    // startup convenience). Returns false when no usable cache exists.
    bool TryLoadCachedResults(TabId tab);

    // Latest progress text (thread-safe read).
    std::wstring ProgressText() const;

    // ---- undo ------------------------------------------------------------
    enum class UndoResult { Ok, NotReversible, DeleteType, Failed };
    UndoResult UndoRecord(const OpRecord& rec, std::wstring& errOut);

    // ---- v2.8 read-only helpers on the worker ------------------------------
    // Batch existence check for a tab's visible list ("deleted by hand in
    // Explorer"). Completion posts WM_APP_VERIFY_DONE; the dead paths are
    // then available via LastDeadPaths()/LastVerifyTab().
    bool VerifyPathsAsync(TabId tab, std::vector<std::wstring> paths);
    std::vector<std::wstring> LastDeadPaths() const;
    TabId LastVerifyTab() const;

    // GuardRails dry-run ("预览执行") over the checked rows. Completion posts
    // WM_APP_PREVIEW_DONE; the report is in LastPreview(). The canonical-
    // path resolution does real I/O per item — this used to freeze the UI.
    struct PreviewReport {
        int pass = 0;
        int denied = 0;
        std::wstring details;   // "⊘ title — reason" lines
    };
    bool PreviewAsync(TabId tab, std::vector<ScanItem> items,
                      std::vector<size_t> selected);
    const PreviewReport& LastPreview() const;

    // REVIEW P1-4 (04-R5 / 03-B10): batch undo on the worker thread — the
    // synchronous UI-thread version froze the window for the whole
    // (multi-GB) copy-back. Completion: WM_APP_OP_DONE, report in
    // LastReport(); per-record failures listed in details.
    bool UndoRecordsAsync(const std::vector<OpRecord>& records);

    // Cancels and joins any running task (call before the window goes away).
    void Shutdown();

private:
    SessionService();
    ~SessionService();
    SessionService(const SessionService&) = delete;
    SessionService& operator=(const SessionService&) = delete;

    bool StartTask(TaskKind kind, std::function<void()> body);
    void RunScan(TabId tab, std::shared_ptr<Scanner> scanner, HWND hwnd);
    void RunPlan(const CleanPlan plan, HWND hwnd);
    void RunEmptyQuarantine(HWND hwnd);
    void SaveResultsCache(TabId tab);   // v2.5 persisted scan results

    void SetProgress(const std::wstring& text);
    void Post(HWND hwnd, UINT msg, WPARAM wp = 0, LPARAM lp = 0);
    static std::wstring FormatCountSimple(size_t n);   // "1,234,567"

    HWND hwnd_ = nullptr;
    std::thread worker_;
    std::atomic<int> taskKind_{0};          // TaskKind
    std::atomic<bool> cancelScan_{false};
    mutable std::mutex progressMu_;
    std::wstring progressText_;
    mutable std::mutex resultsMu_;          // REVIEW P1-1
    std::vector<std::vector<ScanItem>> results_;   // indexed by TabId
    // v2.5 cache metadata (guarded by resultsMu_ alongside results_).
    uint64_t scanAt_[static_cast<size_t>(TabId::Count)] = {};
    bool     fromCache_[static_cast<size_t>(TabId::Count)] = {};

    // v2.5 (U-5): quarantine usage text is recomputed only when the
    // operation log generation changes, not on every progress tick.
    mutable std::mutex qCacheMu_;
    mutable uint64_t    qCacheGen_ = 0;
    mutable std::wstring qCacheText_;

    mutable std::mutex reportMu_;
    ExecuteReport lastReport_;

    // v2.8 verify / preview results (worker writes, UI reads on DONE).
    mutable std::mutex verifyMu_;
    std::vector<std::wstring> lastDead_;
    TabId lastVerifyTab_ = TabId::Junk;
    mutable std::mutex previewMu_;
    PreviewReport lastPreview_;
};

} // namespace minisys
