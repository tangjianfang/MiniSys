#pragma once
#include "core/TabId.h"
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

    void OnCreate();
    void OnSize();
    void OnTabChanged();
    void OnScan();
    void OnScanDone();
    void OnExecute();
    void OnPlanDone();
    void OnEmptyQuarantine();
    void OnOpenLocation();
    void OnChooseTarget();
    void UpdateStatusBar();

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
};

} // namespace minisys
