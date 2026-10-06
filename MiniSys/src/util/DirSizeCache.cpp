#include "util/DirSizeCache.h"

#include "util/PathUtils.h"
#include "util/StringUtils.h"

namespace minisys {

DirSizeCache& DirSizeCache::Instance() {
    static DirSizeCache inst;
    return inst;
}

unsigned long long DirSizeCache::SizeOf(const std::filesystem::path& dir,
                                        uint64_t dirMtime, int threads) {
    std::wstring key = ToLower(dir.wstring());

    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = map_.find(key);
        if (it != map_.end() && it->second.first == dirMtime) {
            return it->second.second;
        }
    }

    unsigned long long size = DirectorySizeParallel(dir, threads);
    if (size > 0) {
        std::lock_guard<std::mutex> g(mu_);
        map_[key] = { dirMtime, size };
    }
    return size;
}

} // namespace minisys
