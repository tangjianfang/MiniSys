#include "core/SessionService.h"

#include "core/DeleteOp.h"
#include "core/DelegateOp.h"
#include "core/GuardRails.h"
#include "core/MoveJunctionOp.h"
#include "core/OperationLog.h"
#include "core/QuarantineOp.h"
#include "platform/SystemRestore.h"
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

void SessionService::Post(HWND hwnd, UINT msg, WPARAM wp) {
    if (hwnd) PostMessageW(hwnd, msg, wp, 0);
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
    {
        std::lock_guard<std::mutex> g(resultsMu_);   // REVIEW P1-1
        results_[static_cast<size_t>(tab)] = std::move(buffer);
        // M4: benchmark record (perf numbers come from the log).
        MS_LOG_INFO(L"Scan tab=%d done: %zu items in %lld ms",
                    static_cast<int>(tab),
                    results_[static_cast<size_t>(tab)].size(),
                    static_cast<long long>(ms));
        SetProgress(FormatW(L"扫描完成: %zu 项",
                            results_[static_cast<size_t>(tab)].size()));
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
            // file operation; the command comes from the rule table only.
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
    if (count == 0) return {};
    return FormatW(L"隔离区: %llu 项 / %s (清空后释放)", count,
                   FormatSize(bytes).c_str());
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

void SessionService::Shutdown() {
    cancelScan_.store(true);
    if (worker_.joinable()) worker_.join();
}

} // namespace minisys
