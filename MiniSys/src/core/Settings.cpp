#include "core/Settings.h"

#include "util/Json.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <fstream>
#include <iterator>

namespace minisys {

std::wstring Settings::SettingsPath() {
    return (AppDataDir() / L"settings.json").wstring();
}

Settings Settings::Load() {
    Settings s;
    std::error_code ec;
    auto path = std::filesystem::path(SettingsPath());
    if (!std::filesystem::exists(path, ec)) return s;

    std::ifstream f(path, std::ios::binary);
    if (!f) return s;
    std::string utf8((std::istreambuf_iterator<char>(f)), {});
    Json j;
    std::wstring err;
    if (!Json::Parse(Utf8ToWide(utf8), j, err) || !j.IsObject()) {
        MS_LOG_WARN(L"settings.json parse failed: %s — using defaults", err.c_str());
        return s;
    }
    s.migrateTargetRoot = j.Get(L"migrateTargetRoot").AsString();
    s.useSymlink = j.Get(L"useSymlink").AsBool(false);
    s.largeFilesMinMB = static_cast<int>(j.Get(L"largeFilesMinMB").AsInt(100));
    s.largeFilesExtFilter = j.Get(L"largeFilesExtFilter").AsString();
    s.largeFilesDrives = j.Get(L"largeFilesDrives").AsString();
    const Json& ex = j.Get(L"exclusions");
    if (ex.IsArray()) {
        for (size_t i = 0; i < ex.Size(); ++i) {
            auto v = ex.At(i).AsString();
            if (!v.empty()) s.exclusions.push_back(std::move(v));
        }
    }
    return s;
}

void Settings::Save() const {
    Json j = Json::Object();
    j.Set(L"migrateTargetRoot", Json(migrateTargetRoot));
    j.Set(L"useSymlink", Json(useSymlink));
    j.Set(L"largeFilesMinMB", Json(static_cast<double>(largeFilesMinMB)));
    j.Set(L"largeFilesExtFilter", Json(largeFilesExtFilter));
    j.Set(L"largeFilesDrives", Json(largeFilesDrives));
    Json arr = Json::Array();
    for (const auto& e : exclusions) arr.Push(Json(e));
    j.Set(L"exclusions", arr);

    auto path = std::filesystem::path(SettingsPath());
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            MS_LOG_WARN(L"Failed to write settings.json");
            return;
        }
        f << WideToUtf8(j.Dump()) << "\n";
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

} // namespace minisys
