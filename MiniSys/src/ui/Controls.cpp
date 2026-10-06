#include "ui/Controls.h"
#include "res/resource.h"

#include <commctrl.h>
#include <initializer_list>

namespace minisys {

UiHandles CreateControls(HWND parent, HINSTANCE inst) {
    UiHandles h;
    h.main = parent;

    h.tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_TABCTRL), inst, nullptr);

    h.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_LISTVIEW), inst, nullptr);
    ListView_SetExtendedListViewStyle(h.list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    auto AddCol = [&](int idx, int width, const wchar_t* title) {
        col.cx = width; col.pszText = const_cast<LPWSTR>(title);
        ListView_InsertColumn(h.list, idx, &col);
    };
    // v2.2 (REVIEW P0-6 / 07-X2): dedicated risk column — the risk level
    // data existed but was never rendered.
    AddCol(0, 180, L"分类");
    AddCol(1, 84, L"风险");
    AddCol(2, 330, L"项目");
    AddCol(3, 100, L"大小");
    AddCol(4, 320, L"详情");

    h.scan = CreateWindowExW(0, L"BUTTON", L"扫描",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_SCAN), inst, nullptr);
    h.exec = CreateWindowExW(0, L"BUTTON", L"执行选中操作",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_EXECUTE), inst, nullptr);
    h.undo = CreateWindowExW(0, L"BUTTON", L"撤销选中(历史)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_UNDO), inst, nullptr);
    h.emptyQ = CreateWindowExW(0, L"BUTTON", L"清空隔离区",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_EMPTY_Q), inst, nullptr);
    h.open = CreateWindowExW(0, L"BUTTON", L"打开所在位置",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_OPENLOC), inst, nullptr);
    h.targetBtn = CreateWindowExW(0, L"BUTTON", L"选择迁移目标盘…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_CHOOSE_TARGET), inst, nullptr);
    h.advancedChk = CreateWindowExW(0, L"BUTTON", L"高级模式: 使用 Symlink (默认 Junction)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_CHK_ADVANCED), inst, nullptr);
    h.info = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_LABEL_INFO), inst, nullptr);

    h.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_STATUSBAR), inst, nullptr);
    // Progress bar is a child of parent (not the status bar) so PBM_SETMARQUEE
    // works reliably.
    h.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | PBS_SMOOTH | PBS_MARQUEE,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_PROGRESSBAR), inst, nullptr);
    SendMessageW(h.progress, PBM_SETRANGE32, 0, 100);

    // ---- LargeFiles settings panel (hidden by default) ----
    h.lblMinSize = CreateWindowExW(0, L"STATIC", L"最小大小:",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, parent, nullptr, inst, nullptr);
    h.editMinSize = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"100",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_EDIT_MINSIZE), inst, nullptr);
    h.lblMinSizeUnit = CreateWindowExW(0, L"STATIC", L"MB",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, parent, nullptr, inst, nullptr);
    h.lblFileType = CreateWindowExW(0, L"STATIC", L"文件类型(.ext;...):",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, parent, nullptr, inst, nullptr);
    h.editFileType = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_EDIT_FILETYPE), inst, nullptr);
    h.lblDrives = CreateWindowExW(0, L"STATIC", L"扫描磁盘(C;D; 空=默认):",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, parent, nullptr, inst, nullptr);
    h.editDrives = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_EDIT_DRIVES), inst, nullptr);

    // ---- Sort buttons (shown for scan result tabs, hidden for History/FolderTree) ----
    h.btnSortSize = CreateWindowExW(0, L"BUTTON", L"按大小排序",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_SORT_SIZE), inst, nullptr);
    h.btnSortTime = CreateWindowExW(0, L"BUTTON", L"按时间排序",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_SORT_TIME), inst, nullptr);

    // ---- FolderTree TreeView ----
    h.tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_TREEVIEW), inst, nullptr);

    // ---- About (always visible, right-aligned by the layout) ----
    h.about = CreateWindowExW(0, L"BUTTON", L"关于…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_ABOUT), inst, nullptr);

    // Use the default GUI font (Segoe UI) for all controls.
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    for (HWND ctl : { h.tab, h.list, h.scan, h.exec, h.undo, h.emptyQ, h.open,
                      h.targetBtn, h.advancedChk, h.info, h.status,
                      h.lblMinSize, h.editMinSize, h.lblMinSizeUnit,
                      h.lblFileType, h.editFileType,
                      h.lblDrives, h.editDrives,
                      h.btnSortSize, h.btnSortTime, h.tree, h.about }) {
        SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    return h;
}

} // namespace minisys
