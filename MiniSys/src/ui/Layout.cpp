#include "ui/Layout.h"

#include <windows.h>
#include <commctrl.h>

namespace minisys {

namespace {
// REVIEW P3 (07-X18): fixed pixel constants declared PerMonitorV2-aware but
// never scaled — at 150% the buttons stayed 30 px and icons 16 px. All
// layout metrics scale with the window DPI now.
int Scale(int v, UINT dpi) { return v * static_cast<int>(dpi) / 96; }

UINT WindowDpi(HWND hwnd) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (hwnd) {
            using Fn = UINT(WINAPI*)(HWND);
            if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))) {
                return fn(hwnd);
            }
        } else {
            // No window yet (initial CreateWindow size) — system DPI.
            using FnS = UINT(WINAPI*)();
            if (auto fn = reinterpret_cast<FnS>(GetProcAddress(user32, "GetDpiForSystem"))) {
                return fn();
            }
        }
    }
    return 96;
}
} // namespace

int UiScale(HWND hwnd, int v) { return Scale(v, WindowDpi(hwnd)); }

void LayoutWindow(const UiHandles& ui, int W, int H, bool showSettings,
                  bool showSearch) {
    UINT dpi = WindowDpi(ui.main);

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
    // REVIEW-UI P1 (L-4/X-16): right-anchored — the fixed x=828+w=360
    // overflowed the default 1100px window's client area.
    {
        int advW = Scale(240, dpi);
        SetWindowPos(ui.advancedChk, nullptr, W - pad - advW, btnY, advW, btnH, SWP_NOZORDER);
    }
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
        // v2.6: [搜索框][快速筛选 ▾][匹配完整路径] — the quick-filter
        // button squeezes in between the edit and the checkbox.
        int chkW = Scale(146, dpi);
        int qfW  = Scale(104, dpi);
        int chkX = W - pad - chkW;
        int qfX  = chkX - 6 - qfW;
        SetWindowPos(ui.chkMatchPath, nullptr, chkX, settingsY + 1,
                     chkW, lblH, SWP_NOZORDER);
        SetWindowPos(ui.btnQuickFilter, nullptr, qfX, settingsY,
                     qfW, editH, SWP_NOZORDER);
        SetWindowPos(ui.editSearch,  nullptr, sx, settingsY,
                     qfX - 6 - sx, editH, SWP_NOZORDER);
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
    // REVIEW-UI 2026-10-06 live test: the list used to start at
    // infoY + Scale(40) while the info label is Scale(52) tall — the label
    // (higher in z-order) covered the top half of the column headers.
    // The list now starts strictly BELOW the info label.
    int infoH = Scale(52, dpi);
    SetWindowPos(ui.info, nullptr, pad, infoY, W - 2*pad, infoH, SWP_NOZORDER);

    int contentY = infoY + infoH + Scale(4, dpi);
    int contentH = H - contentY - statusH - pad;   // REVIEW-UI P1 (L-4b/X-16)
    SetWindowPos(ui.list, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);
    SetWindowPos(ui.tree, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);

    // v2.5: the progress bar no longer floats over the list's bottom-right
    // corner (it covered the horizontal scrollbar) — UpdateStatusBar now
    // positions it as an overlay on a dedicated 4th status pane.
}

} // namespace minisys
