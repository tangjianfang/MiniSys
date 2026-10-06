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

    // REVIEW P1-1: the presenter renders from its own UI-private snapshot
    // — never from the worker-owned storage. Plans are built from it too.
    const std::vector<ScanItem>& Snapshot() const { return snapshot_; }
    const ScanItem* ItemAtRow(int row) const;

protected:
    UiHandles ui_;
    int  sortCol_ = -1;    // -1 none, 0 size, 1 time
    bool sortAsc_ = false; // false = descending
    std::vector<ScanItem> snapshot_;   // UI-private copy (REVIEW P1-1)

private:
    void ApplySortAndRefresh();
    void RenderItems();
    void ShowItemInfo(const ScanItem& it);   // REVIEW P1-6
    static std::wstring RiskBadge(const ScanItem& it);
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
