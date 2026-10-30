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
    // r7-02 U-7: Resolve the import ONCE — GetModuleHandleW+GetProcAddress
    // on every call measured 7.6 µs, paid per drawn row (checkbox glyph).
    // WM_DPICHANGED moves a window across monitors; the cached function
    // POINTER is stable, only its per-hwnd RESULT varies.
    using Fn  = UINT(WINAPI*)(HWND);
    using FnS = UINT(WINAPI*)();
    static Fn  fnWin = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    static FnS fnSys = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<FnS>(GetProcAddress(user32, "GetDpiForSystem"))
                      : nullptr;
    }();
    if (hwnd) {
        if (fnWin) return fnWin(hwnd);
    } else {
        // No window yet (initial CreateWindow size) — system DPI.
        if (fnSys) return fnSys();
    }
    return 96;
}
} // namespace

int UiScale(HWND hwnd, int v) { return Scale(v, WindowDpi(hwnd)); }

void LayoutWindow(const UiHandles& ui, int W, int H, bool showSettings,
                  bool showSearch, bool showDrives) {
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
    // v2.21 (简洁美观): two clean groups on a strict 8px grid — ACTIONS
    // anchored left (scan → exec, then the tab-specific extras), UTILITIES
    // anchored right (打开所在位置, and on Apps the 高级模式 checkbox left
    // of it). The old fixed-x chain left-packed everything and stranded a
    // dead zone on the right; the 按大小/按时间排序 duplicates of the
    // column-header sorting are gone entirely.
    const int scanW = Scale(96, dpi);
    const int execW = Scale(204, dpi);   // fits “执行选中操作（1000 项·1.31 GB）”
    const int undoW = Scale(150, dpi);
    const int emptyW = Scale(124, dpi);
    const int openW = Scale(124, dpi);
    const int targetW = Scale(170, dpi);
    const int advW = Scale(160, dpi);
    int lx = pad;                                    // left group cursor
    SetWindowPos(ui.scan,      nullptr, lx, btnY, scanW, btnH, SWP_NOZORDER);
    lx += scanW + pad;
    SetWindowPos(ui.exec,      nullptr, lx, btnY, execW, btnH, SWP_NOZORDER);
    lx += execW + pad;
    SetWindowPos(ui.targetBtn, nullptr, lx, btnY, targetW, btnH, SWP_NOZORDER);
    // History composition: [撤销][清空隔离区] from the left edge.
    SetWindowPos(ui.undo,   nullptr, pad, btnY, undoW, btnH, SWP_NOZORDER);
    SetWindowPos(ui.emptyQ, nullptr, pad + undoW + pad, btnY, emptyW,
                 btnH, SWP_NOZORDER);
    // Right group: [高级模式(Symlink)](Apps) [打开所在位置]
    int rx = W - pad - openW;
    SetWindowPos(ui.open, nullptr, rx, btnY, openW, btnH, SWP_NOZORDER);
    // REVIEW-UI P1 (L-4/X-16): the Apps checkbox used to be a fixed
    // x=828+w=360 that overflowed the 1100px client — right-anchored next
    // to the utility group now.
    SetWindowPos(ui.advancedChk, nullptr, rx - pad - advW, btnY, advW, btnH,
                 SWP_NOZORDER);

    // Settings/search row — rendered when LargeFiles or Search tab is
    // active (v2.3: the search box lives on the same row mechanism).
    const int editH = Scale(24, dpi);
    const int lblH  = Scale(22, dpi);
    int settingsRowH = (showSettings || showSearch || showDrives) ? (editH + pad) : 0;
    int settingsY = btnY + btnH + pad;

    if (showSearch) {
        int sx = pad;
        SetWindowPos(ui.lblSearch,   nullptr, sx, settingsY + 1,
                     Scale(44, dpi), lblH, SWP_NOZORDER);
        sx += Scale(44, dpi) + pad;
        // v2.6: [搜索框][快速筛选 ▾][匹配完整路径] — v2.21: uniform 8px
        // gaps across the row (was a 4/6/6 mix).
        int chkW = Scale(146, dpi);
        int qfW  = Scale(104, dpi);
        int chkX = W - pad - chkW;
        int qfX  = chkX - pad - qfW;
        SetWindowPos(ui.chkMatchPath, nullptr, chkX, settingsY + 1,
                     chkW, lblH, SWP_NOZORDER);
        SetWindowPos(ui.btnQuickFilter, nullptr, qfX, settingsY,
                     qfW, editH, SWP_NOZORDER);
        SetWindowPos(ui.editSearch,  nullptr, sx, settingsY,
                     qfX - pad - sx, editH, SWP_NOZORDER);
    }

    const int drivesW = Scale(168, dpi);
    if (showDrives && !showSettings) {
        SetWindowPos(ui.btnDrives, nullptr, pad, settingsY, drivesW, editH, SWP_NOZORDER);
    }

    if (showSettings) {
        const int editW   = Scale(70, dpi);
        const int editW2  = Scale(200, dpi);
        const int lblW1   = Scale(76, dpi);
        const int lblW2   = Scale(150, dpi);
        int sx = pad;
        SetWindowPos(ui.lblMinSize,     nullptr, sx,                  settingsY+1, lblW1, lblH, SWP_NOZORDER);
        SetWindowPos(ui.editMinSize,    nullptr, sx+lblW1+pad,        settingsY,   editW, editH, SWP_NOZORDER);
        SetWindowPos(ui.lblMinSizeUnit, nullptr, sx+lblW1+pad+editW+pad, settingsY+1, 28, lblH, SWP_NOZORDER);
        sx = sx + lblW1+pad+editW+pad+28+pad;
        SetWindowPos(ui.lblFileType,    nullptr, sx,                  settingsY+1, lblW2, lblH, SWP_NOZORDER);
        SetWindowPos(ui.editFileType,   nullptr, sx+lblW2+pad,        settingsY,   editW2, editH, SWP_NOZORDER);
        sx = sx + lblW2+pad+editW2+pad;
        SetWindowPos(ui.btnDrives,      nullptr, sx,                  settingsY,   drivesW, editH, SWP_NOZORDER);
    }

    int infoY = btnY + btnH + pad + settingsRowH;
    // REVIEW-UI 2026-10-06 live test: the list used to start at
    // infoY + Scale(40) while the info label is Scale(52) tall — the label
    // (higher in z-order) covered the top half of the column headers.
    // The list now starts strictly BELOW the info label.
    // review-07 X-6 (v2.10): the height follows the actual text (1-6 lines,
    // DT_CALCRECT) — the fixed 52px clipped the 4-8 line compositions that
    // exist since v2.5 (dashboard + notices + provenance + description).
    int infoH = Scale(52, dpi);
    {
        wchar_t buf[2048] = {};
        int len = GetWindowTextW(ui.info, buf, 2048);
        if (len > 0) {
            HDC dc = GetDC(ui.main);
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HGDIOBJ>(
                SendMessageW(ui.info, WM_GETFONT, 0, 0)));
            // v2.21: the strip is a card now — measure inside its inner
            // padding (8 left/right, DrawInfoText).
            RECT rc{ 0, 0, W - 2 * pad - Scale(16, dpi), 0 };
            DrawTextW(dc, buf, len, &rc,
                      DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
            SelectObject(dc, old);
            ReleaseDC(ui.main, dc);
            // v2.19 (user image #8): 17px/line — the owner-drawn info strip
            // (MainWindow DrawInfoText) adds 2px spacing between logical
            // lines on top of the ~15px text height; the old 15px slot
            // clipped the last line's breathing room.
            int lineH = Scale(17, dpi);
            int lines = (rc.bottom + lineH - 1) / lineH;
            if (lines < 1) lines = 1;
            if (lines > 6) lines = 6;   // cap — extremely long hints fold
            // v2.21: card vertical padding (4 top + 3 bottom).
            infoH = lines * lineH + Scale(7, dpi);
        }
    }
    SetWindowPos(ui.info, nullptr, pad, infoY, W - 2*pad, infoH, SWP_NOZORDER);

    int contentY = infoY + infoH + Scale(6, dpi);
    int contentH = H - contentY - statusH - pad;   // REVIEW-UI P1 (L-4b/X-16)
    SetWindowPos(ui.list, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);
    SetWindowPos(ui.tree, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);

    // v2.5: the progress bar no longer floats over the list's bottom-right
    // corner (it covered the horizontal scrollbar) — UpdateStatusBar now
    // positions it as an overlay on a dedicated 4th status pane.
}

} // namespace minisys
