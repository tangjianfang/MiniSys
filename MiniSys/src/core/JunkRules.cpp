#include "core/JunkRules.h"

#include "core/DelegateOp.h"
#include "core/GuardRails.h"
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
          L"C:\\Windows\\System32\\Dism.exe /Online /Cleanup-Image /StartComponentCleanup",
          L"仅可通过 DISM 组件清理，不可直接删除"),
        R(L"windows-old", L"System Component", L"旧系统备份 (Windows.old)",
          L"%SystemDrive%\\Windows.old",
          RM::Subtree, L"", RL::Advanced, 0, CS::Delegate, false, L"C:\\Windows\\System32\\cleanmgr.exe /VERYLOWDISK /d C:",
          L"建议通过磁盘清理工具处理"),
        R(L"hiberfil", L"System Component", L"休眠文件 hiberfil.sys",
          L"%SystemDrive%\\hiberfil.sys",
          RM::Subtree, L"", RL::Advanced, 0, CS::Delegate, false,
          L"C:\\Windows\\System32\\powercfg.exe /h off", L"关闭休眠后自动删除"),

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

namespace {

LoadResult g_lastLoad;

// Whitelist for DelegateOp commands (REVIEW P0-2 / 05-T-B4): commands come
// from a user-editable file and run in an elevated process — only absolute
// paths to system executables with a fixed argument prefix are allowed.
bool DelegateCommandAllowedImpl(const std::wstring& cmd, std::wstring& reason) {
    struct WhitelistEntry {
        const wchar_t* exe;
        const wchar_t* const* prefixes;
        size_t prefixCount;
    };
    static const wchar_t* kDism[] = { L"/Online /Cleanup-Image" };
    static const wchar_t* kPowercfg[] = { L"/h ", L"-h ", L"/hibernate " };
    static const wchar_t* kCleanmgr[] = { L"", L"/VERYLOWDISK", L"/LOWDISK",
                                          L"/AUTOCLEAN", L"/SAGERUN" };
    static const WhitelistEntry kEntries[] = {
        { L"c:\\windows\\system32\\dism.exe",    kDism,     1 },
        { L"c:\\windows\\system32\\powercfg.exe", kPowercfg, 3 },
        { L"c:\\windows\\system32\\cleanmgr.exe", kCleanmgr, 5 },
    };

    std::wstring exe, args;
    if (!DelegateOp::SplitCommandLine(cmd, exe, args)) {
        reason = L"无效的委派命令行";
        return false;
    }
    // Resolve to an absolute path — a bare "Dism.exe" resolves via CWD/PATH
    // and can be hijacked; only the absolute system location is accepted.
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetFullPathNameW(exe.c_str(), MAX_PATH * 2, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) {
        reason = L"委派命令路径无法解析: " + exe;
        return false;
    }
    std::wstring abs = ToLower(buf);
    std::wstring argsLow = ToLower(args);
    for (const auto& e : kEntries) {
        if (abs != e.exe) continue;
        for (size_t i = 0; i < e.prefixCount; ++i) {
            std::wstring p = ToLower(e.prefixes[i]);
            // v2.12 (adversarial corpus): the prefix must end at a token
            // boundary — "/Cleanup-Image\ncmd /c calc" used to pass as a
            // prefix of "/Online /Cleanup-Image". A prefix that itself ends
            // in whitespace ("/h ") already consumed its separator.
            bool atBoundary = p.empty() || p.back() == L' ' || p.back() == L'\t' ||
                              argsLow.size() == p.size() ||
                              argsLow[p.size()] == L' ' ||
                              argsLow[p.size()] == L'\t';
            bool ok = p.empty() ? argsLow.empty()
                                : (argsLow.size() >= p.size() &&
                                   argsLow.compare(0, p.size(), p) == 0 &&
                                   atBoundary);
            if (ok) {
                return true;
            }
        }
        reason = L"委派命令参数不在白名单: " + args;
        return false;
    }
    reason = L"委派命令不在白名单: " + exe;
    return false;
}

// Per-rule load-time validation (REVIEW P0-2 / 05-T-B3).
bool ValidateRule(const JunkRule& r, std::wstring& why) {
    if (r.strategy == CleanStrategy::Delegate) {
        if (r.command.empty()) {
            why = L"委派规则缺少 command";
            return false;
        }
        return DelegateCommandAllowedImpl(r.command, why);
    }
    if (r.strategy != CleanStrategy::Quarantine) return true;  // InfoOnly: display only
    auto expanded = ExpandEnv(r.path);
    if (expanded.empty()) {
        why = L"路径为空或环境变量展开失败";
        return false;
    }
    // Quarantine rules may target user space or EXACTLY the built-in
    // exempted system cleanup roots — never new system-area subtrees.
    if (GuardRails::IsProtectedPath(expanded) &&
        !GuardRails::IsExemptSystemCleanupRoot(expanded)) {
        why = L"路径位于受保护系统区域";
        return false;
    }
    return true;
}

} // namespace

const LoadResult& LastLoad() {
    return g_lastLoad;
}

bool DelegateCommandAllowed(const std::wstring& commandLine,
                            std::wstring& reason) {
    return DelegateCommandAllowedImpl(commandLine, reason);
}

LoadResult LoadValidated() {
    LoadResult result;

    auto validateInto = [&](std::vector<JunkRule> rules, bool external) {
        for (auto& r : rules) {
            std::wstring why;
            if (!ValidateRule(r, why)) {
                result.rejected.emplace_back(r.id, why);
                MS_LOG_WARN(L"JunkRules: 拒绝规则 %s: %s", r.id.c_str(), why.c_str());
                continue;
            }
            result.rules.push_back(std::move(r));
        }
        result.usedExternal = external;
    };

    auto path = RulesFilePath();
    if (!path.empty() && FileExists(path)) {
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
                if (ok) {
                    validateInto(std::move(rules), true);
                    g_lastLoad = result;
                    return result;
                }
                result.externalError = L"rules.json 规则表为空或格式无效";
                MS_LOG_WARN(L"rules.json parsed but empty/invalid rules array");
            } else {
                result.externalError = L"rules.json 解析失败: " + err;
                MS_LOG_WARN(L"rules.json parse failed: %s", err.c_str());
            }
        } else {
            result.externalError = L"rules.json 无法读取";
        }
    }
    validateInto(BuiltinRules(), false);
    g_lastLoad = result;
    return result;
}

std::vector<JunkRule> Load() {
    return LoadValidated().rules;
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
