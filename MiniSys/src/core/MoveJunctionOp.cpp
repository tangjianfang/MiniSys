#include "core/MoveJunctionOp.h"
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

const std::vector<fs::path>& ProtectedPaths() {
    static const std::vector<fs::path> v = []{
        auto sd = SystemDriveRoot();
        return std::vector<fs::path>{
            fs::path(sd) / L"Windows",
            fs::path(sd) / L"Program Files" / L"WindowsApps",
            fs::path(sd) / L"ProgramData" / L"Microsoft",
            fs::path(sd) / L"$Recycle.Bin",
            fs::path(sd) / L"System Volume Information",
        };
    }();
    return v;
}

bool IsUnderProtected(const fs::path& p) {
    auto sp = ToLower(p.wstring());
    for (auto& prot : ProtectedPaths()) {
        auto sx = ToLower(prot.wstring());
        if (sp.size() >= sx.size() && sp.compare(0, sx.size(), sx) == 0) return true;
    }
    return false;
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
        err = FormatW(L"Copy failed (SHFileOperation code %d)", r);
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
        err = FormatW(L"Delete failed (SHFileOperation code %d)", r);
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
    if (!DirExists(source_)) return L"Source directory does not exist.";
    if (IsReparsePoint(source_)) return L"Source is already a reparse point (junction/symlink).";
    if (IsUnderProtected(source_)) return L"Source is under a protected system path; refusing to move.";
    if (IsUnderProtected(target_)) return L"Target is under a protected system path; refusing to move.";
    if (DirExists(target_)) return L"Target directory already exists; choose a non-existent path.";

    auto srcSize = DirectorySize(source_);
    DiskSpace ds;
    auto tgtRoot = DriveRootOf(target_);
    if (tgtRoot.empty() || !QueryDiskSpace(tgtRoot, ds)) return L"Cannot query target disk space.";
    auto required = static_cast<unsigned long long>(srcSize * 1.1);
    if (ds.freeBytes < required) {
        return FormatW(L"Insufficient free space on %s: needs %s, has %s.",
            tgtRoot.c_str(), FormatSize(required).c_str(), FormatSize(ds.freeBytes).c_str());
    }
    auto holders = RestartManagerCheck(source_);
    if (!holders.empty()) {
        std::wstring s = L"Files in source are in use by: ";
        for (size_t i = 0; i < holders.size(); ++i) {
            if (i) s += L", ";
            s += holders[i];
        }
        return s + L". Close these apps and retry.";
    }
    return {};
}

bool MoveJunctionOp::Execute(std::wstring& errOut) {
    auto blocker = PreflightCheck();
    if (!blocker.empty()) {
        errOut = blocker;
        rec_.status = OpStatus::Failed;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }
    rec_.sizeBytes = DirectorySize(source_);

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
                 L". Target preserved at " + target_.wstring();
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
            errOut = L"Failed to create reparse point: " + err + L". Source restored.";
            rec_.status = OpStatus::Failed;
            rec_.note = errOut;
            OperationLog::Instance().Append(rec_);
            return false;
        }
        errOut = L"Reparse point creation failed AND rollback failed. Manual recovery needed: source=" +
                 source_.wstring() + L" target=" + target_.wstring() + L". Reason: " + err;
        rec_.status = OpStatus::Interrupted;
        rec_.note = errOut;
        OperationLog::Instance().Append(rec_);
        return false;
    }

    rec_.status = OpStatus::Success;
    OperationLog::Instance().Append(rec_);
    return true;
}

bool MoveJunctionOp::Undo(std::wstring& errOut) {
    if (!IsReparsePoint(source_)) {
        errOut = L"Source is not a reparse point — nothing to undo, or already undone.";
        return false;
    }
    if (!DirExists(target_)) {
        errOut = L"Target directory missing; cannot restore.";
        return false;
    }
    // 1. Remove junction at source
    std::wstring err;
    if (!RemoveDirectoryReparsePoint(source_, err)) {
        errOut = L"Failed to remove junction: " + err;
        return false;
    }
    // 2. Copy target back to source
    if (!ShCopyDirectory(target_, source_, err)) {
        errOut = L"Failed to copy back: " + err;
        return false;
    }
    // 3. Delete target
    std::wstring _; ShDeleteDirectoryPermanent(target_, _);

    OperationLog::Instance().UpdateStatus(rec_.id, OpStatus::Reverted, L"Undone via MoveJunctionOp::Undo");
    return true;
}

} // namespace minisys
