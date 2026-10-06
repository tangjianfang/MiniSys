#pragma once
#include <string>
#include <vector>

namespace minisys {

// REVIEW P2 (08-F11/F9): persisted user settings — everything used to reset
// on every launch (migration target, symlink mode, large-file filters) and
// the exclusion list ("永不清理此文件夹") had nowhere to live.
struct Settings {
    std::wstring migrateTargetRoot;
    bool useSymlink = false;
    int largeFilesMinMB = 100;
    std::wstring largeFilesExtFilter;   // ".mp4;.mkv"
    std::wstring largeFilesDrives;      // "C;D"
    std::vector<std::wstring> exclusions;   // user-protected paths

    static Settings Load();                    // %LOCALAPPDATA%\MiniSys\settings.json
    void Save() const;                         // atomic (temp + rename)
    static std::wstring SettingsPath();
};

} // namespace minisys
