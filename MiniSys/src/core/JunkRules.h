#pragma once
#include "core/Scanner.h"

#include <filesystem>
#include <string>
#include <vector>

namespace minisys {

// CleanStrategy lives in core/Scanner.h (ScanItem carries it for execution).

// How a rule's path expands into candidate items:
//   Subtree  — the path itself is one item (Temp folder, npm-cache, ...)
//   Children — every direct child of the path is an item, optionally filtered
//              by a wildcard childPattern ("thumbcache_*.db")
//   Profiles — for each direct child dir P of the path, for each semicolon
//              name N in profileNames: P\N is an item (browser multi-profile)
enum class RuleMode { Subtree, Children, Profiles };

struct JunkRule {
    std::wstring id;
    std::wstring category;
    std::wstring title;
    std::wstring path;          // may contain %ENV% placeholders
    RuleMode     mode = RuleMode::Subtree;
    std::wstring childPattern;  // Children mode: wildcard filter ("" = all)
    std::wstring profileNames;  // Profiles mode: "Cache;Code Cache;GPUCache"
    RiskLevel    riskLevel = RiskLevel::Cautious;
    int          minAgeDays = 0;      // item mtime must be older than this
    CleanStrategy strategy = CleanStrategy::Quarantine;
    bool         recommended = true;  // default checkbox state
    std::wstring command;      // Delegate strategy: command line (M3)
    std::wstring detailHint;   // extra detail text shown in the list
};

namespace JunkRules {

// Load rules: rules.json next to the exe, falling back to the built-in
// table on any parse/IO failure (R-005 mitigation).
std::vector<JunkRule> Load();

// Expand %ENV% placeholders in a path (empty on failure).
std::wstring ExpandEnv(const std::wstring& s);

// Case-insensitive wildcard match ('*' and '?').
bool MatchWildcard(const std::wstring& pattern, const std::wstring& name);

// Serialize helpers for rule fields (JSON ↔ enum).
RiskLevel     ParseRiskLevel(const std::wstring& s);
CleanStrategy ParseStrategy(const std::wstring& s);
RuleMode      ParseMode(const std::wstring& s);

} // namespace JunkRules

} // namespace minisys
