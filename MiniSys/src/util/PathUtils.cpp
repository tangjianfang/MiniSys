#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace minisys {

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        r = path;
    }
    if (path) CoTaskMemFree(path);
    return r;
}

fs::path AppDataDir() {
    auto p = KnownFolder(FOLDERID_LocalAppData) / L"MiniSys";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path LogsDir() {
    auto p = AppDataDir() / L"logs";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path HistoryDir() {
    auto p = AppDataDir() / L"history";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::wstring LongPath(const fs::path& p) {
    auto s = p.wstring();
    if (s.size() < 2) return s;
    if (s.rfind(LR"(\\?\)", 0) == 0) return s;
    if (s.rfind(LR"(\\)", 0) == 0)  return LR"(\\?\UNC\)" + s.substr(2);
    if (s.size() >= 2 && s[1] == L':') return LR"(\\?\)" + s;
    return s;
}

bool QueryDiskSpace(const std::wstring& driveRoot, DiskSpace& out) {
    ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
    if (!GetDiskFreeSpaceExW(driveRoot.c_str(), &freeAvail, &total, &freeTotal)) return false;
    out.totalBytes = total.QuadPart;
    out.freeBytes  = freeAvail.QuadPart;
    return true;
}

std::wstring DriveRootOf(const fs::path& p) {
    auto s = p.wstring();
    if (s.size() >= 2 && s[1] == L':') return std::wstring{s[0], L':', L'\\'};
    return {};
}

std::wstring SystemDriveRoot() {
    wchar_t buf[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    if (n >= 3 && buf[1] == L':') return std::wstring{buf[0], L':', L'\\'};
    return L"C:\\";
}

std::vector<std::wstring> EnumerateDrives() {
    std::vector<std::wstring> result;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = L'A' + static_cast<wchar_t>(i);
        std::wstring root = {letter, L':', L'\\'};
        UINT type = GetDriveTypeW(root.c_str());
        // Only include fixed drives (local disks)
        if (type == DRIVE_FIXED) {
            result.push_back(root);
        }
    }
    return result;
}

bool IsOnSystemDrive(const fs::path& p) {
    auto root = DriveRootOf(p);
    return !root.empty() && IEquals(root, SystemDriveRoot());
}

fs::path UserProfileDir() {
    return KnownFolder(FOLDERID_Profile);
}

bool QueryRecycleBin(RecycleBinInfo& out) {
    SHQUERYRBINFO info{ sizeof(info), 0, 0 };
    HRESULT hr = SHQueryRecycleBinW(nullptr, &info);
    if (FAILED(hr)) return false;
    out.sizeBytes = static_cast<unsigned long long>(info.i64Size);
    out.itemCount = static_cast<unsigned long long>(info.i64NumItems);
    return true;
}

bool EmptyRecycleBinAll() {
    HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr,
        SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    return SUCCEEDED(hr);
}

bool FileExists(const fs::path& p) {
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const fs::path& p) {
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsReparsePoint(const fs::path& p) {
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

unsigned long long DirectorySize(const fs::path& p) {
    // Iterative DFS using FindFirstFileExW (basic info + large fetch).
    // Skips reparse points (junctions/symlinks) so we never double-count or escape volume.
    unsigned long long total = 0;
    std::vector<fs::path> stack;
    stack.reserve(64);
    stack.push_back(p);
    WIN32_FIND_DATAW fd{};
    while (!stack.empty()) {
        fs::path dir = std::move(stack.back());
        stack.pop_back();
        std::wstring search = LongPath(dir);
        if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
            search.push_back(L'\\');
        search.push_back(L'*');
        HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            DWORD attr = fd.dwFileAttributes;
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(dir / n);
            } else {
                total += (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32)
                       | static_cast<unsigned long long>(fd.nFileSizeLow);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return total;
}

} // namespace minisys
