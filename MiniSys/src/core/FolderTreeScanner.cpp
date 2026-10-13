#include "core/FolderTreeScanner.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <cstdint>
#include <algorithm>

namespace fs = std::filesystem;

namespace minisys {

FolderTreeScanner::FolderTreeScanner() = default;

FolderTreeScanner::FolderTreeScanner(Config cfg) : cfg_(std::move(cfg)) {}

void FolderTreeScanner::Scan(std::vector<ScanItem>& out,
                             ProgressFn progress,
                             const std::atomic<bool>& cancel) {
    // Determine which drives to scan.
    std::vector<std::wstring> drives;
    if (cfg_.drives.empty()) {
        drives = EnumerateDrives();
    } else {
        drives = cfg_.drives;
    }

    for (const auto& drive : drives) {
        if (cancel.load()) break;

        // Enumerate top-level directories on this drive.
        std::wstring searchPath = drive;
        if (!searchPath.empty() && searchPath.back() != L'\\')
            searchPath.push_back(L'\\');
        searchPath.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(searchPath.c_str(),
                                    FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;

        std::vector<std::pair<fs::path, std::wstring>> subdirs; // (full path, name)
        do {
            if (cancel.load()) break;
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            fs::path full = fs::path(drive) / n;
            subdirs.emplace_back(full, std::wstring(n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        if (cancel.load()) break;

        // Sort alphabetically
        std::sort(subdirs.begin(), subdirs.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        // Compute sizes + emit, then recurse one level deeper per level
        // (v2.18: 层层分析). Child items carry the PARENT FOLDER PATH in
        // `category` — the presenter builds the tree from that linkage.
        std::vector<fs::path> nextLevel;
        size_t idx = 0;
        for (const auto& [dirPath, name] : subdirs) {
            if (cancel.load()) break;
            if (progress) {
                progress(idx, subdirs.size(),
                    drive + name + L"  计算大小中…");
            }

            unsigned long long sz = DirectorySizeParallel(dirPath, 2);

            ScanItem item;
            item.category  = drive;
            item.title     = name;
            item.path      = dirPath;
            item.sizeBytes = sz;
            item.detail    = drive;
            item.recommended = false;
            // REVIEW P1-9 (05-T-B5): the folder-tree channel had no mtime
            // snapshot, silently disabling the TOCTOU re-verification.
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(LongPath(dirPath).c_str(),
                                     GetFileExInfoStandard, &fad)) {
                item.lastWriteFiletime =
                    (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                    static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
                item.createTime = item.lastWriteFiletime;
            }
            out.push_back(std::move(item));
            nextLevel.push_back(dirPath);
            ++idx;
        }

        // v2.18: nested levels.
        for (int level = 1; level < cfg_.depth; ++level) {
            std::vector<fs::path> thisLevel = std::move(nextLevel);
            nextLevel.clear();
            for (const auto& parent : thisLevel) {
                if (cancel.load()) break;
                std::wstring searchPath = parent.wstring();
                if (!searchPath.empty() && searchPath.back() != L'\\')
                    searchPath.push_back(L'\\');
                searchPath.push_back(L'*');
                WIN32_FIND_DATAW fdd{};
                HANDLE hh = FindFirstFileExW(
                    LongPath(fs::path(searchPath)).c_str(),
                    FindExInfoBasic, &fdd, FindExSearchNameMatch, nullptr,
                    FIND_FIRST_EX_LARGE_FETCH);
                if (hh == INVALID_HANDLE_VALUE) continue;
                std::vector<fs::path> kids;
                do {
                    const wchar_t* n = fdd.cFileName;
                    if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
                    if (!(fdd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (fdd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    kids.push_back(parent / n);
                } while (FindNextFileW(hh, &fdd));
                FindClose(hh);
                if (progress && !kids.empty()) {
                    progress(0, 0, parent.wstring() + L" …");
                }
                for (const auto& kid : kids) {
                    if (cancel.load()) break;
                    ScanItem item;
                    item.category  = parent.wstring();   // linkage to parent node
                    item.title     = kid.filename().wstring();
                    item.path      = kid;
                    item.sizeBytes = DirectorySizeParallel(kid, 2);
                    item.detail    = parent.wstring();
                    item.recommended = false;
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (GetFileAttributesExW(LongPath(kid).c_str(),
                                             GetFileExInfoStandard, &fad)) {
                        item.lastWriteFiletime =
                            (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                            static_cast<uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
                        item.createTime = item.lastWriteFiletime;
                    }
                    out.push_back(std::move(item));
                    nextLevel.push_back(kid);
                }
            }
        }
    }

    // Sort output: by drive first, then by size descending within each drive.
    std::stable_sort(out.begin(), out.end(), [](const ScanItem& a, const ScanItem& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.sizeBytes > b.sizeBytes;
    });
}

} // namespace minisys
