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

    enum class TaskKind { None = 0, Scanning = 1, Executing = 2 };

    bool IsBusy() const { return taskKind_.load() != 0; }
    bool IsScanning() const { return taskKind_.load() == static_cast<int>(TaskKind::Scanning); }
    void CancelScan() { cancelScan_.store(true); }

    // ---- scanning --------------------------------------------------------
    // Returns false when another task is running.
    bool StartScan(TabId tab, std::unique_ptr<Scanner> scanner);

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
    const std::vector<ScanItem>& Results(TabId tab) const;
    std::vector<ScanItem>&       MutableResults(TabId tab);

    // Latest progress text (thread-safe read).
    std::wstring ProgressText() const;

    // ---- undo ------------------------------------------------------------
    enum class UndoResult { Ok, NotReversible, DeleteType, Failed };
    UndoResult UndoRecord(const OpRecord& rec, std::wstring& errOut);

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

    void SetProgress(const std::wstring& text);
    void Post(HWND hwnd, UINT msg);

    HWND hwnd_ = nullptr;
    std::thread worker_;
    std::atomic<int> taskKind_{0};          // TaskKind
    std::atomic<bool> cancelScan_{false};
    mutable std::mutex progressMu_;
    std::wstring progressText_;
    std::vector<std::vector<ScanItem>> results_;   // indexed by TabId

    mutable std::mutex reportMu_;
    ExecuteReport lastReport_;
};

} // namespace minisys
