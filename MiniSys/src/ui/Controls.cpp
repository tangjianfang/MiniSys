#include "ui/Controls.h"
#include "res/resource.h"
#include "ui/Layout.h"

#include <commctrl.h>
#include <initializer_list>

namespace minisys {

UiHandles CreateControls(HWND parent, HINSTANCE inst) {
    UiHandles h;
    h.main = parent;

    h.tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_TABCTRL), inst, nullptr);
    // v2.19 (user image #8): wider item padding so the BOLD selected-tab
    // text (NM_CUSTOMDRAW in MainWindow) doesn't clip against its
    // neighbour — the default 6px was cut for the bold advance width.
    SendMessageW(h.tab, TCM_SETPADDING, 0, MAKELPARAM(UiScale(parent, 11), 3));

    // review-07 X-1: WS_TABSTOP on the list/tree (and the settings edits
    // below) — Tab used to cycle only the buttons, the 1000-row result
    // list was keyboard-unreachable.
    // v2.15 (round-4 02 P0): VIRTUAL list — LVM_SETITEMCOUNT instead of
    // per-row inserts (1000 rows: 1.26 s → ~70 ms; 100k rows same price).
    h.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_OWNERDATA | LVS_REPORT |
            LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_LISTVIEW), inst, nullptr);
    // REVIEW-UI P2 (L-11): LABELTIP — truncated cells (the detail column's
    // long paths) get a full-text tooltip instead of a silent clip.
    ListView_SetExtendedListViewStyle(h.list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
        LVS_EX_LABELTIP | LVS_EX_INFOTIP);
    // v2.21 (简洁美观): airier rows — a report-view ListView's row height
    // follows its small-image-list height. An empty 1px-wide list buys
    // +5px of breathing room per row without shifting any text (no
    // LVIF_IMAGE is ever served, so no icon slot is reserved).
    if (HIMAGELIST himl = ImageList_Create(1, UiScale(parent, 22),
                                            ILC_COLOR32, 1, 0)) {
        ListView_SetImageList(h.list, himl, LVSIL_SMALL);
    }

    LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    auto AddCol = [&](int idx, int width, const wchar_t* title) {
        col.cx = UiScale(parent, width); col.pszText = const_cast<LPWSTR>(title);
        col.fmt = (wcscmp(title, L"大小") == 0) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(h.list, idx, &col);
    };
    // v2.2 (REVIEW P0-6 / 07-X2): dedicated risk column — the risk level
    // data existed but was never rendered. v2.5: widths are DPI-scaled
    // (L-12) and the risk column is wide enough for "⚠ 系统组件" (L-14).
    AddCol(0, 180, L"分类");
    AddCol(1, 110, L"风险");
    AddCol(2, 330, L"项目");
    AddCol(3, 100, L"大小");
    AddCol(4, 320, L"详情");

    h.scan = CreateWindowExW(0, L"BUTTON", L"扫描",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_SCAN), inst, nullptr);
    // v2.21 (简洁美观): the PRIMARY action is the window's single accent —
    // owner-drawn (MainWindow DrawPrimaryButton) with an accent fill and
    // white bold text; BS_DEFPUSHBUTTON's thin border said nothing.
    h.exec = CreateWindowExW(0, L"BUTTON", L"执行选中操作",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
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
    h.advancedChk = CreateWindowExW(0, L"BUTTON", L"高级模式(Symlink)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_CHK_ADVANCED), inst, nullptr);
    // v2.19 (user image #8 “UI上的文本没有排版”): owner-drawn — MainWindow's
    // WM_DRAWITEM handler gives the strip real typography (bold summary
    // line, grey hints, amber ⚠ lines, breathing room between lines).
    h.info = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_OWNERDRAW,
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
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_EDIT_FILETYPE), inst, nullptr);
    h.lblDrives = CreateWindowExW(0, L"STATIC", L"扫描磁盘(C;D; 空=默认):",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, parent, nullptr, inst, nullptr);
    h.editDrives = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_EDIT_DRIVES), inst, nullptr);

    // ---- v2.3: instant search row (hidden by default, search tab only) ----
    h.lblSearch = CreateWindowExW(0, L"STATIC", L"搜索:",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, parent, nullptr, inst, nullptr);
    h.editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_EDIT_SEARCH), inst, nullptr);
    h.chkMatchPath = CreateWindowExW(0, L"BUTTON", L"匹配完整路径",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_CHK_MATCHPATH), inst, nullptr);
    // v2.6: one-click filter presets (popup menu) — .pdb / .obj / Debug
    // folders etc. without remembering the query syntax.
    h.btnQuickFilter = CreateWindowExW(0, L"BUTTON", L"快速筛选 ▾",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_BTN_QUICKFILTER), inst, nullptr);
    // REVIEW-UI P2 (L-16): cue banner documents the query syntax right in
    // the box (visible while focused too).
    SendMessageW(h.editSearch, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     L"输入关键词，或点右侧“快速筛选”一键过滤"));

    // ---- FolderTree TreeView ----
    h.tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
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
                      h.lblSearch, h.editSearch, h.chkMatchPath,
                      h.btnQuickFilter, h.tree, h.about }) {
        SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    return h;
}

} // namespace minisys
