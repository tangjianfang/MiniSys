#include "core/PlanBuilder.h"

#include "platform/Hash.h"
#include "util/StringUtils.h"

#include <set>

namespace minisys {

namespace {

// planHash input: tab id + "path|size|mtime;" per item, hashing the fields of
// the ScanItem at plan.items[i].itemIdx. Build hashes the scan-time list;
// PlanMatches hashes the live list — a rescan that replaced the results makes
// the digest differ and the plan is rejected as stale.
uint64_t HashPlanItems(TabId tab,
                       const std::vector<PlanItem>& pi,
                       const std::vector<ScanItem>& items) {
    std::wstring text;
    text.reserve(pi.size() * 96);
    text += std::to_wstring(static_cast<int>(tab));
    for (const auto& p : pi) {
        if (p.itemIdx >= items.size()) {
            text += L";<out-of-range>";
            continue;
        }
        const auto& it = items[p.itemIdx];
        text += L";" + it.path.wstring() + L"|" +
                std::to_wstring(it.sizeBytes) + L"|" +
                std::to_wstring(it.lastWriteFiletime);
    }
    return Hash64OfBuffer(text.data(), text.size() * sizeof(wchar_t));
}

} // namespace

CleanPlan PlanBuilder::Build(TabId tab,
                             const std::vector<ScanItem>& items,
                             const std::vector<size_t>& indices,
                             const std::wstring& migrateTargetRoot,
                             bool useSymlink) {
    CleanPlan plan;
    plan.tab = tab;
    plan.migrateTargetRoot = migrateTargetRoot;
    plan.useSymlink = useSymlink;
    std::set<std::wstring> seen;   // reject duplicate paths (R-006 guard)
    for (size_t idx : indices) {
        if (idx >= items.size()) continue;
        const auto& it = items[idx];
        auto key = ToLower(it.path.wstring());
        if (!seen.insert(key).second) continue;   // same path twice in one plan
        PlanItem pi;
        pi.itemIdx = idx;
        pi.path = it.path;
        pi.sizeAtScan = it.sizeBytes;
        pi.lastWriteAtScan = it.lastWriteFiletime;
        plan.items.push_back(std::move(pi));
    }
    plan.planHash = HashPlanItems(tab, plan.items, items);
    return plan;
}

bool PlanBuilder::PlanMatches(const CleanPlan& plan,
                              const std::vector<ScanItem>& items) {
    return HashPlanItems(plan.tab, plan.items, items) == plan.planHash;
}

} // namespace minisys
