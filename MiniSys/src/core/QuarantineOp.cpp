#include "core/QuarantineOp.h"

#include "core/GuardRails.h"
#include "core/OperationLog.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>

namespace fs = std::filesystem;

namespace minisys {

namespace {

std::wstring LastErr(const wchar_t* prefix) {
    DWORD e = GetLastError();
    return std::wstring(prefix) + L" (Win32 " + std::to_wstring(e) + L")";
}

bool MoveWithRetry(const fs::path& from, const fs::path& to, std::wstring& err) {
    if (MoveFileExW(LongPath(from).c_str(), LongPath(to).c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    err = LastErr(L"MoveFileEx failed");
    return false;
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
    if (!MoveWithRetry(source_, target, err)) {
        errOut = err;
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
    std::wstring err;
    if (!MoveWithRetry(quarantinePath, restoreTo, err)) {
        errOut = err;
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
