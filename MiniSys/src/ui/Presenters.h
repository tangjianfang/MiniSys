#pragma once
#include "core/TabId.h"
#include "core/Scanner.h"
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

    // Indices (into SessionService results) of checked rows.
    std::vector<size_t> CollectChecked() const;

protected:
    UiHandles ui_;
    int  sortCol_ = -1;    // -1 none, 0 size, 1 time
    bool sortAsc_ = false; // false = descending

private:
    void ApplySortAndRefresh();
    void RenderItems();
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

    // Right-click context menu flow (delete folder). Returns true when an
    // item was deleted and the tree changed.
    bool OnContextMenu();

private:
    UiHandles ui_;
    SessionService& svc_;
    std::map<HTREEITEM, std::filesystem::path> itemPaths_;
};

class HistoryPresenter : public TabPresenter {
public:
    HistoryPresenter(UiHandles ui, SessionService& svc);

    void Refresh() override;   // render OperationLog records
    void UndoSelected();       // undo the selected record

private:
    UiHandles ui_;
    SessionService& svc_;
};

} // namespace minisys
