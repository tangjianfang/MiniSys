#include "core/MoveJunctionOp.h"
#include "core/GuardRails.h"
#include "core/OperationLog.h"
#include "platform/Junction.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <shellapi.h>
#include <restartmanager.h>
#include <vector>

namespace fs = std::filesystem;

namespace minisys {

namespace {

// Protected-path policy now lives in GuardRails (single source of truth).
// Migration uses its own list: app sources legitimately live under
// Program Files, which file operations must never touch (REVIEW P0-1).
bool IsUnderProtected(const fs::path& p) {
    return GuardRails::IsProtectedMigrationSource(p);
}

std::wstring DoubleNull(const std::wstring& s) {
    std::wstring r = s;
    r.push_back(L'\0');
    r.push_back(L'\0');
    return r;
}

// Returns names of running processes that hold files inside `dir`. Empty vector means none.
std::vector<std::wstring> RestartManagerCheck(const fs::path& dir) {
    std::vector<std::wstring> out;
    DWORD session = 0;
    WCHAR sessKey[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, sessKey) != ERROR_SUCCESS) return out;
    auto path = dir.wstring();
    LPCWSTR files[1] = { path.c_str() };
    if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) != ERROR_SUCCESS) {
        RmEndSession(session);
        return out;
    }
    UINT nProcInfoNeeded = 0, nProcInfo = 0;
    DWORD reboot = 0;
    DWORD st = RmGetList(session, &nProcInfoNeeded, &nProcInfo, nullptr, &reboot);
    if (st == ERROR_MORE_DATA && nProcInfoNeeded > 0) {
        std::vector<RM_PROCESS_INFO> infos(nProcInfoNeeded);
        nProcInfo = nProcInfoNeeded;
        if (RmGetList(session, &nProcInfoNeeded, &nProcInfo, infos.data(), &reboot) == ERROR_SUCCESS) {
            for (UINT i = 0; i < nProcInfo; ++i) {
                out.emplace_back(infos[i].strAppName);
            }
        }
    }
    RmEndSession(session);
    return out;
}

bool ShCopyDirectory(const fs::path& src, const fs::path& dst, std::wstring& err) {
    auto from = DoubleNull(src.wstring());
    auto to   = DoubleNull(dst.wstring());
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_COPY;
    op.pFrom = from.c_str();
    op.pTo   = to.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI |
                FOF_SILENT | FOF_NO_UI;
    int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) {
        err = FormatW(L"复制失败（SHFileOperation 错误码 %d）", r);
        return false;
    }
    return true;
}

bool ShDeleteDirectoryPermanent(const fs::path& dir, std::wstring& err) {
    auto from = DoubleNull(dir.wstring());
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NO_UI;
    int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) {
        err = FormatW(L"删除失败（SHFileOperation 错误码 %d）", r);
        return false;
    }
    return true;
}

} // namespace

MoveJunctionOp::MoveJunctionOp(fs::path source, fs::path target, bool useSymlink)
    : source_(std::move(source)), target_(std::move(target)), useSymlink_(useSymlink) {
    rec_.id = OperationLog::NewId();
    rec_.type = OpType::MoveAndJunction;
    rec_.source = source_.wstring();
    rec_.target = target_.wstring();
    rec_.isReversible = true;
}

std::wstring MoveJunctionOp::PreflightCheck() const {
    unsigned long long unused = 0;
    return PreflightCheckImpl(unused);
}

std::wstring MoveJunctionOp::PreflightCheckImpl(unsigned long long& srcSizeOut) const {
    if (!DirExists(source_)) return L"源目录不存在（可能已被处理）。";
    if (IsReparsePoint(source_)) return L"源目录已是 Junction/符号链接，拒绝迁移。";
    if (IsUnderProtected(source_)) return L"源目录位于受保护系统区域，拒绝迁移。";
    if (IsUnderProtected(target_)) return L"目标目录位于受保护系统区域，拒绝迁移。";
    if (DirExists(target_)) return L"目标目录已存在，请换一个不存在的路径。";

    auto srcSize = DirectorySize(source_);
    srcSizeOut = srcSize;   // REVIEW P1-3 (03-B12): shared with Execute
    DiskSpace ds;
    auto tgtRoot = DriveRootOf(target_);
    if (tgtRoot.empty() || !QueryDiskSpace(tgtRoot, ds)) return L"无法查询目标磁盘空间。";
    auto required = static_cast<unsigned long long>(srcSize * 1.1);
    if (ds.freeBytes < required) {
        return FormatW(L"目标盘 %s 空间不足：需要 %s，仅有 %s。",
            tgtRoot.c_str(), FormatSize(required).c_str(), FormatSize(ds.freeBytes).c_str());
    }
    auto holders = RestartManagerCheck(source_);
    if (!holders.empty()) {
        std::wstring s = L"源目录文件正被占用: ";
        for (size_t i = 0; i < holders.size(); ++i) {
            if (i) s += L", ";
            s += holders[i];
        }
        return s + L"。请关闭这些程序后重试。";
    }
    return {};
}

bool MoveJunctionOp::Execute(std::wstring& errOut) {
    unsigned long long srcSize = 0;
    auto blocker = PreflightCheckImpl(srcSize);
    if (!blocker.empty()) {
        errOut = blocker;
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    // REVIEW P1-3 (03-B12): the preflight already walked the tree — reuse
    // its result instead of a second full DirectorySize pass.
    rec_.sizeBytes = srcSize;

    // 1. Ensure target parent exists
    std::error_code ec;
    fs::create_directories(target_.parent_path(), ec);

    // 2. Copy
    std::wstring err;
    if (!ShCopyDirectory(source_, target_, err)) {
        errOut = err;
        // Best-effort: remove partially copied target
        std::wstring _; ShDeleteDirectoryPermanent(target_, _);
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    // 3. Delete source contents (we delete the whole source dir, then re-create as junction)
    if (!ShDeleteDirectoryPermanent(source_, err)) {
        errOut = L"Copy succeeded but source delete failed: " + err +
                 L"。目标已保留在 " + target_.wstring();
        rec_.status = OpStatus::Interrupted;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    // 4. Create junction (or symlink)
    bool ok = useSymlink_
        ? CreateDirectorySymlink(source_, target_, err)
        : CreateDirectoryJunction(source_, target_, err);
    if (!ok) {
        // Roll back: copy target back to source position.
        std::wstring _;
        if (ShCopyDirectory(target_, source_, _)) {
            std::wstring _2; ShDeleteDirectoryPermanent(target_, _2);
            errOut = L"创建 Junction 失败: " + err + L"。源目录已还原。";
            rec_.status = OpStatus::Failed;
            rec_.note = errOut;
            OperationLog::Instance().Append(rec_);
            return false;
        }
        errOut = L"创建 Junction 失败且回滚失败，需人工恢复。源=" +
                 source_.wstring() + L" target=" + target_.wstring() + L"。原因: " + err;
        rec_.status = OpStatus::Interrupted;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    // 5. Post-migration self-check (M3): the reparse point must resolve back
    //    to the target we created. A silent mismatch would leave the app
    //    (or Windows update) reading the wrong directory.
    {
        std::wstring resolved;
        if (!ReadReparseTarget(source_, resolved) ||
            !IEquals(fs::path(resolved).lexically_normal().wstring(),
                     fs::path(target_).lexically_normal().wstring())) {
            errOut = L"Junction 已创建但自检失败: 指向 " +
                     resolved + L"（应为 " + target_.wstring() + L"）";
            MS_LOG_ERROR(L"%s", errOut.c_str());
            rec_.status = OpStatus::Interrupted;
            rec_.note = errOut;
            OperationLog::Instance().Append(rec_);
            return false;
        }
    }

    rec_.status = OpStatus::Success;
    OperationLog::Instance().Append(rec_);
    return true;
}

bool MoveJunctionOp::Undo(std::wstring& errOut) {
    if (!IsReparsePoint(source_)) {
        errOut = L"源目录不是 Junction — 无需撤销或已撤销过。";
        return false;
    }
    if (!DirExists(target_)) {
        errOut = L"目标目录不存在，无法还原。";
        return false;
    }
    // 1. Remove junction at source
    std::wstring err;
    if (!RemoveDirectoryReparsePoint(source_, err)) {
        errOut = L"移除 Junction 失败: " + err;
        return false;
    }
    // 2. Copy target back to source
    if (!ShCopyDirectory(target_, source_, err)) {
        errOut = L"拷回失败: " + err;
        return false;
    }
    // 3. Delete target
    std::wstring _; ShDeleteDirectoryPermanent(target_, _);

    OperationLog::Instance().UpdateStatus(rec_.id, OpStatus::Reverted, L"Undone via MoveJunctionOp::Undo");
    return true;
}

} // namespace minisys
