#pragma once
#include "core/TabId.h"
#include "core/Scanner.h"
#include "core/Operation.h"
#include "ui/UiHandles.h"

#include <windows.h>
#include <commctrl.h>
#include <filesystem>
#include <map>
#include <memory>
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

    virtual void SortBySize() {}
    virtual void SortByTime() {}
    virtual void OnColumnClick(int /*col*/) {}
    // REVIEW P1-6 (08-F3): double-click / "说明…" opens the explanation.
    virtual void OnItemActivated(int /*row*/) {}

protected:
    TabId tab_;
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

    // Indices (into the snapshot) of checked rows.
    std::vector<size_t> CollectChecked() const;

    // v2.7/v2.8: drop rows whose path no longer exists on disk (deleted
    // outside the tool). The disk checks run on the SessionService worker
    // (VerifyPathsAsync); this applies the worker's dead-path list
    // (lowercased) to the snapshot, re-renders and syncs the service-side
    // results. Returns how many were removed.
    size_t ApplyDeadPaths(const std::vector<std::wstring>& deadLower);

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
    // (0 分类 / 1 风险 / 2 项目 / 3 大小 / 4 详情→时间); the header shows a
    // direction arrow for the active column.
    int  sortCol_ = -1;
    bool sortAsc_ = false; // false = descending
    std::vector<ScanItem> snapshot_;   // UI-private copy (REVIEW P1-1)
    // REVIEW-UI P0 (L-1): full per-path check state (true AND false) —
    // survives sorts, tab switches and data refreshes.
    std::map<std::wstring, bool> checkStateByPath_;
    bool batchUpdate_ = false;

    void CaptureCheckState();          // call before permuting/replacing snapshot_
    void RenderItems();

private:
    void ApplySortAndRefresh();
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
};

class HistoryPresenter : public TabPresenter {
public:
    HistoryPresenter(UiHandles ui, SessionService& svc);

    void Refresh() override;   // render OperationLog records
    void UndoSelected();       // undo the selected record

private:
    UiHandles ui_;
    SessionService& svc_;
    static std::wstring HistoryTypeLabel(const OpRecord& r);
    static std::wstring HistoryRiskLabel(const OpRecord& r);
};

} // namespace minisys
