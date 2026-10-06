#include "ui/Layout.h"

#include <windows.h>
#include <commctrl.h>

namespace minisys {

namespace {
// REVIEW P3 (07-X18): fixed pixel constants declared PerMonitorV2-aware but
// never scaled — at 150% the buttons stayed 30 px and icons 16 px. All
// layout metrics scale with the window DPI now.
int Scale(int v, UINT dpi) { return v * static_cast<int>(dpi) / 96; }
}

void LayoutWindow(const UiHandles& ui, int W, int H, bool showSettings,
                  bool showSearch) {
    UINT dpi = 96;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using Fn = UINT(WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))) {
            dpi = fn(ui.main);
        }
    }

    SendMessageW(ui.status, WM_SIZE, 0, 0);
    RECT srect; GetClientRect(ui.status, &srect);
    int statusH = srect.bottom - srect.top;

    const int pad = Scale(8, dpi);
    const int btnH = Scale(30, dpi);
    const int btnW = Scale(140, dpi);
    int tabH = Scale(28, dpi);
    SetWindowPos(ui.tab, nullptr, 0, 0, W, tabH, SWP_NOZORDER);

    // About button: top-right on the tab strip (classic help placement,
    // clear of the crowded Apps-tab button row).
    SetWindowPos(ui.about, nullptr, W - Scale(84, dpi), 1, Scale(80, dpi), tabH - 2, SWP_NOZORDER);

    int btnY = tabH + pad;
    SetWindowPos(ui.scan,        nullptr, pad,                     btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(ui.exec,        nullptr, pad + btnW + pad,        btnY, btnW + 20, btnH, SWP_NOZORDER);
    SetWindowPos(ui.undo,        nullptr, pad + 2*(btnW + pad)+20, btnY, btnW + 20, btnH, SWP_NOZORDER);
    SetWindowPos(ui.emptyQ,      nullptr, pad + 3*(btnW + pad)+40, btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(ui.open,        nullptr, pad + 3*(btnW + pad)+40, btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(ui.targetBtn,   nullptr, pad + 4*(btnW + pad)+40, btnY, btnW + 40, btnH, SWP_NOZORDER);
    SetWindowPos(ui.advancedChk, nullptr, pad + 5*(btnW + pad)+80, btnY, 360, btnH, SWP_NOZORDER);
    SetWindowPos(ui.btnSortSize, nullptr, pad + 4*(btnW + pad)+40, btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(ui.btnSortTime, nullptr, pad + 5*(btnW + pad)+40, btnY, btnW, btnH, SWP_NOZORDER);

    // Settings/search row — rendered when LargeFiles or Search tab is
    // active (v2.3: the search box lives on the same row mechanism).
    const int editH = Scale(24, dpi);
    const int lblH  = Scale(22, dpi);
    int settingsRowH = (showSettings || showSearch) ? (editH + pad) : 0;
    int settingsY = btnY + btnH + pad;

    if (showSearch) {
        int sx = pad;
        SetWindowPos(ui.lblSearch,   nullptr, sx, settingsY + 1,
                     Scale(46, dpi), lblH, SWP_NOZORDER);
        sx += Scale(46, dpi) + 4;
        SetWindowPos(ui.editSearch,  nullptr, sx, settingsY,
                     W - sx - pad - Scale(150, dpi), editH, SWP_NOZORDER);
        SetWindowPos(ui.chkMatchPath, nullptr,
                     W - pad - Scale(146, dpi), settingsY + 1,
                     Scale(146, dpi), lblH, SWP_NOZORDER);
    }

    if (showSettings) {
        const int editW   = Scale(70, dpi);
        const int editW2  = Scale(200, dpi);
        const int editW3  = Scale(80, dpi);
        const int lblW1   = Scale(76, dpi);
        const int lblW2   = Scale(150, dpi);
        const int lblW3   = Scale(190, dpi);
        int sx = pad;
        SetWindowPos(ui.lblMinSize,     nullptr, sx,                  settingsY+1, lblW1, lblH, SWP_NOZORDER);
        SetWindowPos(ui.editMinSize,    nullptr, sx+lblW1+4,          settingsY,   editW, editH, SWP_NOZORDER);
        SetWindowPos(ui.lblMinSizeUnit, nullptr, sx+lblW1+4+editW+4,  settingsY+1, 28,   lblH, SWP_NOZORDER);
        sx = sx + lblW1+4+editW+4+28+12;
        SetWindowPos(ui.lblFileType,    nullptr, sx,                  settingsY+1, lblW2, lblH, SWP_NOZORDER);
        SetWindowPos(ui.editFileType,   nullptr, sx+lblW2+4,          settingsY,   editW2, editH, SWP_NOZORDER);
        sx = sx + lblW2+4+editW2+12;
        SetWindowPos(ui.lblDrives,      nullptr, sx,                  settingsY+1, lblW3, lblH, SWP_NOZORDER);
        SetWindowPos(ui.editDrives,     nullptr, sx+lblW3+4,          settingsY,   editW3, editH, SWP_NOZORDER);
    }

    int infoY = btnY + btnH + pad + settingsRowH;
    SetWindowPos(ui.info, nullptr, pad, infoY, W - 2*pad, Scale(36, dpi), SWP_NOZORDER);

    int contentY = infoY + Scale(40, dpi);
    int contentH = H - contentY - pad;
    SetWindowPos(ui.list, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);
    SetWindowPos(ui.tree, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);

    // v2.2 (REVIEW P0-5 / 07-X8): the old position (y = H + 2) placed the
    // bar BELOW the client area — the progress bar has never been visible
    // since v1. Sit it just above the status bar instead.
    int prgW = Scale(220, dpi), prgH = statusH - 4;
    SetWindowPos(ui.progress, nullptr, W - prgW - 4,
                 H - statusH - prgH - 4, prgW, prgH, SWP_NOZORDER);
}

} // namespace minisys
