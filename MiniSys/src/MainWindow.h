#pragma once
#include "core/Scanner.h"
#include "core/Operation.h"
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <vector>
#include <map>

namespace minisys {

enum class TabId : int {
    Junk = 0,
    LargeFiles = 1,
    Apps = 2,
    FolderTree = 3,
    History = 4,
    Count
};

class MainWindow {
public:
    bool Create(HINSTANCE hInst, int nCmdShow);
    int  RunMessageLoop();

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnSize();
    void OnTabChanged();
    void OnScan();
    void OnExecute();
    void OnUndo();
    void OnOpenLocation();
    void OnChooseTarget();
    void OnSortSize();
    void OnSortTime();
    void OnListColumnClick(int col);
    void OnTreeViewRClick();

    void RefreshHistory();
    void UpdateStatusBar();
    void RefreshListView();
    void BuildFolderTree();
    void ApplySortAndRefresh();
    void AppendLog(const std::wstring& msg);

    void StartScannerAsync(std::unique_ptr<Scanner> scanner);
    void OnScanProgress(const std::wstring& msg);
    void OnScanDone();

    TabId CurrentTab() const;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hTab_ = nullptr;
    HWND hList_ = nullptr;
    HWND hStatus_ = nullptr;
    HWND hProgress_ = nullptr;
    HWND hScan_ = nullptr;
    HWND hExec_ = nullptr;
    HWND hUndo_ = nullptr;
    HWND hOpen_ = nullptr;
    HWND hTargetBtn_ = nullptr;
    HWND hAdvancedChk_ = nullptr;
    HWND hInfoLabel_ = nullptr;

    // LargeFiles tab – settings panel (shown only on LargeFiles tab)
    HWND hEditMinSize_    = nullptr; // min file size (MB)
    HWND hEditFileType_   = nullptr; // extension filter
    HWND hEditDrives_     = nullptr; // drives to scan
    HWND hLblMinSize_     = nullptr; // "最小大小:"
    HWND hLblMinSizeUnit_ = nullptr; // "MB"
    HWND hLblFileType_    = nullptr; // "文件类型(.ext;...):"
    HWND hLblDrives_      = nullptr; // "扫描磁盘(C;D; 空=默认):"

    // Sort buttons (shown on scan result tabs)
    HWND hBtnSortSize_ = nullptr;
    HWND hBtnSortTime_ = nullptr;

    // FolderTree tab – TreeView (shown only on FolderTree tab)
    HWND hTreeView_ = nullptr;
    std::map<HTREEITEM, std::filesystem::path> treeItemPaths_;

    // Per-tab results (reused across navigations)
    std::vector<std::vector<ScanItem>> tabResults_;

    // Sort state for the list view
    int  sortCol_ = -1;   // -1 = none, 0 = size, 1 = time
    bool sortAsc_ = false; // false = descending

    // Background scan
    std::thread scanThread_;
    std::atomic<bool> cancelScan_{false};
    std::atomic<bool> scanning_{false};
    std::mutex progressMu_;
    std::wstring scanProgressText_;
    std::vector<ScanItem> scanBuffer_;

    // Migration target dir (for Apps tab)
    std::wstring migrateTargetRoot_; // e.g. D:\MigratedApps
    bool useSymlink_ = false;
};

} // namespace minisys
