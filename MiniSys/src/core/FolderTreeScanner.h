#pragma once
#include "core/Scanner.h"

namespace minisys {

// Scans top-level folders of each fixed drive and reports their sizes.
// Each ScanItem produced has:
//   category  = drive root  (e.g. L"C:\\")
//   title     = folder name (e.g. L"Users")
//   path      = full path   (e.g. "C:\\Users")
//   sizeBytes = recursive size of the folder
class FolderTreeScanner : public Scanner {
public:
    struct Config {
        // Drive roots to scan (e.g. {"C:\\", "D:\\"}). Empty = all fixed drives.
        std::vector<std::wstring> drives;
    };

    FolderTreeScanner();
    explicit FolderTreeScanner(Config cfg);

    void Scan(std::vector<ScanItem>& out,
              ProgressFn progress,
              const std::atomic<bool>& cancel) override;

    const Config& GetConfig() const { return cfg_; }
    void SetConfig(Config c) { cfg_ = std::move(c); }

private:
    Config cfg_;
};

} // namespace minisys
