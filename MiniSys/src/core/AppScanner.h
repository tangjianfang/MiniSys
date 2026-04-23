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
    bool isOnSystemDrive = false;
};

class AppScanner : public Scanner {
public:
    void Scan(std::vector<ScanItem>& out,
              ProgressFn progress,
              const std::atomic<bool>& cancel) override;
};

} // namespace minisys
