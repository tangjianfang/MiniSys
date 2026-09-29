#include "core/GuardRails.h"

#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_DATA_OPEN 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_OFFLINE
#define FILE_ATTRIBUTE_OFFLINE 0x00001000
#endif

namespace fs = std::filesystem;

namespace minisys {

namespace {

const std::vector<fs::path>& ProtectedPaths() {
    static const std::vector<fs::path> v = []{
        auto sd = SystemDriveRoot();
        auto win = fs::path(sd) / L"Windows";
        return std::vector<fs::path>{
            win,
            fs::path(sd) / L"Program Files" / L"WindowsApps",
            fs::path(sd) / L"ProgramData" / L"Microsoft",
            fs::path(sd) / L"$Recycle.Bin",
            fs::path(sd) / L"System Volume Information",
            fs::path(sd) / L"$WinREAgent",
            fs::path(sd) / L"Recovery",
            // Quarantine areas are managed only through their own flows.
            fs::path(sd) / L"MiniSys.Quarantine",
            fs::path(L"D:\\") / L"MiniSys.Quarantine",
            fs::path(L"E:\\") / L"MiniSys.Quarantine",
        };
    }();
    return v;
}

bool IsUnderProtected(const fs::path& p) {
    // Compare the plain path — LongPath's \\?\ prefix would break the
    // prefix match (caught by QuarantineOpTests.QuarantineRootIsProtectedFromPlans).
    auto sp = ToLower(p.wstring());
    for (auto& prot : ProtectedPaths()) {
        auto sx = ToLower(prot.wstring());
        if (sp.size() < sx.size()) continue;
        if (sp.compare(0, sx.size(), sx) != 0) continue;
        if (sp.size() == sx.size()) return true;
        wchar_t next = sp[sx.size()];
        if (next == L'\\' || next == L'/') return true;
    }
    return false;
}

// FILETIME -> seconds since 1970 (rough, only for reporting).
bool GetFileTimes(const fs::path& p, unsigned long long& sizeOut,
                  unsigned long long& mtimeOut) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    sizeOut = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
              static_cast<unsigned long long>(fad.nFileSizeLow);
    mtimeOut = (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
               static_cast<unsigned long long>(fad.ftLastWriteTime.dwLowDateTime);
    return true;
}

} // namespace

GuardRails& GuardRails::Instance() {
    static GuardRails inst;
    return inst;
}

bool GuardRails::IsProtectedPath(const fs::path& p) {
    return IsUnderProtected(p);
}

DWORD GuardRails::FileAttributesOf(const fs::path& p) {
    return GetFileAttributesW(LongPath(p).c_str());
}

GuardRails::Verdict GuardRails::Validate(const PlanItem& pi,
                                         const ScanItem& si) const {
    Verdict v;

    // 1. Protected locations (System32, Windows, quarantine, ...).
    if (IsUnderProtected(pi.path)) {
        v.allow = false;
        v.reason = L"受保护系统路径，拒绝操作";
        return v;
    }

    // 2. Strategy/risk constraints: InfoOnly items are display-only and
    //    Advanced items are delegate-only (WinSxS & co., ADR-008) — neither
    //    may be executed as a direct file operation. Delegate commands get
    //    their own path with M3's DelegateOp.
    if (si.riskLevel == RiskLevel::InfoOnly) {
        v.allow = false;
        v.reason = L"仅提示项，不可执行（请参考详情中的指引）";
        return v;
    }
    if (si.riskLevel == RiskLevel::Advanced) {
        v.allow = false;
        v.reason = L"该级别项目仅支持系统命令委派，不可直接删除";
        return v;
    }

    // 3. Attributes: exists, not a reparse point, not a cloud placeholder.
    DWORD attr = FileAttributesOf(pi.path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        v.allow = false;
        v.reason = L"路径已不存在（可能已被处理）";
        return v;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        v.allow = false;
        v.reason = L"路径是 Junction/符号链接，拒绝操作";
        return v;
    }
    if (attr & (FILE_ATTRIBUTE_RECALL_ON_DATA_OPEN | FILE_ATTRIBUTE_OFFLINE)) {
        v.allow = false;
        v.reason = L"云文件占位符（OneDrive 等），拒绝操作";
        return v;
    }

    // 4. TOCTOU re-verify against the scan snapshot.
    //    - Files: size and mtime must match.
    //    - Directories: the directory entry's own size says nothing about the
    //      subtree, so only mtime (direct children added/removed) is checked.
    unsigned long long sizeNow = 0, mtimeNow = 0;
    if (!GetFileTimes(pi.path, sizeNow, mtimeNow)) {
        v.allow = false;
        v.reason = L"无法读取文件属性";
        return v;
    }
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!isDir && pi.sizeAtScan != 0 && sizeNow != pi.sizeAtScan) {
        v.allow = false;
        v.reason = FormatW(L"扫描后大小已变化（%s → %s）",
                           FormatSize(pi.sizeAtScan).c_str(),
                           FormatSize(sizeNow).c_str());
        return v;
    }
    if (pi.lastWriteAtScan != 0 && mtimeNow != pi.lastWriteAtScan) {
        v.allow = false;
        v.reason = L"扫描后文件已被修改";
        return v;
    }
    return v;
}

} // namespace minisys
