#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <vector>

namespace minisys {

// File entry yielded by FastWalk. Reparse points and excluded directories are
// pruned automatically (caller never sees them).
struct FastFileInfo {
    std::filesystem::path path;
    unsigned long long    sizeBytes;
    unsigned long long    lastWriteFiletime; // raw FILETIME packed into u64
    unsigned long long    createFiletime;    // raw FILETIME packed into u64
};

// Multi-threaded directory walker built on FindFirstFileExW.
//
// Safety guarantees:
//   * Never crosses reparse points (junctions / symlinks). Reparse-point dirs
//     are skipped entirely.
//   * Never recurses into any directory whose lowercased path starts with a
//     prefix in 'excludesLower'. Match is exact prefix, no glob.
//   * onFile is called concurrently from worker threads — caller must handle
//     synchronization.
//   * Cancellation honored at directory boundaries.
//
// numThreads<=0 means auto (min(8, hardware_concurrency)).
void FastWalk(const std::vector<std::filesystem::path>& roots,
              const std::vector<std::wstring>&         excludesLower,
              const std::function<void(const FastFileInfo&)>& onFile,
              const std::atomic<bool>& cancel,
              int numThreads = 0,
              const std::function<void(unsigned long long visitedFiles,
                                       const std::wstring& currentDir)>& onProgress = nullptr);

} // namespace minisys
