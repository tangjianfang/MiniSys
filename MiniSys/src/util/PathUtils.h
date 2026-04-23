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

// Whether a directory entry is a reparse point (junction / symlink).
bool IsReparsePoint(const std::filesystem::path& p);

// Compute total size of a directory subtree (skips reparse points and unreadable entries).
unsigned long long DirectorySize(const std::filesystem::path& p);

} // namespace minisys
