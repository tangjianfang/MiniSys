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
class GuardRails {
public:
    struct Verdict {
        bool allow = true;
        std::wstring reason;   // human-readable deny reason
    };

    static GuardRails& Instance();

    // Full pre-execution check for one plan item (disk I/O involved):
    //   1. path is not under a protected system location
    //   2. path exists (still) and is not a reparse point
    //   3. not a cloud/offline placeholder
    //   4. size and mtime unchanged since scan (TOCTOU re-verify)
    //   5. risk level allows file operations (InfoOnly / delegate-strategy
    //      items are never executed directly)
    Verdict Validate(const PlanItem& pi, const ScanItem& si) const;

    // Pure path-level check (no disk I/O) — also used by scanners to mark
    // items and by MoveJunctionOp's protected-path logic.
    static bool IsProtectedPath(const std::filesystem::path& p);

    // Current attributes of a path (0 on failure).
    static DWORD FileAttributesOf(const std::filesystem::path& p);

private:
    GuardRails() = default;
};

} // namespace minisys
