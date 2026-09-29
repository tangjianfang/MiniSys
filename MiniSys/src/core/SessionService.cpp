#include "core/SessionService.h"

#include "core/DeleteOp.h"
#include "core/DelegateOp.h"
#include "core/GuardRails.h"
#include "core/MoveJunctionOp.h"
#include "core/OperationLog.h"
#include "core/QuarantineOp.h"
#include "res/resource.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace minisys {

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

void SessionService::Post(HWND hwnd, UINT msg) {
    if (hwnd) PostMessageW(hwnd, msg, 0, 0);
}

std::wstring SessionService::ProgressText() const {
    std::lock_guard<std::mutex> g(progressMu_);
    return progressText_;
}

const SessionService::ExecuteReport& SessionService::LastReport() const {
    std::lock_guard<std::mutex> g(reportMu_);
    return lastReport_;
}

const std::vector<ScanItem>& SessionService::Results(TabId tab) const {
    return results_[static_cast<size_t>(tab)];
}

std::vector<ScanItem>& SessionService::MutableResults(TabId tab) {
    return results_[static_cast<size_t>(tab)];
}

bool SessionService::StartTask(TaskKind kind, std::function<void()> body) {
    int expected = static_cast<int>(TaskKind::None);
    if (!taskKind_.compare_exchange_strong(expected, static_cast<int>(kind))) {
        return false;   // busy
    }
    if (worker_.joinable()) worker_.join();
    cancelScan_.store(false);
    worker_ = std::thread([this, body = std::move(body)]() mutable {
        // Shell operations (IFileOperation) need COM on this thread.
        bool com = SUCCEEDED(CoInitializeEx(nullptr,
                          COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        try {
            body();
        } catch (const std::exception& ex) {
            MS_LOG_ERROR(L"Task threw: %hs", ex.what());
        } catch (...) {
            MS_LOG_ERROR(L"Task threw unknown exception");
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

void SessionService::RunScan(TabId tab, std::shared_ptr<Scanner> scanner, HWND hwnd) {
    std::vector<ScanItem> buffer;
    auto t0 = std::chrono::steady_clock::now();
    try {
        scanner->Scan(buffer,
            [this, hwnd](unsigned long long, unsigned long long, const std::wstring& msg) {
                SetProgress(L"扫描: " + msg);
                Post(hwnd, WM_APP_SCAN_PROGRESS);
            },
            cancelScan_);
    } catch (const std::exception& ex) {
        MS_LOG_ERROR(L"Scanner threw: %hs", ex.what());
    } catch (...) {
        MS_LOG_ERROR(L"Scanner threw unknown exception");
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    results_[static_cast<size_t>(tab)] = std::move(buffer);
    // M4: benchmark record (perf numbers come from the log).
    MS_LOG_INFO(L"Scan tab=%d done: %zu items in %lld ms",
                static_cast<int>(tab),
                results_[static_cast<size_t>(tab)].size(), static_cast<long long>(ms));
    SetProgress(FormatW(L"扫描完成: %zu 项",
                        results_[static_cast<size_t>(tab)].size()));
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
    auto& items = results_[static_cast<size_t>(plan.tab)];

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

    for (const auto& pi : plan.items) {
        if (pi.itemIdx >= items.size()) continue;
        const auto& si = items[pi.itemIdx];
        ++done;
        SetProgress(FormatW(L"执行 %d/%d: %s", done, total, si.title.c_str()));
        Post(hwnd, WM_APP_OP_PROGRESS);

        std::wstring err;
        bool ok = false;

        if (si.path == L"$RECYCLE.BIN") {
            // Recycle bin is not a file operation — no GuardRails file checks.
            EmptyRecycleOp op;
            ok = op.Execute(err);
            if (ok) rpt.freedBytes += si.sizeBytes;
        } else if (si.strategy == CleanStrategy::Delegate && !si.command.empty()) {
            // Delegated system command (WinSxS/hiberfil — ADR-008). Not a
            // file operation; the command comes from the rule table only.
            SetProgress(FormatW(L"委派 %d/%d: %s", done, total, si.title.c_str()));
            Post(hwnd, WM_APP_OP_PROGRESS);
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
                // Target root is validated by MoveJunctionOp's preflight.
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

    // Drop succeeded items from the result list so a refresh shows reality.
    if (!succeededIdx.empty()) {
        std::vector<bool> removed(items.size(), false);
        for (size_t i : succeededIdx) removed[i] = true;
        std::vector<ScanItem> kept;
        kept.reserve(items.size() - succeededIdx.size());
        for (size_t i = 0; i < items.size(); ++i) {
            if (!removed[i]) kept.push_back(std::move(items[i]));
        }
        items = std::move(kept);
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
    ExecuteReport rpt;
    unsigned long long bytes = 0;
    int roots = 0;

    for (const auto& drive : EnumerateDrives()) {
        fs::path root = fs::path(drive) / L"MiniSys.Quarantine";
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        ++roots;
        bytes += DirectorySize(root);
        fs::remove_all(root, ec);
        if (ec) {
            rpt.failed++;
            rpt.details += FormatW(L"× 清空 %s 失败: %hs\n", root.wstring().c_str(),
                                   ec.message().c_str());
        }
    }

    // Mark all successful quarantine records as purged.
    for (const auto& r : OperationLog::Instance().LoadAll()) {
        if (r.type == OpType::Quarantine && r.status == OpStatus::Success) {
            OperationLog::Instance().UpdateStatus(
                r.id, OpStatus::Success, L"[已释放] 隔离区已清空，文件已永久删除");
        }
    }

    rpt.succeeded = roots;
    rpt.freedBytes = bytes;
    {
        std::lock_guard<std::mutex> g(reportMu_);
        lastReport_ = rpt;
    }
    SetProgress(FormatW(L"隔离区已清空: 释放 %s",
                        FormatSize(bytes).c_str()));
    Post(hwnd, WM_APP_OP_DONE);
}

std::wstring SessionService::QuarantineUsageText() const {
    unsigned long long bytes = 0;
    unsigned long long count = 0;
    for (const auto& r : OperationLog::Instance().LoadAll()) {
        if (r.type == OpType::Quarantine && r.status == OpStatus::Success) {
            ++count;
            bytes += r.sizeBytes;
        }
    }
    if (count == 0) return {};
    return FormatW(L"隔离区: %llu 项 / %s (清空后释放)", count,
                   FormatSize(bytes).c_str());
}

SessionService::UndoResult SessionService::UndoRecord(const OpRecord& rec,
                                                       std::wstring& errOut) {
    if (!rec.isReversible || rec.status != OpStatus::Success) {
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

void SessionService::Shutdown() {
    cancelScan_.store(true);
    if (worker_.joinable()) worker_.join();
}

} // namespace minisys
