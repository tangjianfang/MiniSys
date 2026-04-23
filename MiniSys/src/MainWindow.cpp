#include "MainWindow.h"
#include "res/resource.h"

#include "core/JunkScanner.h"
#include "core/LargeFileScanner.h"
#include "core/AppScanner.h"
#include "core/FolderTreeScanner.h"
#include "core/DeleteOp.h"
#include "core/MoveJunctionOp.h"
#include "core/OperationLog.h"

#include "platform/Privilege.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace minisys {

namespace {

constexpr wchar_t kWindowClass[] = L"MiniSysMainWnd";
constexpr wchar_t kWindowTitle[] = L"MiniSys — C 盘瘦身助手";

const wchar_t* TabName(TabId t) {
    switch (t) {
        case TabId::Junk:       return L"垃圾清理";
        case TabId::LargeFiles: return L"大文件 / 去重";
        case TabId::Apps:       return L"应用迁移";
        case TabId::FolderTree: return L"文件夹分析";
        case TabId::History:    return L"操作历史";
        default:                return L"";
    }
}

} // namespace

bool MainWindow::Create(HINSTANCE hInst, int nCmdShow) {
    hInst_ = hInst;
    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
        ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"RegisterClassEx failed", L"MiniSys", MB_ICONERROR);
        return false;
    }
    hwnd_ = CreateWindowExW(0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        MessageBoxW(nullptr, L"CreateWindow failed", L"MiniSys", MB_ICONERROR);
        return false;
    }
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (scanThread_.joinable()) {
        cancelScan_.store(true);
        scanThread_.join();
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->WndProc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_SIZE:
            OnSize();
            return 0;
        case WM_NOTIFY: {
            auto nm = reinterpret_cast<LPNMHDR>(lp);
            if (nm->hwndFrom == hTab_ && nm->code == TCN_SELCHANGE) {
                OnTabChanged();
            } else if (nm->hwndFrom == hList_ && nm->code == LVN_COLUMNCLICK) {
                auto nmlv = reinterpret_cast<LPNMLISTVIEW>(lp);
                OnListColumnClick(nmlv->iSubItem);
            } else if (nm->hwndFrom == hTreeView_ && nm->code == NM_RCLICK) {
                OnTreeViewRClick();
            }
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_BTN_SCAN:    OnScan(); break;
                case IDC_BTN_EXECUTE: OnExecute(); break;
                case IDC_BTN_UNDO:    OnUndo(); break;
                case IDC_BTN_OPENLOC: OnOpenLocation(); break;
                case IDC_BTN_CHOOSE_TARGET: OnChooseTarget(); break;
                case IDC_BTN_SORT_SIZE: OnSortSize(); break;
                case IDC_BTN_SORT_TIME: OnSortTime(); break;
                case IDC_CHK_ADVANCED:
                    useSymlink_ = (Button_GetCheck(hAdvancedChk_) == BST_CHECKED);
                    break;
            }
            return 0;
        }
        case WM_APP_SCAN_PROGRESS:
            UpdateStatusBar();
            return 0;
        case WM_APP_SCAN_DONE:
            OnScanDone();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::OnCreate() {
    tabResults_.resize(static_cast<size_t>(TabId::Count));

    hTab_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TABCTRL), hInst_, nullptr);
    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        TCITEMW ti{}; ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(TabName(static_cast<TabId>(i)));
        TabCtrl_InsertItem(hTab_, i, &ti);
    }

    hList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_LISTVIEW), hInst_, nullptr);
    ListView_SetExtendedListViewStyle(hList_,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    auto AddCol = [&](int idx, int width, const wchar_t* title) {
        col.cx = width; col.pszText = const_cast<LPWSTR>(title);
        ListView_InsertColumn(hList_, idx, &col);
    };
    AddCol(0, 220, L"分类");
    AddCol(1, 380, L"项目");
    AddCol(2, 110, L"大小");
    AddCol(3, 320, L"详情");

    hScan_ = CreateWindowExW(0, L"BUTTON", L"扫描",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_SCAN), hInst_, nullptr);
    hExec_ = CreateWindowExW(0, L"BUTTON", L"执行选中操作",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_EXECUTE), hInst_, nullptr);
    hUndo_ = CreateWindowExW(0, L"BUTTON", L"撤销选中(历史)",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_UNDO), hInst_, nullptr);
    hOpen_ = CreateWindowExW(0, L"BUTTON", L"打开所在位置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_OPENLOC), hInst_, nullptr);
    hTargetBtn_ = CreateWindowExW(0, L"BUTTON", L"选择迁移目标盘…",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_CHOOSE_TARGET), hInst_, nullptr);
    hAdvancedChk_ = CreateWindowExW(0, L"BUTTON", L"高级模式: 使用 Symlink (默认 Junction)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_CHK_ADVANCED), hInst_, nullptr);
    hInfoLabel_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_LABEL_INFO), hInst_, nullptr);

    hStatus_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_STATUSBAR), hInst_, nullptr);
    hProgress_ = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0, 0, 0, 0, hStatus_, reinterpret_cast<HMENU>(IDC_PROGRESSBAR), hInst_, nullptr);
    SendMessageW(hProgress_, PBM_SETMARQUEE, FALSE, 0);

    // ---- LargeFiles settings panel (hidden by default) ----
    hLblMinSize_    = CreateWindowExW(0, L"STATIC", L"最小大小:",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
    hEditMinSize_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"100",
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_EDIT_MINSIZE), hInst_, nullptr);
    hLblMinSizeUnit_ = CreateWindowExW(0, L"STATIC", L"MB",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
    hLblFileType_   = CreateWindowExW(0, L"STATIC", L"文件类型(.ext;...):",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
    hEditFileType_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_EDIT_FILETYPE), hInst_, nullptr);
    hLblDrives_     = CreateWindowExW(0, L"STATIC", L"扫描磁盘(C;D; 空=默认):",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
    hEditDrives_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_EDIT_DRIVES), hInst_, nullptr);

    // ---- Sort buttons (shown for scan result tabs, hidden for History/FolderTree) ----
    hBtnSortSize_ = CreateWindowExW(0, L"BUTTON", L"按大小排序",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_SORT_SIZE), hInst_, nullptr);
    hBtnSortTime_ = CreateWindowExW(0, L"BUTTON", L"按时间排序",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_BTN_SORT_TIME), hInst_, nullptr);

    // ---- FolderTree TreeView ----
    hTreeView_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TREEVIEW), hInst_, nullptr);

    // Use the default GUI font (Segoe UI) for all controls.
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    for (HWND h : { hTab_, hList_, hScan_, hExec_, hUndo_, hOpen_,
                    hTargetBtn_, hAdvancedChk_, hInfoLabel_, hStatus_,
                    hLblMinSize_, hEditMinSize_, hLblMinSizeUnit_,
                    hLblFileType_, hEditFileType_,
                    hLblDrives_, hEditDrives_,
                    hBtnSortSize_, hBtnSortTime_, hTreeView_ }) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    OnTabChanged();
    UpdateStatusBar();
}

void MainWindow::OnSize() {
    RECT rc; GetClientRect(hwnd_, &rc);
    SendMessageW(hStatus_, WM_SIZE, 0, 0);
    RECT srect; GetClientRect(hStatus_, &srect);
    int statusH = srect.bottom - srect.top;
    int W = rc.right;
    int H = rc.bottom - statusH;

    constexpr int pad = 8;
    constexpr int btnH = 30;
    constexpr int btnW = 140;
    int tabH = 28;
    SetWindowPos(hTab_, nullptr, 0, 0, W, tabH, SWP_NOZORDER);

    int btnY = tabH + pad;
    SetWindowPos(hScan_,      nullptr, pad,                    btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(hExec_,      nullptr, pad + btnW + pad,       btnY, btnW + 20, btnH, SWP_NOZORDER);
    SetWindowPos(hUndo_,      nullptr, pad + 2*(btnW + pad)+20,btnY, btnW + 20, btnH, SWP_NOZORDER);
    SetWindowPos(hOpen_,      nullptr, pad + 3*(btnW + pad)+40,btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(hTargetBtn_, nullptr, pad + 4*(btnW + pad)+40,btnY, btnW + 40, btnH, SWP_NOZORDER);
    SetWindowPos(hAdvancedChk_,nullptr,pad + 5*(btnW + pad)+80,btnY, 360, btnH, SWP_NOZORDER);
    SetWindowPos(hBtnSortSize_,nullptr,pad + 4*(btnW + pad)+40,btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(hBtnSortTime_,nullptr,pad + 5*(btnW + pad)+40,btnY, btnW, btnH, SWP_NOZORDER);

    // LargeFiles settings row (below main buttons)
    int settingsY = btnY + btnH + pad;
    constexpr int editH   = 24;
    constexpr int lblH    = 22;
    constexpr int editW   = 70;   // size edit
    constexpr int editW2  = 200;  // filetype edit
    constexpr int editW3  = 80;   // drives edit
    constexpr int lblW1   = 76;
    constexpr int lblW2   = 150;
    constexpr int lblW3   = 190;
    int sx = pad;
    // "最小大小:" [edit] "MB"   "文件类型:" [edit]   "扫描磁盘:" [edit]
    SetWindowPos(hLblMinSize_,    nullptr, sx,                         settingsY+1, lblW1,  lblH, SWP_NOZORDER);
    SetWindowPos(hEditMinSize_,   nullptr, sx+lblW1+4,                 settingsY,   editW,  editH, SWP_NOZORDER);
    SetWindowPos(hLblMinSizeUnit_,nullptr, sx+lblW1+4+editW+4,         settingsY+1, 28,     lblH, SWP_NOZORDER);
    sx = sx + lblW1+4+editW+4+28+12;
    SetWindowPos(hLblFileType_,   nullptr, sx,                         settingsY+1, lblW2,  lblH, SWP_NOZORDER);
    SetWindowPos(hEditFileType_,  nullptr, sx+lblW2+4,                 settingsY,   editW2, editH, SWP_NOZORDER);
    sx = sx + lblW2+4+editW2+12;
    SetWindowPos(hLblDrives_,     nullptr, sx,                         settingsY+1, lblW3,  lblH, SWP_NOZORDER);
    SetWindowPos(hEditDrives_,    nullptr, sx+lblW3+4,                 settingsY,   editW3, editH, SWP_NOZORDER);

    int infoY = settingsY + editH + pad;
    SetWindowPos(hInfoLabel_, nullptr, pad, infoY, W - 2*pad, 20, SWP_NOZORDER);

    int contentY = infoY + 24;
    int contentH = H - contentY - pad;
    SetWindowPos(hList_,     nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);
    SetWindowPos(hTreeView_, nullptr, pad, contentY, W - 2*pad, contentH, SWP_NOZORDER);

    // Progress bar inside status bar
    RECT pr; pr.left = W - 240; pr.top = 2; pr.right = W - 24; pr.bottom = statusH - 2;
    SetWindowPos(hProgress_, nullptr, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, SWP_NOZORDER);
}

TabId MainWindow::CurrentTab() const {
    return static_cast<TabId>(TabCtrl_GetCurSel(hTab_));
}

void MainWindow::OnTabChanged() {
    auto t = CurrentTab();
    bool isHistory   = (t == TabId::History);
    bool isFolderTree = (t == TabId::FolderTree);
    bool isApps      = (t == TabId::Apps);
    bool isLargeFiles = (t == TabId::LargeFiles);
    bool isScanTab   = !isHistory && !isFolderTree;

    // Main action buttons
    ShowWindow(hScan_,  (isScanTab || isFolderTree) ? SW_SHOW : SW_HIDE);
    ShowWindow(hExec_,  isScanTab ? SW_SHOW : SW_HIDE);
    ShowWindow(hUndo_,  isHistory ? SW_SHOW : SW_HIDE);
    ShowWindow(hOpen_,  isScanTab ? SW_SHOW : SW_HIDE);

    // Apps-only controls
    ShowWindow(hTargetBtn_,  isApps ? SW_SHOW : SW_HIDE);
    ShowWindow(hAdvancedChk_, isApps ? SW_SHOW : SW_HIDE);

    // LargeFiles settings panel
    for (HWND h : { hLblMinSize_, hEditMinSize_, hLblMinSizeUnit_,
                    hLblFileType_, hEditFileType_, hLblDrives_, hEditDrives_ }) {
        ShowWindow(h, isLargeFiles ? SW_SHOW : SW_HIDE);
    }

    // Sort buttons (only on scan result tabs that have a sortable list, not Apps)
    bool showSort = isScanTab && !isApps;
    ShowWindow(hBtnSortSize_, showSort ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnSortTime_, showSort ? SW_SHOW : SW_HIDE);

    // Content areas
    ShowWindow(hList_,     !isFolderTree ? SW_SHOW : SW_HIDE);
    ShowWindow(hTreeView_, isFolderTree  ? SW_SHOW : SW_HIDE);

    // Reset sort state when changing tabs
    sortCol_ = -1;
    sortAsc_ = false;

    switch (t) {
        case TabId::Junk:
            SetWindowTextW(hInfoLabel_, L"扫描系统/浏览器/Windows Update 等可清理项；删除走回收站，可在系统回收站还原。");
            break;
        case TabId::LargeFiles:
            SetWindowTextW(hInfoLabel_, L"大小≥指定MB，可按文件类型(.ext;..)和磁盘过滤；点击列标题或排序按钮排序。");
            break;
        case TabId::Apps:
            SetWindowTextW(hInfoLabel_, L"扫描 C 盘已安装应用；勾选并点击执行将复制到目标盘并在原位置创建 Junction。");
            break;
        case TabId::FolderTree:
            SetWindowTextW(hInfoLabel_, L"按磁盘显示顶层文件夹及大小；右键文件夹可删除。");
            break;
        case TabId::History:
            SetWindowTextW(hInfoLabel_, L"操作历史。可对支持撤销的迁移记录执行撤销。");
            RefreshHistory();
            break;
        default: break;
    }
    if (!isFolderTree) RefreshListView();
    UpdateStatusBar();
}

void MainWindow::RefreshListView() {
    ListView_DeleteAllItems(hList_);
    auto t = CurrentTab();
    if (t == TabId::History) {
        auto recs = OperationLog::Instance().LoadAll();
        for (size_t i = 0; i < recs.size(); ++i) {
            const auto& r = recs[i];
            LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = static_cast<int>(i);
            std::wstring c0 = OperationLog::TypeToStr(r.type) + L" / " + OperationLog::StatusToStr(r.status);
            it.pszText = c0.data();
            it.lParam = static_cast<LPARAM>(i);
            int row = ListView_InsertItem(hList_, &it);
            ListView_SetItemText(hList_, row, 1, const_cast<LPWSTR>(r.timestamp.c_str()));
            std::wstring sz = FormatSize(r.sizeBytes);
            ListView_SetItemText(hList_, row, 2, sz.data());
            std::wstring detail = r.source;
            if (!r.target.empty()) detail += L"  →  " + r.target;
            if (!r.note.empty())   detail += L"  | " + r.note;
            ListView_SetItemText(hList_, row, 3, detail.data());
        }
        return;
    }
    auto& items = tabResults_[static_cast<size_t>(t)];
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        LVITEMW lvi{}; lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = static_cast<int>(i);
        lvi.pszText = const_cast<LPWSTR>(it.category.c_str());
        lvi.lParam  = static_cast<LPARAM>(i);
        int row = ListView_InsertItem(hList_, &lvi);
        ListView_SetItemText(hList_, row, 1, const_cast<LPWSTR>(it.title.c_str()));
        std::wstring sz = FormatSize(it.sizeBytes);
        ListView_SetItemText(hList_, row, 2, sz.data());
        ListView_SetItemText(hList_, row, 3, const_cast<LPWSTR>(it.detail.c_str()));
        if (it.recommended) ListView_SetCheckState(hList_, row, TRUE);
    }
}

void MainWindow::UpdateStatusBar() {
    DiskSpace ds;
    std::wstring msg;
    if (QueryDiskSpace(SystemDriveRoot(), ds)) {
        msg = FormatW(L"  系统盘 %s : 可用 %s / 共 %s   |   ",
            SystemDriveRoot().c_str(),
            FormatSize(ds.freeBytes).c_str(),
            FormatSize(ds.totalBytes).c_str());
    }
    if (!migrateTargetRoot_.empty()) {
        msg += FormatW(L"迁移目标根: %s   |   ", migrateTargetRoot_.c_str());
    }
    {
        std::lock_guard<std::mutex> g(progressMu_);
        if (!scanProgressText_.empty()) {
            msg += scanProgressText_;
        }
    }
    SendMessageW(hStatus_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(msg.c_str()));
    SendMessageW(hProgress_, PBM_SETMARQUEE, scanning_.load(), 30);
}

void MainWindow::AppendLog(const std::wstring& msg) {
    MS_LOG_INFO(L"%s", msg.c_str());
}

void MainWindow::OnScan() {
    if (scanning_.load()) {
        MessageBoxW(hwnd_, L"已有扫描在进行中。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto t = CurrentTab();
    std::unique_ptr<Scanner> s;
    switch (t) {
        case TabId::Junk:       s = std::make_unique<JunkScanner>(); break;
        case TabId::LargeFiles: {
            LargeFileScanner::Config cfg;

            // -- Min size (MB) --
            wchar_t szBuf[32] = {};
            GetWindowTextW(hEditMinSize_, szBuf, 32);
            unsigned long long mb = _wtoi(szBuf);
            if (mb == 0) mb = 100;
            cfg.minBytes = mb * 1024ULL * 1024ULL;

            // -- Extension filter --
            wchar_t extBuf[512] = {};
            GetWindowTextW(hEditFileType_, extBuf, 512);
            std::wstring extStr(extBuf);
            if (!extStr.empty()) {
                // Split by ";" and normalize each token
                std::wstring tok;
                for (auto ch : extStr) {
                    if (ch == L';' || ch == L',') {
                        if (!tok.empty()) {
                            if (tok[0] != L'.') tok = L'.' + tok;
                            cfg.extFilter.push_back(ToLower(tok));
                            tok.clear();
                        }
                    } else if (ch != L' ') {
                        tok += ch;
                    }
                }
                if (!tok.empty()) {
                    if (tok[0] != L'.') tok = L'.' + tok;
                    cfg.extFilter.push_back(ToLower(tok));
                }
            }

            // -- Drive filter --
            wchar_t drvBuf[128] = {};
            GetWindowTextW(hEditDrives_, drvBuf, 128);
            std::wstring drvStr(drvBuf);
            if (!drvStr.empty()) {
                cfg.roots.clear();
                std::wstring token;
                for (auto ch : drvStr) {
                    if (ch == L';' || ch == L',' || ch == L' ') {
                        if (!token.empty()) {
                            // Accept "C" or "C:" or "C:\"
                            wchar_t letter = ::towupper(token[0]);
                            std::wstring root = {letter, L':', L'\\'};
                            cfg.roots.push_back(root);
                            token.clear();
                        }
                    } else {
                        token += ch;
                    }
                }
                if (!token.empty()) {
                    wchar_t letter = ::towupper(token[0]);
                    cfg.roots.push_back(std::wstring{letter, L':', L'\\'});
                }
            }

            s = std::make_unique<LargeFileScanner>(std::move(cfg));
            break;
        }
        case TabId::Apps: s = std::make_unique<AppScanner>(); break;
        case TabId::FolderTree: {
            // Populate drives from hEditDrives_ if it were available on this tab
            // (FolderTree tab doesn't show that edit, scan all fixed drives)
            s = std::make_unique<FolderTreeScanner>();
            break;
        }
        default: return;
    }
    StartScannerAsync(std::move(s));
}

void MainWindow::StartScannerAsync(std::unique_ptr<Scanner> scanner) {
    if (scanThread_.joinable()) scanThread_.join();
    scanBuffer_.clear();
    cancelScan_.store(false);
    scanning_.store(true);
    {
        std::lock_guard<std::mutex> g(progressMu_);
        scanProgressText_ = L"扫描中…";
    }
    UpdateStatusBar();

    HWND hwnd = hwnd_;
    Scanner* raw = scanner.release();
    scanThread_ = std::thread([this, hwnd, raw]() {
        std::unique_ptr<Scanner> own(raw);
        try {
            own->Scan(scanBuffer_, [this, hwnd](unsigned long long /*cur*/, unsigned long long /*tot*/, const std::wstring& msg) {
                {
                    std::lock_guard<std::mutex> g(progressMu_);
                    scanProgressText_ = L"扫描: " + msg;
                }
                PostMessageW(hwnd, WM_APP_SCAN_PROGRESS, 0, 0);
            }, cancelScan_);
        } catch (const std::exception& ex) {
            MS_LOG_ERROR(L"Scanner threw: %hs", ex.what());
        } catch (...) {
            MS_LOG_ERROR(L"Scanner threw unknown exception");
        }
        scanning_.store(false);
        PostMessageW(hwnd, WM_APP_SCAN_DONE, 0, 0);
    });
}

void MainWindow::OnScanDone() {
    if (scanThread_.joinable()) scanThread_.join();
    auto t = CurrentTab();
    if (t < TabId::History) {
        tabResults_[static_cast<size_t>(t)] = std::move(scanBuffer_);
    }
    {
        std::lock_guard<std::mutex> g(progressMu_);
        scanProgressText_ = FormatW(L"扫描完成: %zu 项",
            tabResults_[static_cast<size_t>(t)].size());
    }
    if (t == TabId::FolderTree) {
        BuildFolderTree();
    } else {
        RefreshListView();
    }
    UpdateStatusBar();
}

void MainWindow::OnExecute() {
    auto t = CurrentTab();
    if (t == TabId::History) return;
    auto& items = tabResults_[static_cast<size_t>(t)];
    if (items.empty()) {
        MessageBoxW(hwnd_, L"列表为空，请先扫描。", L"提示", MB_OK | MB_ICONINFORMATION); return;
    }
    int n = ListView_GetItemCount(hList_);
    std::vector<size_t> selected;
    unsigned long long totalSize = 0;
    bool hasDangerous = false;
    for (int i = 0; i < n; ++i) {
        if (!ListView_GetCheckState(hList_, i)) continue;
        LVITEMW lvi{}; lvi.iItem = i; lvi.mask = LVIF_PARAM;
        ListView_GetItem(hList_, &lvi);
        size_t idx = static_cast<size_t>(lvi.lParam);
        if (idx >= items.size()) continue;
        selected.push_back(idx);
        totalSize += items[idx].sizeBytes;
        if (items[idx].dangerous) hasDangerous = true;
    }
    if (selected.empty()) {
        MessageBoxW(hwnd_, L"请勾选要操作的项目。", L"提示", MB_OK | MB_ICONINFORMATION); return;
    }
    std::wstring action = (t == TabId::Apps) ? L"迁移" : L"删除(进回收站)";
    std::wstring confirm = FormatW(
        L"将对 %zu 项执行【%s】操作，合计 %s。\n\n%s",
        selected.size(), action.c_str(), FormatSize(totalSize).c_str(),
        hasDangerous ? L"⚠ 部分项目标记为危险/不可撤销操作（如清空回收站）。\n\n确定继续？"
                     : L"确定继续？");
    if (MessageBoxW(hwnd_, confirm.c_str(), L"确认操作",
            MB_OKCANCEL | (hasDangerous ? MB_ICONWARNING : MB_ICONQUESTION)) != IDOK) {
        return;
    }

    if (t == TabId::Apps) {
        if (migrateTargetRoot_.empty()) {
            MessageBoxW(hwnd_, L"请先点击『选择迁移目标盘…』指定目标根目录。",
                L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    int success = 0, fail = 0;
    std::wstring report;
    for (auto idx : selected) {
        const auto& it = items[idx];
        std::wstring err;
        bool ok = false;
        if (t == TabId::Apps) {
            std::filesystem::path target = std::filesystem::path(migrateTargetRoot_) / it.path.filename();
            MoveJunctionOp op(it.path, target, useSymlink_);
            ok = op.Execute(err);
        } else if (t == TabId::Junk &&
                   it.category == L"Recycle Bin") {
            EmptyRecycleOp op;
            ok = op.Execute(err);
        } else {
            DeleteOp op(it.path, it.sizeBytes);
            ok = op.Execute(err);
        }
        if (ok) ++success;
        else {
            ++fail;
            report += FormatW(L"\n× %s: %s", it.title.c_str(), err.c_str());
        }
    }
    auto summary = FormatW(L"完成: 成功 %d, 失败 %d.%s", success, fail, report.c_str());
    MessageBoxW(hwnd_, summary.c_str(), L"操作完成",
        MB_OK | (fail ? MB_ICONWARNING : MB_ICONINFORMATION));
    UpdateStatusBar();
}

void MainWindow::OnUndo() {
    if (CurrentTab() != TabId::History) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0) {
        MessageBoxW(hwnd_, L"请选中一条历史记录。", L"提示", MB_OK | MB_ICONINFORMATION); return;
    }
    auto recs = OperationLog::Instance().LoadAll();
    if (sel >= static_cast<int>(recs.size())) return;
    const auto& r = recs[sel];
    if (!r.isReversible || r.status != OpStatus::Success) {
        MessageBoxW(hwnd_, L"该记录不可撤销。", L"提示", MB_OK | MB_ICONWARNING); return;
    }
    if (r.type != OpType::MoveAndJunction) {
        MessageBoxW(hwnd_,
            L"删除类操作请通过 Windows 回收站手动恢复（v1 仅自动撤销迁移类操作）。",
            L"提示", MB_OK | MB_ICONINFORMATION); return;
    }
    MoveJunctionOp op(r.source, r.target, false);
    op.MutableRecord().id = r.id;
    std::wstring err;
    if (op.Undo(err)) {
        MessageBoxW(hwnd_, L"已撤销。", L"完成", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd_, (L"撤销失败: " + err).c_str(), L"错误", MB_OK | MB_ICONERROR);
    }
    RefreshHistory();
    UpdateStatusBar();
}

void MainWindow::OnOpenLocation() {
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0) return;
    auto t = CurrentTab();
    if (t == TabId::History) return;
    auto& items = tabResults_[static_cast<size_t>(t)];
    LVITEMW lvi{}; lvi.iItem = sel; lvi.mask = LVIF_PARAM;
    ListView_GetItem(hList_, &lvi);
    size_t idx = static_cast<size_t>(lvi.lParam);
    if (idx >= items.size()) return;
    auto path = items[idx].path;
    std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::OnChooseTarget() {
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"选择迁移目标根目录 (例如 D:\\MigratedApps)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t buf[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, buf)) {
        migrateTargetRoot_ = buf;
        if (IsOnSystemDrive(migrateTargetRoot_)) {
            MessageBoxW(hwnd_,
                L"目标在系统盘，迁移无意义。已忽略选择。",
                L"提示", MB_OK | MB_ICONWARNING);
            migrateTargetRoot_.clear();
        }
    }
    CoTaskMemFree(pidl);
    UpdateStatusBar();
}

void MainWindow::RefreshHistory() {
    RefreshListView();
}

void MainWindow::OnScanProgress(const std::wstring&) {}

// ---- New feature implementations ----

void MainWindow::OnSortSize() {
    auto t = CurrentTab();
    if (t == TabId::History || t == TabId::FolderTree) return;
    if (sortCol_ == 0) sortAsc_ = !sortAsc_;
    else { sortCol_ = 0; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void MainWindow::OnSortTime() {
    auto t = CurrentTab();
    if (t == TabId::History || t == TabId::FolderTree) return;
    if (sortCol_ == 1) sortAsc_ = !sortAsc_;
    else { sortCol_ = 1; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void MainWindow::OnListColumnClick(int col) {
    auto t = CurrentTab();
    if (t == TabId::History || t == TabId::FolderTree) return;
    // col 2 = size, col 3 = detail; map col 2 -> sort size, others -> sort time
    int sortKey = (col == 2) ? 0 : 1;
    if (sortCol_ == sortKey) sortAsc_ = !sortAsc_;
    else { sortCol_ = sortKey; sortAsc_ = false; }
    ApplySortAndRefresh();
}

void MainWindow::ApplySortAndRefresh() {
    auto t = CurrentTab();
    if (t >= TabId::History) return;
    auto& items = tabResults_[static_cast<size_t>(t)];
    if (items.empty()) return;

    bool asc = sortAsc_;
    if (sortCol_ == 0) {
        std::stable_sort(items.begin(), items.end(), [asc](const ScanItem& a, const ScanItem& b) {
            return asc ? (a.sizeBytes < b.sizeBytes) : (a.sizeBytes > b.sizeBytes);
        });
    } else if (sortCol_ == 1) {
        std::stable_sort(items.begin(), items.end(), [asc](const ScanItem& a, const ScanItem& b) {
            return asc ? (a.createTime < b.createTime) : (a.createTime > b.createTime);
        });
    }
    RefreshListView();
}

void MainWindow::BuildFolderTree() {
    TreeView_DeleteAllItems(hTreeView_);
    treeItemPaths_.clear();

    auto& items = tabResults_[static_cast<size_t>(TabId::FolderTree)];
    if (items.empty()) return;

    std::wstring lastDrive;
    HTREEITEM hDriveNode = nullptr;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];

        // Insert drive node when drive changes
        if (it.category != lastDrive) {
            lastDrive = it.category;
            DiskSpace ds{};
            std::wstring driveLabel = it.category;
            if (QueryDiskSpace(it.category, ds)) {
                driveLabel = FormatW(L"%s  [可用 %s / 共 %s]",
                    it.category.c_str(),
                    FormatSize(ds.freeBytes).c_str(),
                    FormatSize(ds.totalBytes).c_str());
            }
            TVINSERTSTRUCTW tvis{};
            tvis.hParent      = TVI_ROOT;
            tvis.hInsertAfter = TVI_LAST;
            tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
            tvis.item.pszText = driveLabel.data();
            tvis.item.lParam  = -1;
            hDriveNode = TreeView_InsertItem(hTreeView_, &tvis);
        }

        // Insert folder node
        std::wstring label = FormatW(L"%s  (%s)", it.title.c_str(), FormatSize(it.sizeBytes).c_str());
        TVINSERTSTRUCTW tvis{};
        tvis.hParent      = hDriveNode;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = label.data();
        tvis.item.lParam  = static_cast<LPARAM>(i);
        HTREEITEM hItem = TreeView_InsertItem(hTreeView_, &tvis);
        if (hItem) treeItemPaths_[hItem] = it.path;
    }

    // Expand all drive nodes
    HTREEITEM h = TreeView_GetRoot(hTreeView_);
    while (h) {
        TreeView_Expand(hTreeView_, h, TVE_EXPAND);
        h = TreeView_GetNextSibling(hTreeView_, h);
    }
}

void MainWindow::OnTreeViewRClick() {
    // Get the item under the cursor
    DWORD pos = GetMessagePos();
    POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    POINT ptClient = pt;
    ScreenToClient(hTreeView_, &ptClient);

    TVHITTESTINFO hti{};
    hti.pt = ptClient;
    HTREEITEM hItem = TreeView_HitTest(hTreeView_, &hti);
    if (!hItem) return;

    // Drive nodes have lParam == -1; only folder nodes are deletable
    auto it = treeItemPaths_.find(hItem);
    if (it == treeItemPaths_.end()) return;

    auto folderPath = it->second;

    // Select the item
    TreeView_SelectItem(hTreeView_, hItem);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_DELETE, L"删除文件夹…");
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_DELETE) {
        std::wstring msg = FormatW(L"确定要删除文件夹:\n%s\n\n此操作将移动到回收站。",
            folderPath.wstring().c_str());
        if (MessageBoxW(hwnd_, msg.c_str(), L"确认删除",
                MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;

        unsigned long long sz = 0;
        auto& ftItems = tabResults_[static_cast<size_t>(TabId::FolderTree)];
        for (auto& si : ftItems) {
            if (si.path == folderPath) { sz = si.sizeBytes; break; }
        }

        DeleteOp op(folderPath, sz);
        std::wstring err;
        if (op.Execute(err)) {
            // Remove node from tree
            TreeView_DeleteItem(hTreeView_, hItem);
            treeItemPaths_.erase(it);
        } else {
            MessageBoxW(hwnd_, (L"删除失败: " + err).c_str(), L"错误", MB_OK | MB_ICONERROR);
        }
    }
}

} // namespace minisys
