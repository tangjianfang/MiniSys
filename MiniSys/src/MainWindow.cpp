#include "MainWindow.h"
#include "res/resource.h"

#include "core/DeleteOp.h"
#include "core/GuardRails.h"
#include "core/JunkRules.h"
#include "core/PlanBuilder.h"
#include "core/SessionService.h"
#include "core/VolumeIndex.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Layout.h"

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
        ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = reinterpret_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, 0));
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
    // REVIEW P2 (07-X12): keyboard accelerators — F5 = scan/cancel,
    // Ctrl+A = toggle-select all rows. IsDialogMessage gives Tab-order.
    ACCEL acc[] = {
        { FVIRTKEY, VK_F5,      IDC_BTN_SCAN },
        { FVIRTKEY | FCONTROL, 'A', IDC_ACCEL_SELECTALL },
    };
    HACCEL hAccel = CreateAcceleratorTableW(acc, 2);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (hAccel && TranslateAcceleratorW(hwnd_, hAccel, &msg)) continue;
        // Tab navigation across child controls.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
            HWND focus = GetFocus();
            HWND next = GetNextDlgTabItem(hwnd_, focus, (GetKeyState(VK_SHIFT) & 0x8000) ? TRUE : FALSE);
            if (next) { SetFocus(next); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    SessionService::Instance().Shutdown();
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
            if (nm->hwndFrom == h_.tab && nm->code == TCN_SELCHANGE) {
                OnTabChanged();
            } else if (nm->hwndFrom == h_.list &&
                       nm->code == LVN_ITEMCHANGED &&
                       taskMode_ == TaskMode::None) {
                // Live "执行选中操作（N 项 · X）" (REVIEW P0-6).
                auto* nmlv = reinterpret_cast<LPNMLISTVIEW>(lp);
                if (nmlv->uChanged & LVIF_STATE) UpdateExecButton();
            } else if (nm->hwndFrom == h_.list &&
                       nm->code == LVN_ITEMACTIVATE) {
                // REVIEW P1-6: double-click = "why is this here" panel.
                auto* nmia = reinterpret_cast<LPNMITEMACTIVATE>(lp);
                if (auto* p = ActivePresenter()) {
                    p->OnItemActivated(nmia->iItem);
                }
            } else if (nm->hwndFrom == h_.list && nm->code == NM_CUSTOMDRAW) {
                // REVIEW P3: risk-tinted rows (07-X2 / 3-B) — text colour by
                // risk level from the presenter's UI-private snapshot.
                auto* lpnmcd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lp);
                switch (lpnmcd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT:
                        return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT: {
                        auto* p = dynamic_cast<ListTabPresenter*>(ActivePresenter());
                        const ScanItem* it = p ? p->ItemAtRow(
                            static_cast<int>(lpnmcd->nmcd.dwItemSpec)) : nullptr;
                        if (it) {
                            if (it->riskLevel == RiskLevel::Advanced) {
                                lpnmcd->clrText = RGB(0xB2, 0x50, 0x00);
                            } else if (it->riskLevel == RiskLevel::InfoOnly) {
                                lpnmcd->clrText = RGB(0x88, 0x88, 0x88);
                            } else if (it->riskLevel == RiskLevel::Cautious &&
                                       !it->ruleId.empty()) {
                                lpnmcd->clrText = RGB(0x70, 0x5A, 0x00);
                            }
                        }
                        return CDRF_DODEFAULT;
                    }
                }
            } else if (nm->hwndFrom == h_.list && nm->code == LVN_COLUMNCLICK) {
                auto nmlv = reinterpret_cast<LPNMLISTVIEW>(lp);
                if (auto* p = ActivePresenter()) p->OnColumnClick(nmlv->iSubItem);
            } else if (nm->hwndFrom == h_.tree && nm->code == NM_RCLICK) {
                if (auto* p = ActivePresenter()) {
                    auto* ftp = dynamic_cast<FolderTreePresenter*>(p);
                    if (ftp) ftp->OnContextMenu();
                }
            } else if (nm->hwndFrom == h_.list && nm->code == NM_RCLICK) {
                OnListContextMenu();   // REVIEW P2
            } else if (nm->hwndFrom == h_.tree && nm->code == NM_DBLCLK) {
                // REVIEW P3: double-click a folder = drill into it.
                if (auto* p = ActivePresenter()) {
                    auto* ftp = dynamic_cast<FolderTreePresenter*>(p);
                    if (!ftp) return 0;
                    DWORD pos = GetMessagePos();
                    POINT ptClient{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
                    ScreenToClient(h_.tree, &ptClient);
                    TVHITTESTINFO hti{}; hti.pt = ptClient;
                    HTREEITEM hItem = TreeView_HitTest(h_.tree, &hti);
                    if (!hItem) return 0;
                    // Drive nodes have lParam == -1 → drill not available.
                    if (hti.flags & TVHT_ONITEMLABEL) {
                        auto path = ftp->FocusRoot();   // read current state
                        (void)path;
                        // Fetch the item's path from the presenter map via
                        // right-click machinery: simplest is to reuse the
                        // tree item's stored path through a small query.
                        TVITEMEXW tvi{};
                        tvi.hItem = hItem;
                        tvi.mask = TVIF_PARAM;
                        TreeView_GetItem(h_.tree, &tvi);
                        if (tvi.lParam != -1) {
                            auto items = SessionService::Instance().Results(TabId::FolderTree);
                            auto idx = static_cast<size_t>(tvi.lParam);
                            if (idx < items.size()) {
                                ftp->SetFocusRoot(items[idx].path);
                                OnScan();   // scan the drilled subtree
                            }
                        }
                    }
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_BTN_SCAN:    OnScan(); break;
                case IDC_BTN_EXECUTE: OnExecute(); break;
                case IDC_BTN_UNDO:
                    if (CurrentTab() == TabId::History) {
                        static_cast<HistoryPresenter*>(
                            presenters_[static_cast<size_t>(TabId::History)].get())
                            ->UndoSelected();
                    }
                    break;
                case IDC_BTN_OPENLOC: OnOpenLocation(); break;
                case IDC_BTN_EMPTY_Q: OnEmptyQuarantine(); break;
                case IDC_BTN_CHOOSE_TARGET: OnChooseTarget(); break;
                case IDC_BTN_SORT_SIZE:
                    if (auto* p = ActivePresenter()) p->SortBySize();
                    break;
                case IDC_BTN_SORT_TIME:
                    if (auto* p = ActivePresenter()) p->SortByTime();
                    break;
                case IDC_BTN_ABOUT:
                    OnAbout();
                    break;
                case IDC_CHK_ADVANCED:
                    useSymlink_ = (Button_GetCheck(h_.advancedChk) == BST_CHECKED);
                    SaveSettings();
                    break;
                case IDC_ACCEL_SELECTALL:
                    OnSelectAll();
                    break;
                // List context menu (REVIEW P2 / 07-X24).
                case IDM_LIST_OPEN:      OnOpenLocation(); break;
                case IDM_LIST_INFO:
                    if (auto* p = ActivePresenter()) {
                        int sel = ListView_GetNextItem(h_.list, -1, LVNI_SELECTED);
                        if (sel >= 0) p->OnItemActivated(sel);
                    }
                    break;
                case IDM_LIST_SELECTALL:
                case IDM_LIST_SELECTNONE: {
                    bool on = (LOWORD(wp) == IDM_LIST_SELECTALL);
                    int n = ListView_GetItemCount(h_.list);
                    for (int i = 0; i < n; ++i) {
                        ListView_SetCheckState(h_.list, i, on ? TRUE : FALSE);
                    }
                    break;
                }
                case IDM_LIST_PREVIEW:   OnPreviewExecution(); break;
                case IDM_LIST_EXCLUDE:   OnExcludeSelected(); break;
            }
            return 0;
        }
        case WM_APP_SCAN_PROGRESS:
            UpdateStatusBar();
            return 0;
        case WM_APP_OP_PROGRESS:
            // Determinate execution progress (REVIEW P0-5 / 07-X8).
            SendMessageW(h_.progress, PBM_SETPOS, static_cast<int>(wp), 0);
            UpdateStatusBar();
            return 0;
        case WM_APP_SCAN_DONE:
            OnScanDone();
            return 0;
        case WM_APP_TASK_STARTED:
            // An ExecutePlan launched outside OnExecute (folder-tree right
            // click) — lock the action matrix for it.
            SetTaskBusy(TaskMode::Executing);
            UpdateStatusBar();
            return 0;
        case WM_APP_OP_DONE:
            OnPlanDone();
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
    h_ = CreateControls(hwnd_, hInst_);

    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        TCITEMW ti{}; ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(TabName(static_cast<TabId>(i)));
        TabCtrl_InsertItem(h_.tab, i, &ti);
    }

    // ---- presenters + service wiring ----
    auto& svc = SessionService::Instance();
    svc.SetWindow(hwnd_);
    presenters_[static_cast<size_t>(TabId::Junk)]        = std::make_unique<JunkPresenter>(TabId::Junk, h_);
    presenters_[static_cast<size_t>(TabId::LargeFiles)]  = std::make_unique<LargeFilesPresenter>(TabId::LargeFiles, h_);
    presenters_[static_cast<size_t>(TabId::Apps)]        = std::make_unique<AppsPresenter>(TabId::Apps, h_);
    presenters_[static_cast<size_t>(TabId::FolderTree)]  = std::make_unique<FolderTreePresenter>(h_, svc);
    presenters_[static_cast<size_t>(TabId::History)]     = std::make_unique<HistoryPresenter>(h_, svc);

    // ---- classic stock icons on the action buttons ----
    // REVIEW P2 (07-X13): the exec button deliberately has NO icon — the
    // operation is "move to quarantine", not delete; SIID_DELETE misled.
    using icons::SetStockButtonIcon;
    SetStockButtonIcon(h_.scan,      SIID_FIND);         // 扫描 = 放大镜
    SetStockButtonIcon(h_.undo,      SIID_FOLDERBACK);   // 撤销 = 回退箭头
    SetStockButtonIcon(h_.emptyQ,    SIID_RECYCLERFULL); // 清空隔离区 = 回收站
    SetStockButtonIcon(h_.open,      SIID_FOLDEROPEN);   // 打开位置 = 打开的文件夹
    SetStockButtonIcon(h_.targetBtn, SIID_DRIVEFIXED);   // 迁移目标 = 硬盘
    SetStockButtonIcon(h_.about,     SIID_HELP);         // 关于 = 问号

    OnTabChanged();
    LoadSettings();   // REVIEW P2: restore persisted state + exclusions
    UpdateStatusBar();
    UpdateExecButton();
}

void MainWindow::OnSize() {
    RECT rc; GetClientRect(hwnd_, &rc);
    LayoutWindow(h_, rc.right, rc.bottom,
                 CurrentTab() == TabId::LargeFiles);
}

TabId MainWindow::CurrentTab() const {
    return static_cast<TabId>(TabCtrl_GetCurSel(h_.tab));
}

TabPresenter* MainWindow::ActivePresenter() const {
    auto t = CurrentTab();
    // REVIEW P0-5 (04-R3): TabCtrl_GetCurSel returns -1 for an empty tab
    // control; cast-to-size_t of -1 indexed past the array.
    if (t < TabId::Junk || t >= TabId::Count) return nullptr;
    return presenters_[static_cast<size_t>(t)].get();
}

void MainWindow::OnTabChanged() {
    auto t = CurrentTab();
    bool isHistory    = (t == TabId::History);
    bool isFolderTree = (t == TabId::FolderTree);
    bool isApps       = (t == TabId::Apps);
    bool isLargeFiles = (t == TabId::LargeFiles);
    bool isScanTab    = !isHistory && !isFolderTree;

    // Main action buttons
    ShowWindow(h_.scan,   (isScanTab || isFolderTree) ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.exec,   isScanTab ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.undo,   isHistory ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.emptyQ, isHistory ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.open,   isScanTab ? SW_SHOW : SW_HIDE);

    // Apps-only controls
    ShowWindow(h_.targetBtn,   isApps ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.advancedChk, isApps ? SW_SHOW : SW_HIDE);

    // LargeFiles settings panel
    for (HWND h : { h_.lblMinSize, h_.editMinSize, h_.lblMinSizeUnit,
                    h_.lblFileType, h_.editFileType, h_.lblDrives, h_.editDrives }) {
        ShowWindow(h, isLargeFiles ? SW_SHOW : SW_HIDE);
    }

    // Sort buttons (only on scan result tabs that have a sortable list, not Apps)
    bool showSort = isScanTab && !isApps;
    ShowWindow(h_.btnSortSize, showSort ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.btnSortTime, showSort ? SW_SHOW : SW_HIDE);

    // Content areas
    ShowWindow(h_.list, !isFolderTree ? SW_SHOW : SW_HIDE);
    ShowWindow(h_.tree, isFolderTree  ? SW_SHOW : SW_HIDE);

    if (auto* p = ActivePresenter()) p->OnActivate();

    switch (t) {
        case TabId::Junk:
            SetWindowTextW(h_.info,
                L"扫描系统/浏览器/开发缓存等可清理项；勾选后执行将移入隔离区，可在“操作历史”一键还原。\n"
                L"⚠ 危险项（清空回收站、WinSxS 等）默认不勾选；“清空隔离区”后才真正释放空间。");
            break;
        case TabId::LargeFiles:
            SetWindowTextW(h_.info,
                L"按最小大小、文件类型（.ext;..）与磁盘过滤查找大文件与重复文件；点列标题或排序按钮排序。\n"
                L"⚠ 重复文件默认保留最新一份、预选其余副本；执行前会复验文件是否已变化。");
            break;
        case TabId::Apps:
            SetWindowTextW(h_.info,
                L"扫描 C 盘已安装应用；先点“选择迁移目标盘…”，勾选后执行迁移，原位置以 Junction 保持可用。\n"
                L"⚠ 迁移前自动检查目标空间与文件占用；UWP/商店应用不支持迁移，已自动排除。");
            break;
        case TabId::FolderTree:
            SetWindowTextW(h_.info,
                L"按磁盘显示顶层文件夹及其大小，快速定位空间大户。\n"
                L"⚠ 右键文件夹可移入隔离区（可撤销），系统目录受保护名单拦截。");
            break;
        case TabId::History:
            SetWindowTextW(h_.info,
                L"操作历史：迁移与隔离区操作可一键撤销；“清空隔离区”将永久删除并释放空间。\n"
                L"⚠ 撤销时若原路径已被占用，文件将还原为 *.restored。");
            break;
        default: break;
    }
    if (auto* p = ActivePresenter()) p->Refresh();
    UpdateStatusBar();
    UpdateExecButton();
    // Re-layout for the new tab (settings row shown/hidden dynamically).
    OnSize();
}

void MainWindow::UpdateStatusBar() {
    // Three status-bar parts: disk | quarantine/migrate-target | progress.
    RECT rc; GetClientRect(h_.status, &rc);
    int W = rc.right;
    int e0 = 340, e1 = 660;
    if (e1 > W - 160) { e0 = W / 3; e1 = 2 * W / 3; }
    int edges[3] = { e0, e1, -1 };
    SendMessageW(h_.status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(edges));

    std::wstring disk;
    DiskSpace ds;
    if (QueryDiskSpace(SystemDriveRoot(), ds)) {
        disk = FormatW(L" 系统盘 %s  可用 %s / 共 %s",
            SystemDriveRoot().c_str(),
            FormatSize(ds.freeBytes).c_str(),
            FormatSize(ds.totalBytes).c_str());
    }
    std::wstring mid;
    // REVIEW P0-6 (07-X22): the migrate target used to displace the
    // quarantine usage permanently — show it only where it matters.
    if (CurrentTab() == TabId::Apps && !migrateTargetRoot_.empty()) {
        mid = L" 迁移目标: " + migrateTargetRoot_;
    } else if (auto q = SessionService::Instance().QuarantineUsageText(); !q.empty()) {
        mid = L" " + q;
    }
    std::wstring prog = L" " + SessionService::Instance().ProgressText();

    SendMessageW(h_.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(disk.c_str()));
    SendMessageW(h_.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(mid.c_str()));
    SendMessageW(h_.status, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(prog.c_str()));
}

void MainWindow::OnScan() {
    auto& svc = SessionService::Instance();
    // REVIEW P1-5 / 07-X8: while scanning, the scan button doubles as
    // Cancel (SessionService::CancelScan existed with no caller).
    if (svc.IsScanning()) {
        svc.CancelScan();
        ShowHint(L"ℹ 正在取消扫描…");
        return;
    }
    if (svc.IsBusy()) {
        ShowHint(L"ℹ 已有任务在进行中，请稍候。");
        return;
    }
    auto scanner = ActivePresenter() ? ActivePresenter()->BuildScanner() : nullptr;
    if (!scanner) return;
    if (!svc.StartScan(CurrentTab(), std::move(scanner))) return;

    SetTaskBusy(TaskMode::Scanning);
    UpdateStatusBar();
}

void MainWindow::OnScanDone() {
    auto& svc = SessionService::Instance();
    SetTaskBusy(TaskMode::None);

    auto t = CurrentTab();
    if (auto* p = ActivePresenter()) p->OnScanDone();
    if (t != TabId::History && svc.Results(t).empty()) {
        SetWindowTextW(h_.info, L"✓ 未发现可处理项，系统状况良好。");
    }

    // REVIEW P1-5 (07-X10 / 05-T-C3): silent degradation made visible. The
    // external rules and the USN index failed on the user's machine for a
    // week without any indication.
    std::wstring degrade;
    if (t == TabId::Junk) {
        const auto& rl = JunkRules::LastLoad();
        if (!rl.usedExternal && !rl.externalError.empty()) {
            degrade += L"\nℹ 外置规则不可用（" + rl.externalError + L"），已使用内置规则。";
        } else if (!rl.rejected.empty()) {
            degrade += FormatW(L"\nℹ 已拒绝 %zu 条不安全的外置规则（详见日志）。",
                               rl.rejected.size());
        }
        if (!VolumeIndex::Instance().IsValid()) {
            degrade += L"\nℹ 快速索引未启用，本次为全量扫描（较慢）。";
        }
    }
    if (!degrade.empty()) {
        wchar_t cur[1024] = {};
        GetWindowTextW(h_.info, cur, 1024);
        SetWindowTextW(h_.info, (std::wstring(cur) + degrade).c_str());
    }

    // REVIEW P2 (08-F6): one-line dashboard after a junk scan.
    if (t == TabId::Junk) {
        auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
        if (lp) {
            const auto& items = lp->Snapshot();
            unsigned long long safe = 0, cautious = 0;
            unsigned long long safeB = 0, cautiousB = 0;
            for (const auto& it : items) {
                if (it.riskLevel == RiskLevel::Safe) { safe += 1; safeB += it.sizeBytes; }
                else if (it.riskLevel == RiskLevel::Cautious) { cautious += 1; cautiousB += it.sizeBytes; }
            }
            if (!items.empty()) {
                std::wstring dash = FormatW(
                    L"可释放：安全 %llu 项 / %s，谨慎 %llu 项 / %s（勾选后执行，进入隔离区）",
                    safe, FormatSize(safeB).c_str(),
                    cautious, FormatSize(cautiousB).c_str());
                wchar_t cur[1024] = {};
                GetWindowTextW(h_.info, cur, 1024);
                SetWindowTextW(h_.info, (dash + L"\n" + cur).c_str());
            }
        }
    }
    UpdateStatusBar();
}

void MainWindow::SetTaskBusy(TaskMode mode) {
    taskMode_ = mode;
    bool busy = mode != TaskMode::None;
    bool scanning = mode == TaskMode::Scanning;

    // Everything that could race the worker on the results vector goes off;
    // the scan button doubles as Cancel while scanning (REVIEW P1-5 /
    // 07-X8: CancelScan existed with no caller).
    for (HWND h : { h_.exec, h_.undo, h_.emptyQ, h_.open,
                    h_.btnSortSize, h_.btnSortTime, h_.tab, h_.list, h_.tree,
                    h_.targetBtn, h_.advancedChk }) {
        if (h) EnableWindow(h, scanning ? FALSE : (busy ? FALSE : TRUE));
    }
    EnableWindow(h_.scan, scanning ? TRUE : (busy ? FALSE : TRUE));
    SetWindowTextW(h_.scan, scanning ? L"取消" : L"扫描");

    if (mode == TaskMode::None) {
        SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(h_.progress, SW_HIDE);
        UpdateExecButton();
    } else if (scanning) {
        SendMessageW(h_.progress, PBM_SETPOS, 0, 0);
        SendMessageW(h_.progress, PBM_SETMARQUEE, TRUE, 25);
        ShowWindow(h_.progress, SW_SHOW);
    } else {
        SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(h_.progress, PBM_SETPOS, 0, 0);
        ShowWindow(h_.progress, SW_SHOW);
    }
}

void MainWindow::UpdateExecButton() {
    if (taskMode_ != TaskMode::None) return;
    auto t = CurrentTab();
    if (t == TabId::History) {
        SetWindowTextW(h_.exec, L"执行选中操作");
        return;
    }
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) {
        SetWindowTextW(h_.exec, L"执行选中操作");
        return;
    }
    auto& items = lp->Snapshot();   // presenter snapshot (P1-1)
    auto checked = lp->CollectChecked();
    unsigned long long total = 0;
    size_t actionable = 0;
    for (auto idx : checked) {
        if (idx >= items.size()) continue;
        if (items[idx].riskLevel == RiskLevel::InfoOnly) continue;  // P0-6
        total += items[idx].sizeBytes;
        ++actionable;
    }
    if (actionable == 0) {
        SetWindowTextW(h_.exec, L"执行选中操作");
        EnableWindow(h_.exec, FALSE);
    } else {
        EnableWindow(h_.exec, TRUE);
        SetWindowTextW(h_.exec, FormatW(L"执行选中操作（%zu 项 · %s）",
                                        actionable, FormatSize(total).c_str())
                           .c_str());
    }
}

void MainWindow::ShowHint(const std::wstring& text) {
    // Non-modal feedback: guidance and success results go to the info label
    // instead of a MessageBox — modal popups are reserved for confirmations
    // and errors (v2.2 dialog-noise fix).
    SetWindowTextW(h_.info, text.c_str());
}

void MainWindow::OnExecute() {
    auto t = CurrentTab();
    if (t == TabId::History) return;
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;

    auto& svc = SessionService::Instance();
    // REVIEW P0-5 (04-R2): with the exec button live during scans, the
    // confirm dialog could outlive the scan and build a plan from stale
    // indices × new data. The busy gate closes the entry; the snapshot
    // contract (P1-1) removes the class.
    if (svc.IsBusy()) {
        ShowHint(L"ℹ 已有任务在进行中，请稍候。");
        return;
    }
    auto& items = lp->Snapshot();   // presenter snapshot (P1-1)
    if (items.empty()) {
        ShowHint(L"ℹ 列表为空，请先点击“扫描”。");
        return;
    }
    auto checked = lp->CollectChecked();
    if (checked.empty()) {
        ShowHint(L"ℹ 请先勾选要操作的项目（列表左侧复选框）。");
        return;
    }

    // REVIEW P0-6 (07-X3): InfoOnly items are display-only — drop them from
    // the plan up front and say so, instead of letting GuardRails deny them
    // after the user has confirmed.
    std::vector<size_t> selected;
    size_t infoOnly = 0;
    for (auto idx : checked) {
        if (idx >= items.size()) continue;
        if (items[idx].riskLevel == RiskLevel::InfoOnly) { ++infoOnly; continue; }
        selected.push_back(idx);
    }
    if (selected.empty()) {
        ShowHint(L"ℹ 所勾选项目均为“仅提示”项，不可执行（详见风险列）。");
        return;
    }

    if (t == TabId::Apps && migrateTargetRoot_.empty()) {
        // REVIEW P0-6 (07-X15): validate prerequisites BEFORE asking for
        // confirmation — the dialog used to come first.
        MessageBoxW(hwnd_, L"请先点击『选择迁移目标盘…』指定目标根目录。",
                    L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    // REVIEW P0-6 (08-F1): risk-grouped summary instead of "N 项 + 总大小".
    struct Group { size_t count = 0; unsigned long long bytes = 0; };
    Group gSafe, gCautious, gDelegate;
    bool hasRecycleBin = false;
    std::wstring dangerousNames;
    for (auto idx : selected) {
        const auto& it = items[idx];
        if (it.path == L"$RECYCLE.BIN") { hasRecycleBin = true; continue; }
        if (it.riskLevel == RiskLevel::Safe) {
            ++gSafe.count; gSafe.bytes += it.sizeBytes;
        } else if (it.riskLevel == RiskLevel::Advanced) {
            ++gDelegate.count; gDelegate.bytes += it.sizeBytes;
            if (dangerousNames.size() < 80) {
                dangerousNames += (dangerousNames.empty() ? L"" : L"、") + it.title;
            }
        } else {
            ++gCautious.count; gCautious.bytes += it.sizeBytes;
        }
    }

    unsigned long long totalSize = 0;
    for (auto idx : selected) totalSize += items[idx].sizeBytes;

    std::wstring groups;
    auto line = [](const wchar_t* badge, size_t n, unsigned long long bytes,
                   const wchar_t* consequence) {
        return FormatW(L"%s %zu 项（%s）— %s\n", badge, n,
                       FormatSize(bytes).c_str(), consequence);
    };
    if (gSafe.count) groups += line(L"🛡 安全", gSafe.count, gSafe.bytes,
                                    L"缓存类，移入隔离区可随时还原");
    if (gCautious.count) groups += line(L"ℹ 谨慎", gCautious.count, gCautious.bytes,
                                        L"清理后系统/程序需重新下载或重建");
    if (gDelegate.count) groups += line(L"⚠ 系统组件", gDelegate.count, gDelegate.bytes,
                                        L"通过系统命令处理，不可自动撤销");
    if (hasRecycleBin) groups += L"⚠ 清空回收站 — 不可逆\n";
    if (infoOnly) {
        groups += FormatW(L"⊘ 仅提示 %zu 项 — 不可执行，已自动排除\n", infoOnly);
    }
    if (!dangerousNames.empty()) {
        groups += L"\n涉及: " + dangerousNames + L"\n";
    }

    std::wstring confirm = FormatW(
        L"将处理 %zu 项，合计 %s。\n\n%s\n%s",
        selected.size(), FormatSize(totalSize).c_str(), groups.c_str(),
        t == TabId::Apps
            ? L"迁移后原位置以 Junction 保持可用，可在“操作历史”中撤销。"
            : L"隔离区文件可在“操作历史”中一键还原；“清空隔离区”后才真正释放空间。");

    bool createRestorePoint = false;
    // REVIEW P0-6 (07-X5): destructive confirmations default to CANCEL —
    // Enter no longer executes. Apps migrations additionally get a restore-
    // point checkbox (REVIEW P3 / 08-F8) via a verification TaskDialog.
    if (t == TabId::Apps) {
        TASKDIALOGCONFIG tc{};
        tc.cbSize = sizeof(tc);
        tc.hwndParent = hwnd_;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = L"确认操作";
        tc.pszMainIcon = (gDelegate.count || hasRecycleBin) ? TD_WARNING_ICON
                                                            : TD_INFORMATION_ICON;
        std::wstring mainInstr = FormatW(L"将迁移 %zu 项应用，合计 %s",
                                         selected.size(),
                                         FormatSize(totalSize).c_str());
        tc.pszMainInstruction = mainInstr.c_str();
        tc.pszContent = confirm.c_str();
        TASKDIALOG_BUTTON btns[] = {
            { IDOK,   L"开始迁移" },
            { IDCANCEL, L"取消（默认）" },
        };
        tc.pButtons = btns;
        tc.cButtons = 2;
        tc.nDefaultButton = IDCANCEL;   // default CANCEL
        tc.pszVerificationText = L"迁移前创建系统还原点（推荐）";
        tc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
        BOOL verified = FALSE;
        int pressed = IDCANCEL;
        TaskDialogIndirect(&tc, &pressed, nullptr, &verified);
        if (pressed != IDOK) return;
        createRestorePoint = (verified == TRUE);
    } else {
        confirm += L"\n确定继续？";
        if (MessageBoxW(hwnd_, confirm.c_str(), L"确认操作",
                MB_OKCANCEL | MB_DEFBUTTON2 |
                    ((gDelegate.count || hasRecycleBin) ? MB_ICONWARNING
                                                        : MB_ICONQUESTION)) != IDOK) {
            return;
        }
    }

    // Everything goes through PlanBuilder + GuardRails from here on.
    auto plan = PlanBuilder::Build(t, items, selected, migrateTargetRoot_, useSymlink_);
    plan.createRestorePoint = createRestorePoint;
    switch (svc.ExecutePlan(plan)) {
        case SessionService::PlanStart::Started:
            SetTaskBusy(TaskMode::Executing);
            UpdateStatusBar();
            break;
        case SessionService::PlanStart::Busy:
            ShowHint(L"ℹ 已有任务在进行中，请稍候。");
            break;
        case SessionService::PlanStart::PlanStale:
            ShowHint(L"⚠ 计划已过期（扫描结果已变化），请重新扫描。");
            break;
    }
}

void MainWindow::OnPlanDone() {
    auto& svc = SessionService::Instance();
    SetTaskBusy(TaskMode::None);

    const auto& rpt = svc.LastReport();

    // Success feedback is non-modal: one line in the info label.
    std::wstring line;
    if (rpt.quarantinedBytes > 0) {
        line += FormatW(L"已隔离 %s（可还原）", FormatSize(rpt.quarantinedBytes).c_str());
    }
    if (rpt.freedBytes > 0) {
        if (!line.empty()) line += L"，";
        line += FormatW(L"已释放 %s", FormatSize(rpt.freedBytes).c_str());
    }
    std::wstring info = FormatW(L"✓ 完成: 成功 %d · 跳过 %d · 失败 %d",
                                rpt.succeeded, rpt.skipped, rpt.failed);
    if (!line.empty()) info += L" — " + line;
    ShowHint(info);

    // A modal dialog appears only when something needs attention:
    // failures (warning) or guard-rail skips (information with reasons).
    if (rpt.failed > 0 || rpt.skipped > 0) {
        std::wstring lines;
        if (rpt.quarantinedBytes > 0) {
            lines += FormatW(L"已隔离 %s（可还原；清空隔离区后释放）\n",
                             FormatSize(rpt.quarantinedBytes).c_str());
        }
        if (rpt.freedBytes > 0) {
            lines += FormatW(L"已释放 %s\n", FormatSize(rpt.freedBytes).c_str());
        }
        auto summary = FormatW(L"完成: 成功 %d, 跳过 %d, 失败 %d。\n%s%s",
                               rpt.succeeded, rpt.skipped, rpt.failed,
                               lines.c_str(), rpt.details.c_str());
        MessageBoxW(hwnd_, summary.c_str(),
                    rpt.failed > 0 ? L"操作完成（有失败项）" : L"操作完成（有跳过项）",
                    MB_OK | (rpt.failed > 0 ? MB_ICONWARNING : MB_ICONINFORMATION));
    }

    if (auto* p = ActivePresenter()) p->Refresh();
    UpdateStatusBar();
}

void MainWindow::OnEmptyQuarantine() {
    auto& svc = SessionService::Instance();
    std::wstring usage = svc.QuarantineUsageText();
    std::wstring confirm = FormatW(
        L"将永久删除隔离区中的所有文件并释放空间，此操作不可撤销。\n%s\n\n确定继续？",
        usage.empty() ? L"" : (L"当前占用: " + usage).c_str());
    // REVIEW P0-6 (07-X5): the single irreversible file deletion in the app
    // defaults to CANCEL.
    if (MessageBoxW(hwnd_, confirm.c_str(), L"清空隔离区",
            MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONWARNING) != IDOK) {
        return;
    }
    if (!svc.EmptyQuarantine()) {
        ShowHint(L"ℹ 已有任务在进行中，请稍候。");
        return;
    }
    SetTaskBusy(TaskMode::Executing);
}

void MainWindow::OnAbout() {
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = hwnd_;
    tc.hInstance = hInst_;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"关于 MiniSys";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APPICON);
    tc.pszMainInstruction = L"MiniSys — C 盘瘦身助手  v2.2";
    tc.pszContent =
        L"安全、可逆的 C 盘清理与迁移工具。所有文件操作先经安全闸复验，"
        L"默认移入隔离区、可一键还原。\n"
        L"\n【使用说明】\n"
        L"· 垃圾清理：扫描后勾选执行，项目移入隔离区；“清空隔离区”才真正释放空间。\n"
        L"· 大文件/去重：按大小、类型、磁盘过滤；重复文件保留最新一份，预选其余副本。\n"
        L"· 应用迁移：先选目标盘再执行；原位置以 Junction 保持路径可用，历史页可撤销。\n"
        L"· 文件夹分析：右键顶层文件夹可移入隔离区。\n"
        L"\n【注意事项】\n"
        L"· 系统目录（Windows、WinSxS、System32 等）一律拒绝操作。\n"
        L"· 组件存储/休眠文件通过系统命令（DISM / powercfg）处理，耗时数分钟属正常。\n"
        L"· 隔离区位于各盘根目录 MiniSys.Quarantine，清空即永久删除。\n"
        L"· 迁移杀毒软件等敏感应用前，建议先创建系统还原点。";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.nDefaultButton = IDOK;
    int pressed = 0;
    TaskDialogIndirect(&tc, &pressed, nullptr, nullptr);
}

void MainWindow::OnOpenLocation() {
    int sel = ListView_GetNextItem(h_.list, -1, LVNI_SELECTED);
    if (sel < 0) return;
    auto t = CurrentTab();
    if (t == TabId::History) return;
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;
    const auto& items = lp->Snapshot();
    LVITEMW lvi{}; lvi.iItem = sel; lvi.mask = LVIF_PARAM;
    ListView_GetItem(h_.list, &lvi);
    size_t idx = static_cast<size_t>(lvi.lParam);
    if (idx >= items.size()) return;
    auto path = items[idx].path;
    std::wstring args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// REVIEW P2: settings persistence — load once at startup, save on change.
void MainWindow::LoadSettings() {
    settings_ = Settings::Load();
    migrateTargetRoot_ = settings_.migrateTargetRoot;
    useSymlink_ = settings_.useSymlink;
    Button_SetCheck(h_.advancedChk, useSymlink_ ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(h_.editMinSize, std::to_wstring(settings_.largeFilesMinMB).c_str());
    SetWindowTextW(h_.editFileType, settings_.largeFilesExtFilter.c_str());
    SetWindowTextW(h_.editDrives, settings_.largeFilesDrives.c_str());
    GuardRails::SetUserExclusions(settings_.exclusions);
}

void MainWindow::SaveSettings() {
    settings_.migrateTargetRoot = migrateTargetRoot_;
    settings_.useSymlink = useSymlink_;
    wchar_t buf[512] = {};
    GetWindowTextW(h_.editMinSize, buf, 32);
    if (int mb = _wtoi(buf); mb > 0) settings_.largeFilesMinMB = mb;
    GetWindowTextW(h_.editFileType, buf, 512);
    settings_.largeFilesExtFilter = buf;
    GetWindowTextW(h_.editDrives, buf, 128);
    settings_.largeFilesDrives = buf;
    settings_.Save();
    GuardRails::SetUserExclusions(settings_.exclusions);
}

// REVIEW P2 (07-X12): Ctrl+A toggles all rows (select-all when none or some
// checked, clear when all checked).
void MainWindow::OnSelectAll() {
    auto t = CurrentTab();
    if (t == TabId::History || t == TabId::FolderTree) return;
    int n = ListView_GetItemCount(h_.list);
    if (n == 0) return;
    int checked = 0;
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(h_.list, i)) ++checked;
    }
    bool target = (checked < n);   // if not everything is checked → select all
    for (int i = 0; i < n; ++i) {
        ListView_SetCheckState(h_.list, i, target ? TRUE : FALSE);
    }
}

// REVIEW P2 (08-F7): dry-run preview — batch GuardRails::Validate over the
// checked rows and show which would pass / be denied, with reasons. Pure
// (no side effects); state can drift before a real execute, which planHash
// and the per-item re-verification still guard.
void MainWindow::OnPreviewExecution() {
    auto t = CurrentTab();
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;
    const auto& items = lp->Snapshot();
    auto checked = lp->CollectChecked();
    if (checked.empty()) {
        ShowHint(L"ℹ 请先勾选要预览的项目。");
        return;
    }
    int pass = 0;
    std::wstring denied;
    for (size_t idx : checked) {
        if (idx >= items.size()) continue;
        const auto& it = items[idx];
        PlanItem pi;
        pi.itemIdx = idx;
        pi.path = it.path;
        pi.sizeAtScan = it.sizeBytes;
        pi.lastWriteAtScan = it.lastWriteFiletime;
        auto ctx = (t == TabId::Apps) ? GuardRails::Context::Migration
                                      : GuardRails::Context::FileOp;
        auto verdict = GuardRails::Instance().Validate(pi, it, ctx);
        if (verdict.allow) {
            ++pass;
        } else {
            denied += FormatW(L"⊘ %s — %s\n", it.title.c_str(),
                              verdict.reason.c_str());
        }
    }
    std::wstring content = FormatW(L"通过安全闸：%d 项（可执行）\n被拒：%d 项\n\n%s",
                                   pass, static_cast<int>(checked.size()) - pass,
                                   denied.c_str());
    MessageBoxW(hwnd_, content.c_str(), L"预览执行（不会做任何更改）",
                MB_OK | MB_ICONINFORMATION);
}

// REVIEW P2 (08-F9): add the selected item's path to the exclusion list.
void MainWindow::OnExcludeSelected() {
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;
    int sel = ListView_GetNextItem(h_.list, -1, LVNI_SELECTED);
    if (sel < 0) {
        ShowHint(L"ℹ 请先选中要排除的项目。");
        return;
    }
    const ScanItem* it = lp->ItemAtRow(sel);
    if (!it) return;
    auto path = it->path.wstring();
    // Avoid duplicates.
    for (const auto& e : settings_.exclusions) {
        if (IEquals(e, path)) {
            ShowHint(L"ℹ 该项目已在排除清单中。");
            return;
        }
    }
    settings_.exclusions.push_back(path);
    SaveSettings();
    ShowHint(L"✓ 已加入排除清单（永不清理）: " + path +
             L"。重新扫描后将不再出现；已在列表中的项目执行时也会被安全闸拒绝。");
    MS_LOG_INFO(L"Exclusion added: %s", path.c_str());
}

// List context menu (REVIEW P2 / 07-X24).
void MainWindow::OnListContextMenu() {
    auto t = CurrentTab();
    if (t == TabId::History || t == TabId::FolderTree) return;
    DWORD pos = GetMessagePos();
    POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_OPEN,       L"打开所在位置\tEnter");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_INFO,       L"说明（这是什么？）…");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_SELECTALL,  L"全选\tCtrl+A");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_SELECTNONE, L"全不选");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_PREVIEW,    L"预览执行（安全闸检查）…");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_EXCLUDE,    L"永不清理此文件夹");
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);
    if (cmd == 0) return;
    // Re-dispatch through the command path so all handlers stay in one place.
    SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
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
    SaveSettings();   // REVIEW P2 (08-F11): persist across restarts
    UpdateStatusBar();
}

} // namespace minisys
