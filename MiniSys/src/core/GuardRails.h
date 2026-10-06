#pragma once
#include "core/PlanBuilder.h"
#include "core/Scanner.h"

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace minisys {

// The single safety gate every execution must pass (DESIGN-v2 §3 invariant:
// no Operation is constructed without GuardRails). All checks are deny-only:
// a deny never fails the batch, the item goes to the "skipped" list.
//
// v2.2 (REVIEW-2026-10-06 P0-1): paths are CANONICALIZED before any prefix
// comparison (GetFinalPathNameByHandleW resolves 8.3 short names, "..",
// doubled separators and \\?\ prefixes — the raw-string prefix match was
// bypassable five ways, see review 05-T-B1). UNC paths are denied outright
// for file operations. The quarantine roots are generated for every fixed
// drive instead of a hardcoded C/D/E subset.
class GuardRails {
public:
    // Which operation channel the validation runs for — the protected set
    // differs: app migration sources legitimately live under Program Files,
    // while file operations (quarantine/delete) must never touch it.
    enum class Context { FileOp, Migration };

    struct Verdict {
        bool allow = true;
        std::wstring reason;   // human-readable deny reason
    };

    static GuardRails& Instance();

    // Full pre-execution check for one plan item (disk I/O involved):
    //   1. path is not protected (context-dependent list, canonicalized)
    //   2. path exists (still) and is not a reparse point
    //   3. not a cloud/offline placeholder
    //   4. size and mtime unchanged since scan (TOCTOU re-verify; Safe-level
    //      items skip the mtime check — temp dirs churn constantly and the
    //      check misfired on exactly the items users clean most)
    //   5. risk level allows the operation (InfoOnly / delegate-strategy
    //      items are never executed as direct file operations)
    Verdict Validate(const PlanItem& pi, const ScanItem& si,
                     Context ctx = Context::FileOp) const;

    // Protected for FILE operations (quarantine/delete): Windows (minus
    // built-in exempted cleanup subtrees), Program Files ×2, ProgramData
    // (minus exemptions), PerfLogs, Windows.old, $Recycle.Bin, SVI,
    // $WinREAgent, Recovery, the Users ROOT (exact — the user profile tree
    // itself is the main cleanup target), every fixed drive's quarantine
    // root, and all UNC paths. Canonicalized before matching.
    static bool IsProtectedPath(const std::filesystem::path& p);

    // Protected as an app-migration SOURCE/TARGET: Windows, WindowsApps,
    // ProgramData\Microsoft, the classic system roots, Users root, UNC,
    // quarantine roots. Program Files itself is NOT protected here — that
    // is where most migrated apps legitimately live.
    static bool IsProtectedMigrationSource(const std::filesystem::path& p);

    // Canonical form used for all comparisons: absolute, long names,
    // single backslashes, no \\?\ prefix. Returns false when the input
    // cannot be normalized (deny-by-default).
    static bool Canonicalize(const std::wstring& in, std::wstring& out);

    // True when the path is one of the built-in exempted system cleanup
    // subtrees (e.g. %SystemRoot%\Temp) — used by rule load-time validation.
    static bool IsExemptSystemCleanupPath(const std::filesystem::path& p);

    // Stricter variant for rule loading: the path must BE an exempt root
    // exactly (external rules cannot register new system-area subtrees).
    static bool IsExemptSystemCleanupRoot(const std::filesystem::path& p);

    // REVIEW P2 (08-F9): user exclusion list ("永不清理此文件夹") — set at
    // startup from settings.json; Validate denies these paths.
    static void SetUserExclusions(const std::vector<std::wstring>& paths);
    static bool IsUserExcluded(const std::filesystem::path& p);

    // Current attributes of a path (0 on failure).
    static DWORD FileAttributesOf(const std::filesystem::path& p);

private:
    GuardRails() = default;
};

} // namespace minisys
