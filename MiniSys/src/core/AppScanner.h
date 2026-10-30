#pragma once
#include "core/Scanner.h"

namespace minisys {

// Records about installed apps from the Uninstall registry hives.
struct AppInfo {
    std::wstring displayName;
    std::wstring publisher;
    std::wstring installLocation;
    std::wstring uninstallString;
    unsigned long long sizeBytesEstimated = 0; // EstimatedSize * 1024
    unsigned long long sizeBytesActual    = 0; // computed via DirectorySize when possible
    bool isUWP = false;
    bool isOnScanDrive = false;   // install location is on a selected drive
};

class AppScanner : public Scanner {
public:
    struct Config {
        // v2.25: drive roots whose apps are listed (e.g. {"C:\\", "D:\\"}).
        // Empty = the system drive only (the pre-v2.25 behaviour: the tab
        // exists to move apps OFF the system drive).
        std::vector<std::wstring> drives;
    };

    AppScanner() = default;
    explicit AppScanner(Config cfg) : cfg_(std::move(cfg)) {}

    const Config& GetConfig() const { return cfg_; }

    void Scan(std::vector<ScanItem>& out,
              ProgressFn progress,
              const std::atomic<bool>& cancel) override;

private:
    Config cfg_;
};

} // namespace minisys
