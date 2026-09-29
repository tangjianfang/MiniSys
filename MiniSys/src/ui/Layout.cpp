#include "ui/Layout.h"

#include <windows.h>
#include <commctrl.h>

namespace minisys {

void LayoutWindow(const UiHandles& ui, int W, int H, bool showSettings) {
    SendMessageW(ui.status, WM_SIZE, 0, 0);
    RECT srect; GetClientRect(ui.status, &srect);
    int statusH = srect.bottom - srect.top;

    constexpr int pad = 8;
    constexpr int btnH = 30;
    constexpr int btnW = 140;
    int tabH = 28;
    SetWindowPos(ui.tab, nullptr, 0, 0, W, tabH, SWP_NOZORDER);

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

    // LargeFiles settings row — only rendered when LargeFiles tab is active.
    constexpr int editH   = 24;
    constexpr int lblH    = 22;
    int settingsRowH = showSettings ? (editH + pad) : 0;

    if (showSettings) {
        constexpr int editW   = 70;
        constexpr int editW2  = 200;
        constexpr int editW3  = 80;
        constexpr int lblW1   = 76;
        constexpr int lblW2   = 150;
        constexpr int lblW3   = 190;
        int settingsY = btnY + btnH + pad;
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
    SetWindowPos(ui.info, nullptr, pad, infoY, W - 2*pad, 20, SWP_NOZORDER);

    int contentY = infoY + 24;
    int contentH = H - contentY - pad;
    SetWindowPos(ui.list, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);
    SetWindowPos(ui.tree, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);

    // Progress bar: over the right side of the status bar area.
    int sbTop = H;  // status bar starts at H (below content)
    int prgW = 220, prgH = statusH - 4;
    SetWindowPos(ui.progress, nullptr, W - prgW - 4, sbTop + 2, prgW, prgH, SWP_NOZORDER);
}

} // namespace minisys
