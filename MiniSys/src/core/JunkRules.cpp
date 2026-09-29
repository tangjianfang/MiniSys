#include "core/JunkRules.h"

#include "util/Json.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <fstream>
#include <iterator>

namespace minisys {

namespace {

std::vector<JunkRule> BuiltinRules() {
    // Mirrors rules.json (fallback when the external file is missing/broken).
    // Keep in sync with MiniSys/rules.json.
    auto R = [](std::wstring id, std::wstring cat, std::wstring title,
                std::wstring path, RuleMode mode, std::wstring pattern,
                RiskLevel risk, int ageDays, CleanStrategy strat,
                bool recommended, std::wstring cmd = {},
                std::wstring hint = {}) {
        JunkRule r;
        r.id = std::move(id); r.category = std::move(cat);
        r.title = std::move(title); r.path = std::move(path);
        r.mode = mode; r.childPattern = std::move(pattern);
        r.riskLevel = risk; r.minAgeDays = ageDays;
        r.strategy = strat; r.recommended = recommended;
        r.command = std::move(cmd); r.detailHint = std::move(hint);
        return r;
    };
    using RL = RiskLevel; using CS = CleanStrategy; using RM = RuleMode;
    return {
        // ---- Safe: quarantine, pre-checked ----
        R(L"user-temp",  L"System Temp", L"用户临时文件夹", L"%TEMP%",
          RM::Subtree, L"", RL::Safe, 0, CS::Quarantine, true),
        R(L"windows-temp", L"System Temp", L"Windows 临时文件夹", L"%SystemRoot%\\Temp",
          RM::Subtree, L"", RL::Safe, 0, CS::Quarantine, true),
        R(L"thumbnails", L"Explorer", L"缩略图缓存", L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer",
          RM::Children, L"thumbcache_*.db", RL::Safe, 0, CS::Quarantine, true),
        R(L"d3dscache", L"Explorer", L"D3D 着色器缓存", L"%LOCALAPPDATA%\\D3DSCache",
          RM::Subtree, L"", RL::Safe, 0, CS::Quarantine, true),
        R(L"wer", L"Explorer", L"错误报告 (WER)", L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER",
          RM::Subtree, L"", RL::Safe, 0, CS::Quarantine, true),
        R(L"font-cache", L"Explorer", L"字体缓存", L"%SystemDrive%\\Windows\\ServiceProfiles\\LocalService\\AppData\\Local\\FontCache",
          RM::Subtree, L"", RL::Safe, 0, CS::Quarantine, true),
        R(L"edge-profiles", L"Browser Cache", L"Edge 缓存 (所有 Profile)",
          L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data",
          RM::Profiles, L"Cache;Code Cache;GPUCache", RL::Safe, 0, CS::Quarantine, true),
        R(L"chrome-profiles", L"Browser Cache", L"Chrome 缓存 (所有 Profile)",
          L"%LOCALAPPDATA%\\Google\\Chrome\\User Data",
          RM::Profiles, L"Cache;Code Cache;GPUCache", RL::Safe, 0, CS::Quarantine, true),
        R(L"firefox-profiles", L"Browser Cache", L"Firefox 缓存 (所有 Profile)",
          L"%LOCALAPPDATA%\\Mozilla\\Firefox\\Profiles",
          RM::Profiles, L"cache2", RL::Safe, 0, CS::Quarantine, true),
        R(L"dev-npm",   L"Dev Cache", L"npm 缓存",   L"%LOCALAPPDATA%\\npm-cache",
          RM::Subtree, L"", RL::Safe, 30, CS::Quarantine, true),
        R(L"dev-pip",   L"Dev Cache", L"pip 缓存",   L"%LOCALAPPDATA%\\pip\\cache",
          RM::Subtree, L"", RL::Safe, 30, CS::Quarantine, true),
        R(L"dev-cargo", L"Dev Cache", L"cargo 缓存", L"%USERPROFILE%\\.cargo\\registry",
          RM::Subtree, L"", RL::Safe, 30, CS::Quarantine, true),
        R(L"dev-maven", L"Dev Cache", L"Maven 仓库", L"%USERPROFILE%\\.m2\\repository",
          RM::Subtree, L"", RL::Safe, 30, CS::Quarantine, true),
        R(L"dev-gradle",L"Dev Cache", L"Gradle 缓存", L"%USERPROFILE%\\.gradle\\caches",
          RM::Subtree, L"", RL::Safe, 30, CS::Quarantine, true),
        R(L"dev-nuget", L"Dev Cache", L"NuGet 包缓存", L"%USERPROFILE%\\.nuget\\packages",
          RM::Subtree, L"", RL::Safe, 30, CS::Quarantine, true),

        // ---- Cautious: quarantine, not pre-checked ----
        R(L"wu-download", L"Windows Update", L"更新下载缓存",
          L"%SystemRoot%\\SoftwareDistribution\\Download",
          RM::Subtree, L"", RL::Cautious, 0, CS::Quarantine, false),
        R(L"cbs-logs", L"Windows Logs", L"CBS 日志", L"%SystemRoot%\\Logs\\CBS",
          RM::Subtree, L"", RL::Cautious, 0, CS::Quarantine, false),
        R(L"dism-logs", L"Windows Logs", L"DISM 日志", L"%SystemRoot%\\Logs\\DISM",
          RM::Subtree, L"", RL::Cautious, 0, CS::Quarantine, false),
        R(L"minidump", L"Windows Logs", L"蓝屏转储 (Minidump)", L"%SystemRoot%\\Minidump",
          RM::Subtree, L"", RL::Cautious, 0, CS::Quarantine, false),
        R(L"do-cache", L"Windows Update", L"传递优化缓存",
          L"%ProgramData%\\Microsoft\\Windows\\DeliveryOptimization\\Cache",
          RM::Subtree, L"", RL::Cautious, 0, CS::Quarantine, false),
        R(L"wechat-files", L"Chat Files", L"微信接收文件 (按账号)",
          L"%USERPROFILE%\\Documents\\WeChat Files",
          RM::Children, L"", RL::Cautious, 30, CS::Quarantine, false),
        R(L"qq-files", L"Chat Files", L"QQ 接收文件 (按账号)",
          L"%USERPROFILE%\\Documents\\Tencent Files",
          RM::Children, L"", RL::Cautious, 30, CS::Quarantine, false),

        // ---- Advanced: delegate to system commands, never hand-delete ----
        R(L"winsxs", L"System Component", L"组件存储 (WinSxS)",
          L"%SystemRoot%\\WinSxS",
          RM::Subtree, L"", RL::Advanced, 0, CS::Delegate, false,
          L"Dism.exe /Online /Cleanup-Image /StartComponentCleanup",
          L"仅可通过 DISM 组件清理，不可直接删除"),
        R(L"windows-old", L"System Component", L"旧系统备份 (Windows.old)",
          L"%SystemDrive%\\Windows.old",
          RM::Subtree, L"", RL::Advanced, 0, CS::Delegate, false, L"cleanmgr",
          L"建议通过磁盘清理工具处理"),
        R(L"hiberfil", L"System Component", L"休眠文件 hiberfil.sys",
          L"%SystemDrive%\\hiberfil.sys",
          RM::Subtree, L"", RL::Advanced, 0, CS::Delegate, false,
          L"powercfg /h off", L"关闭休眠后自动删除"),

        // ---- Info-only: display + guidance ----
        R(L"pagefile", L"System Reserved", L"页面文件 pagefile.sys",
          L"%SystemDrive%\\pagefile.sys",
          RM::Subtree, L"", RL::InfoOnly, 0, CS::InfoOnly, false, L"",
          L"通过 系统属性→高级→虚拟内存 调整"),
        R(L"swapfile", L"System Reserved", L"交换文件 swapfile.sys",
          L"%SystemDrive%\\swapfile.sys",
          RM::Subtree, L"", RL::InfoOnly, 0, CS::InfoOnly, false, L"",
          L"系统管理，不可直接删除"),
    };
}

std::filesystem::path RulesFilePath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path() / L"rules.json";
}

std::vector<JunkRule> ParseRulesJson(const Json& root, bool& ok) {
    ok = false;
    std::vector<JunkRule> rules;
    if (!root.IsObject()) return rules;
    const Json& arr = root.Get(L"rules");
    if (!arr.IsArray()) return rules;
    for (size_t i = 0; i < arr.Size(); ++i) {
        const Json& jr = arr.At(i);
        if (!jr.IsObject()) continue;
        JunkRule r;
        r.id       = jr.Get(L"id").AsString();
        r.category = jr.Get(L"category").AsString();
        r.title    = jr.Get(L"title").AsString();
        r.path     = jr.Get(L"path").AsString();
        if (r.id.empty() || r.path.empty()) continue;   // required fields
        r.mode         = JunkRules::ParseMode(jr.Get(L"mode").AsString(L"subtree"));
        r.childPattern = jr.Get(L"childPattern").AsString();
        r.profileNames = jr.Get(L"profileNames").AsString();
        r.riskLevel    = JunkRules::ParseRiskLevel(jr.Get(L"riskLevel").AsString(L"cautious"));
        r.minAgeDays   = static_cast<int>(jr.Get(L"minAgeDays").AsInt(0));
        r.strategy     = JunkRules::ParseStrategy(jr.Get(L"strategy").AsString(L"quarantine"));
        r.recommended  = jr.Get(L"recommended").AsBool(
                             r.riskLevel == RiskLevel::Safe);
        r.command      = jr.Get(L"command").AsString();
        r.detailHint   = jr.Get(L"detailHint").AsString();
        rules.push_back(std::move(r));
    }
    ok = !rules.empty();
    return rules;
}

} // namespace

namespace JunkRules {

std::vector<JunkRule> Load() {
    auto path = RulesFilePath();
    if (!path.empty() && FileExists(path)) {
        // Read file as UTF-8.
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::string utf8((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            auto text = Utf8ToWide(utf8);
            Json root;
            std::wstring err;
            if (Json::Parse(text, root, err)) {
                bool ok = false;
                auto rules = ParseRulesJson(root, ok);
                if (ok) return rules;
                MS_LOG_WARN(L"rules.json parsed but empty/invalid rules array");
            } else {
                MS_LOG_WARN(L"rules.json parse failed: %s", err.c_str());
            }
        }
    }
    return BuiltinRules();
}

std::wstring ExpandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, MAX_PATH * 4);
    if (n == 0 || n > MAX_PATH * 4) return {};
    return buf;
}

bool MatchWildcard(const std::wstring& pattern, const std::wstring& name) {
    // Iterative '*' backtracking matcher, case-insensitive.
    size_t p = 0, i = 0, starP = std::wstring::npos, starI = 0;
    auto eq = [](wchar_t a, wchar_t b) { return ::towlower(a) == ::towlower(b); };
    while (i < name.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || eq(pattern[p], name[i]))) {
            ++p; ++i;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            starP = p++; starI = i;
        } else if (starP != std::wstring::npos) {
            p = starP + 1; i = ++starI;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

RiskLevel ParseRiskLevel(const std::wstring& s) {
    if (IEquals(s, L"safe"))      return RiskLevel::Safe;
    if (IEquals(s, L"advanced"))  return RiskLevel::Advanced;
    if (IEquals(s, L"info"))      return RiskLevel::InfoOnly;
    return RiskLevel::Cautious;
}

CleanStrategy ParseStrategy(const std::wstring& s) {
    if (IEquals(s, L"delegate")) return CleanStrategy::Delegate;
    if (IEquals(s, L"info"))     return CleanStrategy::InfoOnly;
    return CleanStrategy::Quarantine;
}

RuleMode ParseMode(const std::wstring& s) {
    if (IEquals(s, L"children")) return RuleMode::Children;
    if (IEquals(s, L"profiles")) return RuleMode::Profiles;
    return RuleMode::Subtree;
}

} // namespace JunkRules

} // namespace minisys
