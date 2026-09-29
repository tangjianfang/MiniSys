#include "MainWindow.h"
#include "res/resource.h"

#include "core/DeleteOp.h"
#include "core/PlanBuilder.h"
#include "core/SessionService.h"
#include "ui/Controls.h"
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
            } else if (nm->hwndFrom == h_.list && nm->code == LVN_COLUMNCLICK) {
                auto nmlv = reinterpret_cast<LPNMLISTVIEW>(lp);
                if (auto* p = ActivePresenter()) p->OnColumnClick(nmlv->iSubItem);
            } else if (nm->hwndFrom == h_.tree && nm->code == NM_RCLICK) {
                if (auto* p = ActivePresenter()) {
                    auto* ftp = dynamic_cast<FolderTreePresenter*>(p);
                    if (ftp) ftp->OnContextMenu();
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
                case IDC_CHK_ADVANCED:
                    useSymlink_ = (Button_GetCheck(h_.advancedChk) == BST_CHECKED);
                    break;
            }
            return 0;
        }
        case WM_APP_SCAN_PROGRESS:
        case WM_APP_OP_PROGRESS:
            UpdateStatusBar();
            return 0;
        case WM_APP_SCAN_DONE:
            OnScanDone();
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

    OnTabChanged();
    UpdateStatusBar();
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
    if (t < TabId::Count) return presenters_[static_cast<size_t>(t)].get();
    return nullptr;
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
            SetWindowTextW(h_.info, L"扫描系统/浏览器/开发缓存等可清理项；默认移入隔离区（可一键还原），清空隔离区后才真正释放空间。");
            break;
        case TabId::LargeFiles:
            SetWindowTextW(h_.info, L"大小≥指定MB，可按文件类型(.ext;..)和磁盘过滤；点击列标题或排序按钮排序。");
            break;
        case TabId::Apps:
            SetWindowTextW(h_.info, L"扫描 C 盘已安装应用；勾选并点击执行将复制到目标盘并在原位置创建 Junction。");
            break;
        case TabId::FolderTree:
            SetWindowTextW(h_.info, L"按磁盘显示顶层文件夹及大小；右键文件夹可删除。");
            break;
        case TabId::History:
            SetWindowTextW(h_.info, L"操作历史。迁移与隔离区操作可一键撤销；“清空隔离区”将永久删除并释放空间。");
            break;
        default: break;
    }
    if (auto* p = ActivePresenter()) p->Refresh();
    UpdateStatusBar();
    // Re-layout for the new tab (settings row shown/hidden dynamically).
    OnSize();
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
    if (auto q = SessionService::Instance().QuarantineUsageText(); !q.empty()) {
        msg += q + L"   |   ";
    }
    msg += SessionService::Instance().ProgressText();
    SendMessageW(h_.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(msg.c_str()));
}

void MainWindow::OnScan() {
    auto& svc = SessionService::Instance();
    if (svc.IsScanning()) {
        MessageBoxW(hwnd_, L"已有扫描在进行中。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto scanner = ActivePresenter() ? ActivePresenter()->BuildScanner() : nullptr;
    if (!scanner) return;
    if (!svc.StartScan(CurrentTab(), std::move(scanner))) return;

    // Disable scan button and show progress bar while scanning.
    EnableWindow(h_.scan, FALSE);
    ShowWindow(h_.progress, SW_SHOW);
    SendMessageW(h_.progress, PBM_SETMARQUEE, TRUE, 25);
    UpdateStatusBar();
}

void MainWindow::OnScanDone() {
    auto& svc = SessionService::Instance();
    // Re-enable scan button and hide progress bar.
    EnableWindow(h_.scan, TRUE);
    SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(h_.progress, SW_HIDE);

    auto t = CurrentTab();
    if (auto* p = ActivePresenter()) p->OnScanDone();
    if (t != TabId::History && svc.Results(t).empty()) {
        SetWindowTextW(h_.info, L"✓ 未发现可处理项，系统状况良好。");
    }
    UpdateStatusBar();
}

void MainWindow::OnExecute() {
    auto t = CurrentTab();
    if (t == TabId::History) return;
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;

    auto& svc = SessionService::Instance();
    auto& items = svc.Results(t);
    if (items.empty()) {
        MessageBoxW(hwnd_, L"列表为空，请先扫描。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto selected = lp->CollectChecked();
    if (selected.empty()) {
        MessageBoxW(hwnd_, L"请勾选要操作的项目。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    unsigned long long totalSize = 0;
    bool hasDangerous = false;
    for (auto idx : selected) {
        if (idx >= items.size()) continue;
        totalSize += items[idx].sizeBytes;
        if (items[idx].dangerous) hasDangerous = true;
    }
    std::wstring action = (t == TabId::Apps) ? L"迁移"
                                               : L"移入隔离区（可撤销）";
    std::wstring confirm = FormatW(
        L"将对 %zu 项执行【%s】操作，合计 %s。\n"
        L"隔离区项目可在“操作历史”中一键还原。\n\n%s",
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

    // Everything goes through PlanBuilder + GuardRails from here on.
    auto plan = PlanBuilder::Build(t, items, selected, migrateTargetRoot_, useSymlink_);
    switch (svc.ExecutePlan(plan)) {
        case SessionService::PlanStart::Started:
            EnableWindow(h_.exec, FALSE);
            EnableWindow(h_.undo, FALSE);
            EnableWindow(h_.scan, FALSE);
            ShowWindow(h_.progress, SW_SHOW);
            SendMessageW(h_.progress, PBM_SETMARQUEE, TRUE, 25);
            UpdateStatusBar();
            break;
        case SessionService::PlanStart::Busy:
            MessageBoxW(hwnd_, L"已有任务在进行中，请稍候。", L"提示",
                        MB_OK | MB_ICONINFORMATION);
            break;
        case SessionService::PlanStart::PlanStale:
            MessageBoxW(hwnd_, L"计划已过期（扫描结果已变化），请重新扫描。",
                        L"提示", MB_OK | MB_ICONWARNING);
            break;
    }
}

void MainWindow::OnPlanDone() {
    auto& svc = SessionService::Instance();
    EnableWindow(h_.exec, TRUE);
    EnableWindow(h_.undo, TRUE);
    EnableWindow(h_.emptyQ, TRUE);
    EnableWindow(h_.scan, TRUE);
    SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(h_.progress, SW_HIDE);

    const auto& rpt = svc.LastReport();
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
    MessageBoxW(hwnd_, summary.c_str(), L"操作完成",
        MB_OK | (rpt.failed ? MB_ICONWARNING : MB_ICONINFORMATION));

    if (auto* p = ActivePresenter()) p->Refresh();
    UpdateStatusBar();
}

void MainWindow::OnEmptyQuarantine() {
    auto& svc = SessionService::Instance();
    std::wstring usage = svc.QuarantineUsageText();
    std::wstring confirm = FormatW(
        L"将永久删除隔离区中的所有文件并释放空间，此操作不可撤销。\n%s\n\n确定继续？",
        usage.empty() ? L"" : (L"当前占用: " + usage).c_str());
    if (MessageBoxW(hwnd_, confirm.c_str(), L"清空隔离区",
            MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        return;
    }
    if (!svc.EmptyQuarantine()) {
        MessageBoxW(hwnd_, L"已有任务在进行中，请稍候。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    EnableWindow(h_.undo, FALSE);
    EnableWindow(h_.emptyQ, FALSE);
    ShowWindow(h_.progress, SW_SHOW);
    SendMessageW(h_.progress, PBM_SETMARQUEE, TRUE, 25);
}

void MainWindow::OnOpenLocation() {
    int sel = ListView_GetNextItem(h_.list, -1, LVNI_SELECTED);
    if (sel < 0) return;
    auto t = CurrentTab();
    if (t == TabId::History) return;
    auto& items = SessionService::Instance().Results(t);
    LVITEMW lvi{}; lvi.iItem = sel; lvi.mask = LVIF_PARAM;
    ListView_GetItem(h_.list, &lvi);
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

} // namespace minisys
