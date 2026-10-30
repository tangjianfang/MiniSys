#pragma once
#include "core/Scanner.h"

namespace minisys {

class LargeFileScanner : public Scanner {
public:
    struct Config {
        std::vector<std::filesystem::path> roots;
        std::vector<std::filesystem::path> excludes;
        unsigned long long minBytes = 100ULL * 1024 * 1024; // default 100 MiB
        size_t topN = 200;
        bool   detectDuplicates = true;
        // Extension filter: if non-empty only files whose lowercase extension is
        // in this list are reported (e.g. {L".mp4", L".mkv"}).
        std::vector<std::wstring> extFilter;
    };

    // v2.26: what kind of file a path is, for the 风险 column and for the
    // safety rules around it. Pure function of the path (nothing is read
    // from disk), so it also works on cached rows and search results.
    enum class FileClass {
        Normal,        // nothing known about it
        VcsInternal,   // inside .git / .hg / .svn — deleting ONE file corrupts the repo
        Regenerable,   // IntelliSense / precompiled-header / object caches: a rebuild recreates them
    };
    static FileClass Classify(const std::filesystem::path& p);

    LargeFileScanner();
    explicit LargeFileScanner(Config cfg);

    void Scan(std::vector<ScanItem>& out,
              ProgressFn progress,
              const std::atomic<bool>& cancel) override;

    const Config& GetConfig() const { return cfg_; }
    void SetConfig(Config c) { cfg_ = std::move(c); }

private:
    Config cfg_;
};

} // namespace minisys
