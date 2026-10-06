#include "core/QuarantineOp.h"

#include "core/GuardRails.h"
#include "core/OperationLog.h"
#include "platform/RestartManager.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Win32Error.h"

#include <windows.h>

namespace fs = std::filesystem;

namespace minisys {

namespace {

// REVIEW P1-2 (03-B6 / 05-T-B7): real retry with back-off for the transient
// lock errors, then a Restart Manager diagnosis so the failure message names
// the culprit instead of "Win32 5".
bool MoveWithRetry(const fs::path& from, const fs::path& to,
                   std::wstring& err, unsigned long* lastError) {
    constexpr int kAttempts = 3;
    constexpr DWORD kDelaysMs[kAttempts - 1] = { 150, 400 };
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (MoveFileExW(LongPath(from).c_str(), LongPath(to).c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        *lastError = GetLastError();
        DWORD e = *lastError;
        bool transient = (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION);
        if (!transient || attempt == kAttempts - 1) break;
        Sleep(kDelaysMs[attempt]);
    }
    err = FormatW(L"无法移动（Win32 %lu）", *lastError);
    return false;
}

std::wstring DiagnoseLock(const fs::path& source, unsigned long lastError) {
    if (lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION) {
        return Win32ErrorText(lastError);
    }
    auto holders = LockingProcesses(source);
    if (holders.empty()) {
        return Win32ErrorText(lastError);
    }
    std::wstring list;
    for (size_t i = 0; i < holders.size() && i < 5; ++i) {
        if (i) list += L"、";
        list += holders[i];
    }
    if (holders.size() > 5) list += L" 等";
    return L"文件正被 " + list + L" 占用，关闭这些程序后重试（或重启电脑后立即执行）。";
}

} // namespace

QuarantineOp::QuarantineOp(fs::path source, unsigned long long sizeBytes)
    : source_(std::move(source)) {
    rec_.id = OperationLog::NewId();
    rec_.type = OpType::Quarantine;
    rec_.source = source_.wstring();
    rec_.sizeBytes = sizeBytes;
    rec_.isReversible = true;
}

fs::path QuarantineOp::QuarantineRootFor(const fs::path& source) {
    auto root = DriveRootOf(source);
    if (root.empty()) root = SystemDriveRoot();
    return fs::path(root) / L"MiniSys.Quarantine";
}

fs::path QuarantineOp::UniqueTargetFor(const fs::path& source) {
    auto root = QuarantineRootFor(source);
    auto name = source.filename().wstring();
    fs::path candidate = root / name;
    int suffix = 2;
    while (FileExists(candidate) || DirExists(candidate)) {
        candidate = root / FormatW(L"%s-%d", name.c_str(), suffix);
        ++suffix;
    }
    return candidate;
}

bool QuarantineOp::Execute(std::wstring& errOut) {
    // Defense in depth: SessionService validated via GuardRails already;
    // re-check the essentials here so the op is safe standalone too.
    if (!FileExists(source_) && !DirExists(source_)) {
        errOut = L"路径已不存在";
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    if (GuardRails::IsProtectedPath(source_)) {
        errOut = L"受保护系统路径，拒绝操作";
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    auto root = QuarantineRootFor(source_);
    std::error_code ec;
    fs::create_directories(root, ec);

    auto target = UniqueTargetFor(source_);
    std::wstring err;
    unsigned long lastError = 0;
    if (!MoveWithRetry(source_, target, err, &lastError)) {
        // REVIEW P1-2: explain WHY in human terms (occupying processes or
        // the matching advice for the error class).
        errOut = DiagnoseLock(source_, lastError) +
                 FormatW(L"（Win32 %lu）", lastError);
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    rec_.target = target.wstring();
    rec_.status = OpStatus::Success;
    OperationLog::Instance().Append(rec_);
    return true;
}

bool QuarantineOp::Undo(std::wstring& errOut) {
    return UndoPaths(rec_.id, source_, fs::path(rec_.target), errOut);
}

bool QuarantineOp::UndoPaths(const std::wstring& recordId,
                             const fs::path& source,
                             const fs::path& quarantinePath,
                             std::wstring& errOut) {
    if (!FileExists(quarantinePath) && !DirExists(quarantinePath)) {
        errOut = L"隔离区中找不到该项目（可能已被清空）";
        return false;
    }
    fs::path restoreTo = source;
    if (FileExists(restoreTo) || DirExists(restoreTo)) {
        // Original path was recreated meanwhile — don't clobber it.
        restoreTo = fs::path(source.wstring() + L".restored");
    }
    // REVIEW P1-4 (05-T-B6): undo used to bypass every gate — at minimum
    // never restore INTO a protected system path.
    if (GuardRails::IsProtectedPath(restoreTo)) {
        errOut = L"还原目标位于受保护系统路径，已拒绝: " + restoreTo.wstring();
        return false;
    }
    std::wstring err;
    unsigned long lastError = 0;
    if (!MoveWithRetry(quarantinePath, restoreTo, err, &lastError)) {
        errOut = DiagnoseLock(quarantinePath, lastError) +
                 FormatW(L"（Win32 %lu）", lastError);
        return false;
    }
    if (!recordId.empty()) {
        OperationLog::Instance().UpdateStatus(
            recordId, OpStatus::Reverted,
            L"已还原到 " + restoreTo.wstring());
    }
    return true;
}

} // namespace minisys
