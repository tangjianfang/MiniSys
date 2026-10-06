#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace minisys {

// v2.7: session-wide directory-size cache, keyed by (lowercased path,
// directory mtime). Repeat scans reuse the recorded subtree size when the
// directory's own mtime is unchanged — the dominant rescan cost is the
// parallel subtree sizing pass, so this turns a "scan again after deleting
// a few things" from ~30 s into seconds.
//
// Soundness: a directory's mtime changes whenever a DIRECT child is
// created/deleted/renamed; deep (grandchild-level) modifications leave it
// untouched and can overstate a cached size. Sizes are display-only —
// execution re-verifies each item and the plan hash — so the tradeoff is
// accepted; a full rescan always recomputes changed directories.
class DirSizeCache {
public:
    static DirSizeCache& Instance();

    // Cached size when (path, mtime) matches a previous computation;
    // otherwise runs DirectorySizeParallel and records the result.
    // Computed sizes of 0 (failed/empty reads) are not cached.
    unsigned long long SizeOf(const std::filesystem::path& dir,
                              uint64_t dirMtime, int threads);

private:
    DirSizeCache() = default;
    std::mutex mu_;
    // lowercased path → { directory mtime, subtree size }
    std::map<std::wstring, std::pair<uint64_t, unsigned long long>> map_;
};

} // namespace minisys
