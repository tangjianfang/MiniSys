#pragma once
#include "core/TabId.h"
#include "core/Scanner.h"
#include "core/Operation.h"
#include "ui/UiHandles.h"

#include <windows.h>
#include <commctrl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace minisys {

class SessionService;

// Per-tab UI behaviour. MainWindow owns the controls and routes messages;
// presenters implement tab-specific logic (scanner construction, rendering,
// actions). One instance per tab, owned by MainWindow.
class TabPresenter {
public:
    explicit TabPresenter(TabId tab) : tab_(tab) {}
    virtual ~TabPresenter() = default;

    TabId Tab() const { return tab_; }

    // Tab became active (v1 reset sort state here).
    virtual void OnActivate() {}

    // Scanner for this tab, or nullptr if the tab has no scan.
    virtual std::unique_ptr<Scanner> BuildScanner() { return nullptr; }

    // Re-render tab content from SessionService results.
    virtual void Refresh() {}

    // Scan finished for this tab (default: refresh).
    virtual void OnScanDone() { Refresh(); }

    // v2.24 (r7 X-2): should this row paint dimmed (dead/ historical)?
    // The history page greys out [已释放] rows so "cleaned" state reads at
    // a glance. Custom-draw asks the ACTIVE presenter per row.
    virtual bool RowIsDimmed(size_t /*row*/) const { return false; }

    virtual void SortBySize() {}
    virtual void SortByTime() {}
    virtual void OnColumnClick(int /*col*/) {}
    // REVIEW P1-6 (08-F3): double-click / "说明…" opens the explanation.
    virtual void OnItemActivated(int /*row*/) {}

    // v2.15 (round-4 02 P0): the ListView is virtual (LVS_OWNERDATA) —
    // presenters supply cell text/state through LVN_GETDISPINFO instead of
    // per-row inserts (1000 rows measured 1.26-1.36 s; virtual ≈ 70 ms).
    virtual bool SupplyDispInfo(NMLVDISPINFOW* /*di*/) { return false; }

    // v2.25: the user's 扫描盘 selection as drive roots ({"C:\\", "D:\\"});
    // empty = each tab's own default. Shared by all presenters, UI thread
    // only — MainWindow writes it, BuildScanner() overrides read it.
    static void SetScanDrives(std::vector<std::wstring> roots) {
        scanDrives_ = std::move(roots);
    }
    static const std::vector<std::wstring>& ScanDrives() { return scanDrives_; }

protected:
    TabId tab_;

private:
    static inline std::vector<std::wstring> scanDrives_;
};

// Shared list rendering + sorting for the three ListView scan tabs
// (Junk / LargeFiles / Apps).
class ListTabPresenter : public TabPresenter {
public:
    ListTabPresenter(TabId tab, UiHandles ui);

    void OnActivate() override;
    void Refresh() override;
    void SortBySize() override;
    void SortByTime() override;
    void OnColumnClick(int col) override;
    void OnItemActivated(int row) override;

    // Indices (into the snapshot) of checked rows (v2.15: map-driven —
    // the virtual list stores no per-item state).
    std::vector<size_t> CollectChecked() const;

    // v2.15 virtual mode: text/state for row `di->item.iItem`.
    bool SupplyDispInfo(NMLVDISPINFOW* di) override;
    // v2.15: set every row's check (Ctrl+A / menu) without a message storm.
    void SetAllChecked(bool on);
    // v2.22: 反选 — flip every row's check (context menu / Ctrl+I).
    void InvertChecked();
    // v2.26: bring every checked row to the top (context menu) so the
    // "执行（N 项）" count can be seen, not just trusted.
    void MoveCheckedToTop();
    // v2.15: record a user check-flip from the virtual control into the map.
    void OnCheckFlipped(int row, bool checked);
    // X-41 (review r6-07, P0): read one row's check state — the source the
    // owner-drawn checkbox glyph paints from (the comctl6 owner-data list
    // never paints the state-image slot itself).
    bool IsCheckedAt(int row) const;
    // v2.15: toggle one row's check; returns the NEW state (drives the
    // state-icon click / Space paths for virtual lists).
    bool ToggleCheckAt(int row);

    // v2.7/v2.8: drop rows whose path no longer exists on disk (deleted
    // outside the tool). The disk checks run on the SessionService worker
    // (VerifyPathsAsync); this applies the worker's dead-path list
    // (lowercased) to the snapshot and syncs the service-side results.
    // render=false prunes silently — review-04 R-1: the shared ListView
    // belongs to the ACTIVE tab; rendering a background tab's rows into it
    // corrupts the display and the lParam↔snapshot mapping.
    size_t ApplyDeadPaths(const std::vector<std::wstring>& deadLower,
                          bool render = true);

    // REVIEW P1-1: the presenter renders from its own UI-private snapshot
    // — never from the worker-owned storage. Plans are built from it too.
    const std::vector<ScanItem>& Snapshot() const { return snapshot_; }
    const ScanItem* ItemAtRow(int row) const;

    // REVIEW-UI P0/P1 (L-6/04-7): while true, the main window skips the
    // per-change UpdateExecButton work during batch renders.
    bool InBatchUpdate() const { return batchUpdate_; }
    void SetBatchUpdate(bool on) { batchUpdate_ = on; }

protected:
    UiHandles ui_;
    // REVIEW-UI P2 (L-10): sortCol_ now stores the actual COLUMN index
    // (0 分类 / 1 风险 / 2 项目 / 3 大小 / 4 详情 / 5 修改时间); the header
    // shows a direction arrow for the active column.
    int  sortCol_ = -1;
    bool sortAsc_ = false; // false = descending
    std::vector<ScanItem> snapshot_;   // UI-private copy (REVIEW P1-1)
    std::vector<std::wstring> cells_;  // v2.15: pre-rendered row texts (6/row)
    // REVIEW-UI P0 (L-1): full per-path check state (true AND false) —
    // survives sorts, tab switches and data refreshes.
    std::map<std::wstring, bool> checkStateByPath_;
    bool batchUpdate_ = false;
    // review-07 X-7 (v2.10): view state survives full re-renders (sort,
    // tab round-trips, verify prunes, search refresh) — top row, focus and
    // selection, keyed by path like the check state.
    std::wstring viewTopPath_;
    std::wstring viewFocusPath_;
    std::vector<std::wstring> viewSelPaths_;

    void CaptureCheckState();          // v2.15: no-op (map is authoritative)
    void CaptureViewState();           // X-7: same timing rule as above
    void SyncCheckDefaults();          // v2.15: seed map for new snapshot paths
    void RestoreViewState();           // X-7: after the insert loop
    void RenderItems();

private:
    void ApplySortAndRefresh();
    // r7 X-4: the permutation core (no capture/render) — Refresh() replays
    // the user's sort on fresh data; v2.23 reset row order on scan-done /
    // tab round-trips while the header arrow kept promising otherwise.
    void ApplySortToSnapshot();
    void ShowItemInfo(const ScanItem& it);   // REVIEW P1-6
    std::wstring RiskBadge(const ScanItem& it);   // tab-aware (v2.4)
};

class JunkPresenter : public ListTabPresenter {
public:
    using ListTabPresenter::ListTabPresenter;
    std::unique_ptr<Scanner> BuildScanner() override;
};

class LargeFilesPresenter : public ListTabPresenter {
public:
    using ListTabPresenter::ListTabPresenter;
    std::unique_ptr<Scanner> BuildScanner() override;  // reads the settings edits
};

class AppsPresenter : public ListTabPresenter {
public:
    using ListTabPresenter::ListTabPresenter;
    std::unique_ptr<Scanner> BuildScanner() override;
};

// v2.3: Everything-style instant search over the shared VolumeIndex.
// No scanner — the search box drives filtering on the UI thread with a
// debounce; the scan button rebuilds/refreshes the index instead.
class SearchPresenter : public ListTabPresenter {
public:
    SearchPresenter(TabId tab, UiHandles ui);

    std::unique_ptr<Scanner> BuildScanner() override;   // nullptr
    void Refresh() override;

    // Called by the main window (debounced edit change / checkbox toggle).
    // REVIEW-UI P1: records the request only — SearchAsync (worker) runs it.
    void SetQuery(const std::wstring& text, bool matchPath);

    const std::wstring& Query() const { return query_; }
    size_t CellCountForProbe() const { return cells_.size(); }   // -probe
    bool MatchPath() const { return matchPath_; }
    size_t TotalIndexed() const;

private:
    std::wstring query_;
    bool matchPath_ = false;
};

class FolderTreePresenter : public TabPresenter {
public:
    FolderTreePresenter(UiHandles ui, SessionService& svc);

    std::unique_ptr<Scanner> BuildScanner() override;
    void Refresh() override;   // rebuild TreeView from results
    void OnScanDone() override;

    // Right-click context menu flow (delete folder). Returns true when an
    // item was deleted and the tree changed.
    bool OnContextMenu();

    // v2.23: lazy expansion — TVN_ITEMEXPANDING on an unloaded node kicks a
    // worker fetch (FolderChildrenAsync); WM_APP_TREE_CHILDREN applies it
    // (MainWindow unpacks the heap payload and calls ApplyChildren).
    bool OnItemExpanding(HTREEITEM item);
    void ApplyChildren(const std::wstring& parentLower,
                       std::vector<ScanItem> items);
    // v2.23: path lookup for drill-down (lazy nodes carry no lParam index).
    std::filesystem::path PathOfItem(HTREEITEM item) const {
        auto it = itemPaths_.find(item);
        return it == itemPaths_.end() ? std::filesystem::path{} : it->second;
    }

    // REVIEW P3: drill into a subfolder (double-click) — the scan then
    // covers that folder's top level instead of all fixed drives; clicking
    // 扫描 again returns to the full-drive view.
    void SetFocusRoot(const std::filesystem::path& p) { focusRoot_ = p; }
    void ClearFocusRoot() { focusRoot_.clear(); }
    bool HasFocusRoot() const { return !focusRoot_.empty(); }
    std::filesystem::path FocusRoot() const { return focusRoot_; }

private:
    UiHandles ui_;
    SessionService& svc_;
    std::map<HTREEITEM, std::filesystem::path> itemPaths_;
    std::filesystem::path focusRoot_;   // REVIEW P3 drill-down
    // v2.23 lazy expansion state
    std::map<std::wstring, HTREEITEM> nodeByPath_;      // lowercase path → node
    std::set<std::wstring> childrenLoaded_;             // lowercase paths
    HTREEITEM loadingItem_ = nullptr;                   // "加载中…" placeholder
    HTREEITEM InsertFolderNode(HTREEITEM hParent, const std::wstring& title,
                               unsigned long long sizeBytes,
                               const std::filesystem::path& path);
};

class HistoryPresenter : public TabPresenter {
public:
    HistoryPresenter(UiHandles ui, SessionService& svc);

    void Refresh() override;   // render OperationLog records
    // v2.24 (r7 X-3): entering the page reconciles record bookkeeping with
    // disk truth (releases records whose quarantine targets vanished).
    void OnActivate() override;
    void UndoSelected();       // undo the selected record
    bool SupplyDispInfo(NMLVDISPINFOW* di) override;   // v2.15
    bool RowIsDimmed(size_t row) const override;   // [已释放] rows grey out

    // v2.24: public + static so the label contract is unit-testable
    // headless (MiniSysTests) — the v2.23 bug lived exactly here: rows
    // kept saying "可还原" after 清空隔离区 permanently deleted the files.
    static std::wstring HistoryTypeLabel(const OpRecord& r);
    static std::wstring HistoryRiskLabel(const OpRecord& r);

    // v2.24: undo pre-flight classification (also unit-testable). Rows
    // whose quarantine copy was released are counted separately — the
    // v2.23 pre-flight waved them through into guaranteed failures.
    struct UndoClassify {
        size_t eligible = 0, released = 0, notReversible = 0,
               deleteType = 0, unsignedN = 0;
    };
    static UndoClassify ClassifyUndo(const std::vector<OpRecord>& sel);

private:
    UiHandles ui_;
    SessionService& svc_;
    std::vector<OpRecord> rows_;   // v2.15: virtual-mode backing store
    std::vector<std::wstring> cells_;   // pre-rendered per-row texts (col-major)
};

} // namespace minisys
