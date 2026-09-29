#include "core/DeleteOp.h"

#include "core/OperationLog.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shellapi.h>

namespace minisys {

DeleteOp::DeleteOp(std::filesystem::path path, unsigned long long sizeBytes) : path_(std::move(path)) {
    rec_.id = OperationLog::NewId();
    rec_.type = OpType::DeleteToRecycleBin;
    rec_.source = path_.wstring();
    rec_.sizeBytes = sizeBytes;
    rec_.isReversible = true;
}

bool DeleteOp::Execute(std::wstring& errOut) {
    // M1: migrated from deprecated SHFileOperationW to IFileOperation
    // (DESIGN-v2 R-006). Delete-to-recycle-bin via the shell COM API.
    IFileOperation* pfo = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pfo));
    if (FAILED(hr)) {
        errOut = FormatW(L"CoCreateInstance(FileOperation) failed (0x%08lX)", hr);
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    bool ok = false;
    IShellItem* item = nullptr;
    hr = SHCreateItemFromParsingName(LongPath(path_).c_str(), nullptr,
                                      IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr)) {
        pfo->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
                               FOF_NOERRORUI | FOFX_RECYCLEONDELETE);
        hr = pfo->DeleteItem(item, nullptr);
        item->Release();
        if (SUCCEEDED(hr)) {
            hr = pfo->PerformOperations();
            if (SUCCEEDED(hr)) {
                BOOL aborted = FALSE;
                pfo->GetAnyOperationsAborted(&aborted);
                if (aborted) {
                    errOut = L"操作被中止";
                } else {
                    ok = true;
                }
            } else {
                errOut = FormatW(L"IFileOperation::PerformOperations failed (0x%08lX)", hr);
            }
        } else {
            errOut = FormatW(L"IFileOperation::DeleteItem failed (0x%08lX)", hr);
        }
    } else {
        errOut = FormatW(L"SHCreateItemFromParsingName failed (0x%08lX)", hr);
    }
    pfo->Release();

    if (!ok) {
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
    // Restore-from-recycle needs the recycle bin item id; from v2 on, the
    // default delete path is QuarantineOp (fully reversible). This optional
    // strategy still requires a manual restore.
    errOut = L"回收站删除请从回收站手动还原（默认策略已改为隔离区，可自动撤销）";
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
