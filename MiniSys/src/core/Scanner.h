#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <thread>

namespace minisys {

// A single result item produced by a Scanner.
struct ScanItem {
    std::wstring        category;     // e.g. "Browser Cache", "Video", "App: Notepad++"
    std::wstring        title;        // display title
    std::filesystem::path path;       // primary path (file or directory)
    unsigned long long  sizeBytes  = 0;
    uint64_t            createTime = 0; // Windows FILETIME (100-ns ticks since 1601-01-01)
    std::wstring        detail;       // extra info: "12345 files", "SHA256: ...", etc.
    bool                recommended = true;  // pre-checked in UI
    bool                dangerous   = false; // require extra confirmation
    std::wstring        groupKey;     // for dedup grouping
};

// Progress callback: (current, total, message). total may be 0 if unknown.
using ProgressFn = std::function<void(unsigned long long, unsigned long long, const std::wstring&)>;

class Scanner {
public:
    virtual ~Scanner() = default;

    // Run the scan synchronously on the calling thread, populating 'out'.
    // 'cancel' may become true to abort early.
    virtual void Scan(std::vector<ScanItem>& out,
                      ProgressFn progress,
                      const std::atomic<bool>& cancel) = 0;
};

} // namespace minisys
