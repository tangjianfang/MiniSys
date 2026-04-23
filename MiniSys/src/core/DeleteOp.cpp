#include "core/DeleteOp.h"
#include "core/OperationLog.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <shellapi.h>

namespace minisys {

namespace {
// SHFileOperationW expects double-null-terminated path.
std::wstring DoubleNull(const std::wstring& s) {
    std::wstring r = s;
    r.push_back(L'\0');
    r.push_back(L'\0');
    return r;
}
} // namespace

DeleteOp::DeleteOp(std::filesystem::path path, unsigned long long sizeBytes) : path_(std::move(path)) {
    rec_.id = OperationLog::NewId();
    rec_.type = OpType::DeleteToRecycleBin;
    rec_.source = path_.wstring();
    rec_.sizeBytes = sizeBytes;
    rec_.isReversible = true;
}

bool DeleteOp::Execute(std::wstring& errOut) {
    auto buf = DoubleNull(path_.wstring());
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                FOF_NOCONFIRMMKDIR | FOF_NO_UI;
    int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) {
        errOut = FormatW(L"SHFileOperation FO_DELETE failed (code %d)", r);
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    rec_.status = OpStatus::Success;
    OperationLog::Instance().Append(rec_);
    return true;
}

bool DeleteOp::Undo(std::wstring& errOut) {
    // Restore-from-recycle is non-trivial without storing the recycle bin item id.
    // For v1 we instruct user to restore manually from the Recycle Bin shell folder.
    errOut = L"Auto-undo not implemented in v1. Open Recycle Bin and restore '"
             + path_.filename().wstring() + L"' manually.";
    return false;
}

EmptyRecycleOp::EmptyRecycleOp() {
    rec_.id = OperationLog::NewId();
    rec_.type = OpType::EmptyRecycleBin;
    rec_.source = L"$RECYCLE.BIN";
    rec_.isReversible = false;
}

bool EmptyRecycleOp::Execute(std::wstring& errOut) {
    RecycleBinInfo info;
    if (QueryRecycleBin(info)) {
        rec_.sizeBytes = info.sizeBytes;
    }
    if (!EmptyRecycleBinAll()) {
        errOut = L"SHEmptyRecycleBin failed";
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    rec_.status = OpStatus::Success;
    OperationLog::Instance().Append(rec_);
    return true;
}

bool EmptyRecycleOp::Undo(std::wstring& errOut) {
    errOut = L"Empty Recycle Bin is irreversible.";
    return false;
}

} // namespace minisys
