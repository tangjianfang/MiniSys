#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace minisys {

// Paths inside %LOCALAPPDATA%/MiniSys/
std::filesystem::path AppDataDir();
std::filesystem::path LogsDir();
std::filesystem::path HistoryDir();

// Add the \\?\ long-path prefix if absent and path is absolute.
std::wstring LongPath(const std::filesystem::path& p);

// Disk free / total bytes for the volume containing 'driveRoot' (e.g. "C:/").
struct DiskSpace {
    unsigned long long totalBytes = 0;
    unsigned long long freeBytes  = 0;
};
bool QueryDiskSpace(const std::wstring& driveRoot, DiskSpace& out);

// "C:/" for path "C:/Foo/bar"
std::wstring DriveRootOf(const std::filesystem::path& p);

// Enumerate logical drive roots available on this system (e.g. {"C:\\", "D:\\"}).
std::vector<std::wstring> EnumerateDrives();

// v2.25 (configurable scan drives): "C;D" / "c:, d:\" -> {"C:\\", "D:\\"}.
// Separators ; , and space; a token is a drive letter optionally followed by
// ':' and '\'. Upper-cased, de-duplicated, order kept; anything else dropped.
std::vector<std::wstring> ParseDriveList(const std::wstring& text);
// Inverse of ParseDriveList: {"C:\\", "D:\\"} -> "C;D".
std::wstring FormatDriveList(const std::vector<std::wstring>& roots);
// Whether `p` lives on one of `roots` (drive roots as from ParseDriveList).
bool IsOnAnyDrive(const std::filesystem::path& p,
                  const std::vector<std::wstring>& roots);

// Whether path is on system drive (the drive of %SystemRoot%).
bool IsOnSystemDrive(const std::filesystem::path& p);

// Path of system drive root, e.g. "C:/".
std::wstring SystemDriveRoot();

// Current user profile directory (e.g. C:/Users/Name).
std::filesystem::path UserProfileDir();

// Recycle Bin query / empty (whole system).
struct RecycleBinInfo { unsigned long long sizeBytes = 0; unsigned long long itemCount = 0; };
bool QueryRecycleBin(RecycleBinInfo& out);
bool EmptyRecycleBinAll(); // shows no UI, no sound, no confirmation

// Existence checks.
bool FileExists(const std::filesystem::path& p);
bool DirExists(const std::filesystem::path& p);

// r7-04 U-1: best-effort recursive deletion that survives Win32-illegal
// leaf names (trailing dots/spaces — plain DeleteFileW gets err 2 on them,
// and fs::remove_all then reports the WHOLE root as failed while leaving
// everything else deleted). Enumerates with FindFirstFileW and deletes
// through \\?\-prefixed paths; never recurses into reparse points (same
// safety property as fs::remove_all — verified r7-05). Returns the number
// of entries that could not be deleted; their \\?\-prefixed paths land in
// *survivors (optional). A root that does not exist is a no-op (0).
size_t RemoveAllLongPath(const std::wstring& root,
                         std::vector<std::wstring>* survivors = nullptr);

// Whether a directory entry is a reparse point (junction / symlink).
bool IsReparsePoint(const std::filesystem::path& p);

// Compute total size of a directory subtree (skips reparse points and unreadable entries).
unsigned long long DirectorySize(const std::filesystem::path& p);

// REVIEW P1-3 (03-B11): parallel variant — a directory-queue worker pool
// (same shape as FastWalk). The single-threaded DFS made WinSxS (300k
// entries) alone cost tens of seconds during every junk scan.
unsigned long long DirectorySizeParallel(const std::filesystem::path& p,
                                         int numThreads = 0);

} // namespace minisys
