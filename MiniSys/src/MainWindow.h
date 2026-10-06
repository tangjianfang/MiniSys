#pragma once
#include "core/TabId.h"
#include "core/Settings.h"
#include "ui/UiHandles.h"
#include "ui/Presenters.h"

#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <string>

namespace minisys {

// Main window shell: owns the controls, routes messages to the active
// TabPresenter and delegates scanning/execution to SessionService.
// Tab-specific behaviour lives in ui/Presenters.cpp (M0 refactor).
class MainWindow {
public:
    bool Create(HINSTANCE hInst, int nCmdShow);
    int  RunMessageLoop();

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // REVIEW P0-5 (04-R1 stopgap): one place toggles every action control
    // for the current task state — the old per-handler EnableWindow calls
    // left sort buttons / column clicks / tab switches live while the
    // worker mutated the results vector.
    enum class TaskMode { None, Scanning, Executing };
    void SetTaskBusy(TaskMode mode);

    void OnCreate();
    void OnSize();
    void OnTabChanged();
    void OnScan();
    void OnScanDone();
    void OnExecute();
    void OnPlanDone();
    void OnEmptyQuarantine();
    void OnAbout();
    void OnOpenLocation();
    void OnChooseTarget();
    void UpdateStatusBar();
    void UpdateExecButton();                  // REVIEW P0-6: live count/size
    void ShowHint(const std::wstring& text);   // non-modal hint in the info label

    // REVIEW P2 additions.
    void LoadSettings();
    void SaveSettings();
    void OnSelectAll();                        // Ctrl+A
    void OnPreviewExecution();                 // dry-run GuardRails (08-F7)
    void OnExcludeSelected();                  // "永不清理此文件夹" (08-F9)
    void OnListContextMenu();                  // list right-click menu
    void UpdateSearchStatus();                 // v2.3: search result count

    TabId CurrentTab() const;
    TabPresenter* ActivePresenter() const;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    UiHandles h_{};
    std::unique_ptr<TabPresenter>
        presenters_[static_cast<size_t>(TabId::Count)];

    // Migration target dir (for Apps tab)
    std::wstring migrateTargetRoot_; // e.g. D:\MigratedApps
    bool useSymlink_ = false;
    TaskMode taskMode_ = TaskMode::None;
    Settings settings_;                       // REVIEW P2: persisted state
};

} // namespace minisys
