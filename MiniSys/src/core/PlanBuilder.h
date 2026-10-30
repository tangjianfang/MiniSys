#pragma once
#include "core/TabId.h"
#include "core/Scanner.h"

#include <filesystem>
#include <vector>

namespace minisys {

// One unit of work chosen by the user for execution, frozen at scan time.
struct PlanItem {
    size_t itemIdx = 0;              // index into the ScanItem result list
    std::filesystem::path path;      // copied for re-verification
    uint64_t sizeAtScan = 0;
    uint64_t lastWriteAtScan = 0;
};

// The whole confirmed batch. planHash is a 64-bit digest (SHA-256 derived)
// over every item's (path, sizeAtScan, lastWriteAtScan); ExecutePlan
// recomputes it against the live result list and rejects the whole plan on
// mismatch (ERR_PLAN_STALE).
struct CleanPlan {
    TabId tab = TabId::Junk;
    std::vector<PlanItem> items;
    uint64_t planHash = 0;

    // Apps-tab migration context (empty for other tabs).
    std::wstring migrateTargetRoot;
    bool useSymlink = false;
    bool createRestorePoint = false;   // REVIEW P3 (08-F8)
};

class PlanBuilder {
public:
    // Build a plan from the checked rows. `indices` index into `items`.
    static CleanPlan Build(TabId tab,
                           const std::vector<ScanItem>& items,
                           const std::vector<size_t>& indices,
                           const std::wstring& migrateTargetRoot = {},
                           bool useSymlink = false);

    // Recompute the hash of `plan` against the live result list. Returns
    // false when the list has changed since the plan was built (stale).
    static bool PlanMatches(const CleanPlan& plan,
                            const std::vector<ScanItem>& items);

    // v2.26: rows that must clear an extra "are you REALLY sure" dialog on
    // top of the normal confirmation: flagged dangerous, plain quarantine
    // strategy, and not already covered by the system-component / recycle-bin
    // warnings. Today that is version-control internals (.git packs). Before
    // v2.26 ScanItem::dangerous was written by the scanners but read by
    // nobody — the flag promised a confirmation that never appeared.
    static bool IsGuarded(const ScanItem& it);

    // v2.26: Apps tab — drop selected rows that already live on the
    // migration target's drive (a same-volume move frees nothing; the
    // MoveJunctionOp preflight refuses them, but only AFTER the user has
    // confirmed). `dropped` receives how many were removed.
    static std::vector<size_t> DropSameDrive(const std::vector<ScanItem>& items,
                                             const std::vector<size_t>& indices,
                                             const std::wstring& migrateTargetRoot,
                                             size_t* dropped = nullptr);
};

} // namespace minisys
