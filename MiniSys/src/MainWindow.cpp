#include "MainWindow.h"
#include "res/resource.h"
#include "res/version.h"   // generated per build (tools/incr_build.bat)

#include "core/DeleteOp.h"
#include "core/GuardRails.h"
#include "core/JunkRules.h"
#include "core/PlanBuilder.h"
#include "core/SessionService.h"
#include "core/VolumeIndex.h"
#include "ui/CommandPalette.h"
#include "ui/Controls.h"
#include "ui/Dialogs.h"
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
// v2.11: identity widened from C 盘瘦身助手 to 磁盘瘦身助手 — the tool now
// indexes/scans ALL fixed disks by default.
constexpr wchar_t kWindowTitle[] =
    L"MiniSys — 磁盘瘦身助手 " MINISYS_VERSION_TITLE;

const wchar_t* TabName(TabId t) {
    switch (t) {
        case TabId::Junk:       return L"垃圾清理";
        case TabId::Search:     return L"文件搜索";
        case TabId::LargeFiles: return L"大文件 / 去重";
        case TabId::Apps:       return L"应用迁移";
        case TabId::FolderTree: return L"文件夹分析";
        case TabId::History:    return L"操作历史";
        default:                return L"";
    }
}

// v2.3: thousands-separator count formatting ("1,234,567").
std::wstring FormatCount(size_t n) {
    std::wstring raw = std::to_wstring(n);
    std::wstring out;
    for (size_t i = 0; i < raw.size(); ++i) {
        size_t fromEnd = raw.size() - i;
        out += raw[i];
        if (fromEnd > 1 && (fromEnd - 1) % 3 == 0) out += L',';
    }
    return out;
}

// v2.5 cached results: human-readable age of a FILETIME timestamp.
std::wstring FormatAge(uint64_t filetime) {
    if (filetime == 0) return {};
    FILETIME localNow{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    SystemTimeToFileTime(&st, &localNow);
    uint64_t now = (static_cast<uint64_t>(localNow.dwHighDateTime) << 32) |
                   localNow.dwLowDateTime;
    uint64_t ageMs = (now > filetime) ? (now - filetime) / 10000 : 0;
    if (ageMs < 10 * 60 * 1000) return L"刚刚";
    if (ageMs < 60 * 60 * 1000) {
        return std::to_wstring(ageMs / (60 * 1000)) + L" 分钟前";
    }
    if (ageMs < 24ULL * 3600 * 1000) {
        return std::to_wstring(ageMs / (3600 * 1000)) + L" 小时前";
    }
    ULARGE_INTEGER ul{};
    ul.QuadPart = filetime;
    FILETIME local{};
    local.dwLowDateTime  = ul.LowPart;
    local.dwHighDateTime = ul.HighPart;
    SYSTEMTIME out{};
    FileTimeToSystemTime(&local, &out);
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
               out.wYear, out.wMonth, out.wDay, out.wHour, out.wMinute);
    return buf;
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
        CW_USEDEFAULT, CW_USEDEFAULT,
        // REVIEW-UI P2 (L-12): the old fixed 1100×720 physical pixels
        // shrank with every DPI step — scale the initial size for the
        // primary monitor's DPI.
        UiScale(nullptr, 1100), UiScale(nullptr, 720),
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
        { FVIRTKEY | FCONTROL, 'K', IDC_ACCEL_PALETTE },   // v2.10 命令面板
    };
    HACCEL hAccel = CreateAcceleratorTableW(acc, 3);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (hAccel && TranslateAcceleratorW(hwnd_, hAccel, &msg)) continue;
        // Tab navigation across child controls.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
            HWND focus = GetFocus();
            HWND next = GetNextDlgTabItem(hwnd_, focus, (GetKeyState(VK_SHIFT) & 0x8000) ? TRUE : FALSE);
            if (next) { SetFocus(next); continue; }
        }
        // v2.9: Enter in the search box re-runs the query (the cached rows
        // from last session display without running anything; this is the
        // explicit "refresh"). review-07 X-19 (v2.10): Esc clears it.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == h_.editSearch && taskMode_ == TaskMode::None) {
            RunSearch();
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            GetFocus() == h_.editSearch) {
            SetWindowTextW(h_.editSearch, L"");
            if (taskMode_ == TaskMode::None) RunSearch();
            continue;
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
        case WM_DPICHANGED: {
            // review-07 X-4: the manifest declares PerMonitorV2 but this
            // message was never handled — after dragging across monitors
            // the window kept its physical size while LayoutWindow started
            // scaling for the new DPI (controls overflow). Adopt the
            // system's suggested rectangle; the WM_SIZE that follows
            // re-runs the layout at the new scale.
            auto prc = reinterpret_cast<LPRECT>(lp);
            SetWindowPos(hwnd_, nullptr, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            // review-07 X-5: no minimum size — below ~936 logical px the
            // button row clips and on the Apps page targetBtn overlaps
            // advancedChk (W < 1068). Clamp to the layout's real floor.
            auto mmi = reinterpret_cast<LPMINMAXINFO>(lp);
            mmi->ptMinTrackSize.x = UiScale(hwnd_, 980);
            mmi->ptMinTrackSize.y = UiScale(hwnd_, 560);
            return 0;
        }
        case WM_NOTIFY: {
            auto nm = reinterpret_cast<LPNMHDR>(lp);
            if (nm->hwndFrom == h_.tab && nm->code == TCN_SELCHANGE) {
                OnTabChanged();
            } else if (nm->hwndFrom == h_.list &&
                       nm->code == LVN_ITEMCHANGED &&
                       taskMode_ == TaskMode::None) {
                // Live "执行选中操作（N 项 · X）" (REVIEW P0-6).
                // REVIEW-UI P1 (L-6/04-7): react only when the CHECK IMAGE
                // actually flipped — selection-only changes and batch
                // renders must not trigger the O(N) recount.
                auto* nmlv = reinterpret_cast<LPNMLISTVIEW>(lp);
                bool checkFlipped =
                    (nmlv->uChanged & LVIF_STATE) &&
                    ((nmlv->uNewState ^ nmlv->uOldState) & LVIS_STATEIMAGEMASK);
                auto* lp2 = dynamic_cast<ListTabPresenter*>(ActivePresenter());
                if (checkFlipped && !(lp2 && lp2->InBatchUpdate())) {
                    UpdateExecButton();
                }
            } else if (nm->hwndFrom == h_.list &&
                       nm->code == LVN_ITEMACTIVATE) {
                // REVIEW P1-6: double-click = "why is this here" panel.
                auto* nmia = reinterpret_cast<LPNMITEMACTIVATE>(lp);
                if (CurrentTab() == TabId::Search) {
                    OnOpenLocation();   // Everything-style: locate the file
                } else if (auto* p = ActivePresenter()) {
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
                        // review-07 X-14 (v2.10): skip the risk tints under
                        // high-contrast themes — fixed warm colours on an
                        // unpredictable background can be unreadable; the
                        // badge column carries the information anyway.
                        HIGHCONTRASTW hc{ sizeof(hc) };
                        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,
                                                  sizeof(hc), &hc, 0) &&
                            (hc.dwFlags & HCF_HIGHCONTRASTON)) {
                            return CDRF_DODEFAULT;
                        }
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
                                // review-07 X-11 (v2.10): say WHERE the view
                                // is now and how to get back.
                                ShowHint(L"ℹ 已下钻到 " + items[idx].path.wstring() +
                                         L"（再点一次“扫描”返回全盘视图）");
                                OnScan();   // scan the drilled subtree
                            }
                        }
                    }
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            // v2.3: instant-search input (debounced) + match-path toggle.
            if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_EDIT_SEARCH) {
                SetTimer(hwnd_, TIMER_SEARCH_DEBOUNCE, 200, nullptr);
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == IDC_CHK_MATCHPATH) {
                // REVIEW-UI P1 (04-2): gated like the debounce timer.
                if (taskMode_ == TaskMode::None) RunSearch();
                SaveSettings();   // v2.9: persist the toggle
                return 0;
            }
            switch (LOWORD(wp)) {
                case IDC_BTN_SCAN:    OnScan(); break;
                case IDC_BTN_EXECUTE: OnExecute(); break;
                case IDC_BTN_QUICKFILTER:
                    OnQuickFilterMenu();
                    break;
                case IDC_ACCEL_PALETTE:
                    OnCommandPalette();
                    break;
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
                    // review-07 X-2: with focus in an edit box Ctrl+A must
                    // select the TEXT, not flip every list checkbox.
                    if (HWND focus = GetFocus()) {
                        wchar_t cls[8] = {};
                        GetClassNameW(focus, cls, 8);
                        if (wcscmp(cls, L"Edit") == 0) {
                            SendMessageW(focus, EM_SETSEL, 0, -1);
                            break;
                        }
                    }
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
                    // review-04 R-2: same batch guard as Ctrl+A — without
                    // it the loop storms LVN_ITEMCHANGED (O(N²) freezes) and
                    // the exec button count goes stale.
                    bool on = (LOWORD(wp) == IDM_LIST_SELECTALL);
                    auto* lp2 = dynamic_cast<ListTabPresenter*>(ActivePresenter());
                    if (lp2) lp2->SetBatchUpdate(true);
                    int n = ListView_GetItemCount(h_.list);
                    for (int i = 0; i < n; ++i) {
                        ListView_SetCheckState(h_.list, i, on ? TRUE : FALSE);
                    }
                    if (lp2) lp2->SetBatchUpdate(false);
                    UpdateExecButton();
                    break;
                }
                case IDM_LIST_PREVIEW:   OnPreviewExecution(); break;
                case IDM_LIST_EXCLUDE:   OnExcludeSelected(); break;
                case IDM_LIST_VERIFY:    OnVerifyList(/*manual=*/true); break;
            }
            return 0;
        }
        case WM_APP_SCAN_PROGRESS:
            // REVIEW-UI P2 (L-13): wp = done, lp = total. A phase that knows
            // its totals switches the bar from marquee to a percentage; the
            // next indeterminate phase switches it back.
            if (lp > 0 && wp <= static_cast<unsigned long long>(lp)) {
                if (!progDeterminate_) {
                    SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
                    progDeterminate_ = true;
                }
                SendMessageW(h_.progress, PBM_SETPOS,
                             static_cast<int>(wp * 100 /
                                 static_cast<unsigned long long>(lp)), 0);
            } else if (progDeterminate_) {
                progDeterminate_ = false;
                SendMessageW(h_.progress, PBM_SETPOS, 0, 0);
                SendMessageW(h_.progress, PBM_SETMARQUEE, TRUE, 25);
            }
            UpdateStatusBar();
            return 0;
        case WM_APP_OP_PROGRESS:
            // Determinate execution progress (REVIEW P0-5 / 07-X8).
            SendMessageW(h_.progress, PBM_SETPOS, static_cast<int>(wp), 0);
            UpdateStatusBar();
            return 0;
        case WM_APP_SCAN_DONE:
            // review-04 R-4: drop stale completions from superseded tasks.
            if (wp != SessionService::Instance().CurrentTaskGen()) return 0;
            OnScanDone();
            return 0;
        case WM_APP_TASK_STARTED:
            // An ExecutePlan launched outside OnExecute (folder-tree right
            // click) — lock the action matrix for it.
            // REVIEW-UI P1 (04-4): gate on IsBusy — if the worker already
            // finished (its OP_DONE was queued first), locking now would
            // deadlock the matrix forever.
            // R-4: plus the generation gate (stale START of an old task).
            if (wp == SessionService::Instance().CurrentTaskGen() &&
                SessionService::Instance().IsBusy()) {
                SetTaskBusy(TaskMode::Executing);
                UpdateStatusBar();
            }
            return 0;
        case WM_APP_OP_DONE:
            if (wp != SessionService::Instance().CurrentTaskGen()) return 0;
            OnPlanDone();
            return 0;
        case WM_APP_SEARCH_DONE:
            // REVIEW-UI P1 (04-1/U-1): async search completed on the worker.
            if (wp != SessionService::Instance().CurrentTaskGen()) return 0;
            if (CurrentTab() == TabId::Search) {
                if (auto* p = ActivePresenter()) p->Refresh();
                UpdateSearchStatus();
            }
            UpdateStatusBar();
            return 0;
        case WM_APP_VERIFY_DONE:
            if (wp != SessionService::Instance().CurrentTaskGen()) return 0;
            OnVerifyDone();
            UpdateStatusBar();
            return 0;
        case WM_APP_PREVIEW_DONE:
            if (wp != SessionService::Instance().CurrentTaskGen()) return 0;
            OnPreviewDone();
            UpdateStatusBar();
            return 0;
        case WM_TIMER:
            // v2.3: search debounce elapsed — apply the query.
            if (wp == TIMER_SEARCH_DEBOUNCE) {
                KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);
                // REVIEW-UI P1 (04-2): never touch the snapshot/results from
                // a timer while any task is live.
                if (taskMode_ == TaskMode::None) RunSearch();
            } else if (wp == TIMER_SEARCH_RETRY) {
                // A previous search was still draining — try again.
                KillTimer(hwnd_, TIMER_SEARCH_RETRY);
                if (taskMode_ == TaskMode::None) RunSearch();
            } else if (wp == TIMER_IDLE_REFRESH) {
                // v2.5 cached results: while the user is away, refresh the
                // active tab's stale list in the background — the cache
                // stays accurate without anyone asking.
                OnIdleCheck();
            } else if (wp == TIMER_VERIFY_LIST) {
                KillTimer(hwnd_, TIMER_VERIFY_LIST);
                OnVerifyList(/*manual=*/false);
            } else if (wp == TIMER_IDLE_COUNTDOWN) {
                // v2.10 (X-10): grace countdown for the idle rescan — any
                // global input since arming cancels it.
                KillTimer(hwnd_, TIMER_IDLE_COUNTDOWN);
                LASTINPUTINFO li{ sizeof(li) };
                if (idleCountdown_ <= 0 || !GetLastInputInfo(&li) ||
                    li.dwTime != idleArmInput_ || taskMode_ != TaskMode::None) {
                    idleCountdown_ = 0;
                    return 0;   // user came back — cancelled
                }
                --idleCountdown_;
                if (idleCountdown_ > 0) {
                    ShowHint(FormatW(L"ℹ 系统空闲 — %d 秒后自动刷新本页扫描结果（动一下鼠标或键盘即取消）",
                                     idleCountdown_));
                    SetTimer(hwnd_, TIMER_IDLE_COUNTDOWN, 1000, nullptr);
                    return 0;
                }
                ShowHint(L"ℹ 系统空闲 — 正在自动刷新本页扫描结果…");
                OnScan();
            }
            return 0;
        case WM_ACTIVATE:
            // v2.7: coming back from Explorer after deleting things by
            // hand — verify the visible list shortly (debounced; a one-shot
            // timer so the activation burst doesn't run during redraw).
            if (LOWORD(wp) != WA_INACTIVE && HIWORD(wp) == 0) {
                SetTimer(hwnd_, TIMER_VERIFY_LIST, 400, nullptr);
            }
            return 0;
        case WM_CLOSE:
            SaveSettings();   // v2.9: keep query/tab/filters for next session
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
    presenters_[static_cast<size_t>(TabId::Search)]     = std::make_unique<SearchPresenter>(TabId::Search, h_);
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

    // v2.9: restore persisted state FIRST (it re-selects the last active
    // tab and refills the search box), so the initial tab change renders
    // the right page.
    LoadSettings();   // REVIEW P2: restore persisted state + exclusions
    OnTabChanged();
    // v2.5 cached results: restore the last session's scan lists so tabs
    // open populated; each tab's staleness is judged separately and shown
    // ("上次扫描: …（缓存）"). v2.9: the search tab joins (query + results).
    // Execution re-verifies every item regardless.
    for (int i = 0; i < static_cast<int>(TabId::Count); ++i) {
        svc.TryLoadCachedResults(static_cast<TabId>(i));
    }
    // The active tab rendered before the cache load — re-render it now.
    if (auto* p = ActivePresenter()) p->Refresh();
    // v2.5: idle-time refresh cadence (see OnIdleCheck).
    SetTimer(hwnd_, TIMER_IDLE_REFRESH, 30 * 1000, nullptr);
    UpdateStatusBar();
    UpdateExecButton();
}

void MainWindow::OnSize() {
    RECT rc; GetClientRect(hwnd_, &rc);
    LayoutWindow(h_, rc.right, rc.bottom,
                 CurrentTab() == TabId::LargeFiles,
                 CurrentTab() == TabId::Search);
    // v2.5: the progress bar overlays the 4th status pane — reposition it
    // whenever the parts move.
    UpdateStatusBar();
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

// v2.6: one-click search filter presets (.pdb / .obj / Debug 文件夹 …).
// Selecting one rewrites the query's filter token (same category replaced,
// other categories and free text preserved) and runs the search.
void MainWindow::OnQuickFilterMenu() {
    struct QF { int id; const wchar_t* label; const wchar_t* token; };
    static const QF kPresets[] = {
        { IDM_QF_PDB,       L"调试符号  .pdb",                          L"ext:pdb" },
        { IDM_QF_OBJ,       L"编译中间文件  .obj",                       L"ext:obj" },
        { IDM_QF_BUILD_TMP, L"链接/构建临时  .ilk;.idb;.tlog",          L"ext:ilk;idb;tlog;lastbuildstate" },
        { IDM_QF_PCH,       L"预编译头  .ipch;.pch",                     L"ext:ipch;pch" },
        { 0, nullptr, nullptr },
        { IDM_QF_DIR_DEBUG,   L"Debug 文件夹",   L"folder:debug" },
        { IDM_QF_DIR_RELEASE, L"Release 文件夹", L"folder:release" },
        { IDM_QF_DIR_BIN,     L"bin 文件夹",     L"folder:bin" },
        { IDM_QF_DIR_OBJ,     L"obj 文件夹",     L"folder:obj" },
        { 0, nullptr, nullptr },
        { IDM_QF_EXE,     L"安装包/程序  .exe;.msi",   L"ext:exe;msi" },
        { IDM_QF_ARCHIVE, L"压缩包  .zip;.rar;.7z",    L"ext:zip;rar;7z" },
        { IDM_QF_VIDEO,   L"视频  .mp4;.mkv;.avi",     L"ext:mp4;mkv;avi" },
        { IDM_QF_IMAGE,   L"图片  .jpg;.png;.bmp",     L"ext:jpg;jpeg;png;bmp" },
        { IDM_QF_AUDIO,   L"音频  .mp3;.flac;.wav",    L"ext:mp3;flac;wav" },
        { IDM_QF_DOC,     L"文档  .doc;.xls;.pdf",     L"ext:doc;docx;xls;xlsx;pdf" },
        { 0, nullptr, nullptr },
        { IDM_QF_CLEAR,   L"清除全部过滤条件",  L"" },
    };

    HMENU m = CreatePopupMenu();
    wchar_t cur[512] = {};
    GetWindowTextW(h_.editSearch, cur, 512);
    std::wstring curLower = ToLower(cur);
    for (const auto& p : kPresets) {
        if (!p.label) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        } else {
            // review-07 X-8: mark the active preset so "what am I filtering
            // by" is visible on the menu itself.
            UINT flags = MF_STRING;
            if (p.token[0] &&
                curLower.find(ToLower(p.token)) != std::wstring::npos) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(m, flags, p.id, p.label);
        }
    }
    RECT rc{};
    GetWindowRect(h_.btnQuickFilter, &rc);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             rc.left, rc.bottom, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (!cmd) return;

    wchar_t buf[512] = {};
    GetWindowTextW(h_.editSearch, buf, 512);
    for (const auto& p : kPresets) {
        if (p.id != cmd) continue;
        if (cmd == IDM_QF_CLEAR) {
            // Strip ALL filter tokens, keep the free text.
            std::wstring free;
            VolumeIndex::ParseFilterTerms(buf, free);
            SetWindowTextW(h_.editSearch, free.c_str());
        } else {
            SetWindowTextW(h_.editSearch,
                VolumeIndex::ToggleQueryToken(buf, p.token).c_str());
        }
        break;
    }

    if (taskMode_ == TaskMode::None) {
        // review-07 X-8: SetWindowText above fired EN_CHANGE which armed the
        // 200 ms debounce — kill it or the query runs twice (the second run
        // cancelling and restarting the first for nothing).
        KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);
        RunSearch();
    } else {
        // Index still building (most common busy case here) — OnScanDone
        // re-applies the (now non-empty) query when it lands.
        ShowHint(L"ℹ 索引构建中，完成后将自动应用该筛选。");
    }
}

// v2.7: sync the visible list with reality after out-of-tool deletions.
// v2.8: the disk checks run on the worker (attribute queries against
// hundreds of rows would stutter the UI); WM_APP_VERIFY_DONE applies.
void MainWindow::OnVerifyList(bool manual) {
    if (taskMode_ != TaskMode::None) return;
    auto t = CurrentTab();
    if (t == TabId::History || t == TabId::FolderTree) {
        if (manual) ShowHint(L"ℹ 本页无需校验。");
        return;
    }
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;
    std::vector<std::wstring> paths;
    for (const auto& it : lp->Snapshot()) paths.push_back(it.path.wstring());
    if (!SessionService::Instance().VerifyPathsAsync(t, std::move(paths))) {
        if (manual) ShowHint(L"ℹ 已有任务在进行中，请稍候。");
    }
}

// v2.8: worker verify finished — prune the ghost rows (even if the user
// switched tabs meanwhile; the presenter outlives the switch).
void MainWindow::OnVerifyDone() {
    auto& svc = SessionService::Instance();
    auto dead = svc.LastDeadPaths();
    // review-07 X-3: nothing removed → silent success (pane2 already says
    // "校验完成"). This used to clobber the info dashboard on EVERY Alt-Tab.
    if (dead.empty()) return;
    auto tab = svc.LastVerifyTab();
    if (tab < TabId::Junk || tab >= TabId::Count) return;
    auto* lp = dynamic_cast<ListTabPresenter*>(
        presenters_[static_cast<size_t>(tab)].get());
    if (!lp) return;
    // review-04 R-1: the user may have switched tabs while the verify ran —
    // only the ACTIVE tab may render into the shared ListView; a background
    // tab prunes silently and shows its updated list on next activation.
    bool active = (CurrentTab() == tab);
    size_t removed = lp->ApplyDeadPaths(dead, /*render=*/active);
    if (removed > 0) {
        ShowHint(FormatW(L"ℹ 已移除 %zu 项（在磁盘上已不存在，可能已被手动删除）。", removed));
        if (active) UpdateExecButton();
    }
}

// v2.8: worker preview finished — show the dry-run report.
void MainWindow::OnPreviewDone() {
    const auto& rpt = SessionService::Instance().LastPreview();
    std::wstring content = FormatW(L"通过安全闸：%d 项（可执行）\n被拒：%d 项\n\n%s",
                                   rpt.pass, rpt.denied, rpt.details.c_str());
    MessageBoxW(hwnd_, content.c_str(), L"预览执行（不会做任何更改）",
                MB_OK | MB_ICONINFORMATION);
}

// v2.10 (review-07 blueprint #5): Ctrl+K command palette — every buried
// action at zero depth.
void MainWindow::OnCommandPalette() {
    std::vector<palette::Command> cmds;
    auto add = [&](const wchar_t* name, std::function<void()> run) {
        cmds.push_back({ name, std::move(run) });
    };

    bool searchTab = (CurrentTab() == TabId::Search);
    add(searchTab ? L"重建索引" : L"扫描当前页（F5）", [this] { OnScan(); });
    add(L"刷新列表 — 移除已手动删除的项", [this] { OnVerifyList(true); });
    add(L"全选 / 全不选切换（Ctrl+A）", [this] { OnSelectAll(); });
    add(L"预览执行 — 安全闸检查", [this] { OnPreviewExecution(); });
    add(L"清空隔离区…", [this] { OnEmptyQuarantine(); });
    add(L"关于 MiniSys", [this] { OnAbout(); });
    add(L"设置开发缓存扫描根…", [this] { OnDevCacheRoots(); });

    struct TabCmd { const wchar_t* name; TabId tab; };
    static const TabCmd kTabs[] = {
        { L"切换到 垃圾清理",   TabId::Junk },
        { L"切换到 文件搜索",   TabId::Search },
        { L"切换到 大文件/去重", TabId::LargeFiles },
        { L"切换到 应用迁移",   TabId::Apps },
        { L"切换到 文件夹分析", TabId::FolderTree },
        { L"切换到 操作历史",   TabId::History },
    };
    for (const auto& t : kTabs) {
        add(t.name, [this, tab = t.tab] {
            TabCtrl_SetCurSel(h_.tab, static_cast<int>(tab));
            OnTabChanged();
        });
    }

    // Quick filters jump to the search tab and apply the token.
    auto quickFilter = [this](const wchar_t* token) {
        TabCtrl_SetCurSel(h_.tab, static_cast<int>(TabId::Search));
        OnTabChanged();
        wchar_t buf[512] = {};
        GetWindowTextW(h_.editSearch, buf, 512);
        SetWindowTextW(h_.editSearch,
            VolumeIndex::ToggleQueryToken(buf, token).c_str());
        KillTimer(hwnd_, TIMER_SEARCH_DEBOUNCE);
        if (taskMode_ == TaskMode::None) RunSearch();
    };
    add(L"筛选：调试符号 .pdb", [quickFilter] { quickFilter(L"ext:pdb"); });
    add(L"筛选：编译中间文件 .obj", [quickFilter] { quickFilter(L"ext:obj"); });
    add(L"筛选：Debug 文件夹", [quickFilter] { quickFilter(L"folder:debug"); });
    add(L"筛选：bin 文件夹", [quickFilter] { quickFilter(L"folder:bin"); });

    palette::Show(hwnd_, cmds);
}

// v2.12: editor for the dev-build-cache scan roots (settings.json
// "devCacheRoots" had no UI entry — hand-editing JSON is not a workflow).
// Minimal modal dialog: multiline edit (one absolute path per line) +
// browse + OK/Cancel. Auto-detected conventional roots always run in
// addition to these.
namespace {
struct RootsDlgState {
    std::wstring joined;   // ';'-separated result
    bool ok = false;
};

LRESULT CALLBACK RootsDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDOK: {
                    auto* st = reinterpret_cast<RootsDlgState*>(
                        GetWindowLongPtrW(h, GWLP_USERDATA));
                    HWND edit = GetDlgItem(h, 1);
                    int len = GetWindowTextLengthW(edit);
                    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
                    GetWindowTextW(edit, text.data(), len + 1);
                    text.resize(len);
                    std::wstring joined, cur;
                    auto flush = [&] {
                        if (!cur.empty()) {
                            if (!joined.empty()) joined += L';';
                            joined += cur;
                        }
                        cur.clear();
                    };
                    for (wchar_t ch : text) {
                        if (ch == L'\n') flush;
                        else if (ch != L'\r') cur += ch;
                    }
                    flush();
                    st->joined = std::move(joined);
                    st->ok = true;
                    DestroyWindow(h);
                    return 0;
                }
                case IDCANCEL:
                    DestroyWindow(h);
                    return 0;
                case 100: {   // 浏览添加…（100：IDCANCEL 宏恰好等于 2）
                    BROWSEINFOW bi{};
                    bi.hwndOwner = h;
                    bi.lpszTitle = L"选择一个代码根目录（如 D:\\projects）";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t buf[MAX_PATH]{};
                        if (SHGetPathFromIDListW(pidl, buf) && buf[0]) {
                            HWND edit = GetDlgItem(h, 1);
                            int len = GetWindowTextLengthW(edit);
                            if (len > 0) {
                                SendMessageW(edit, EM_SETSEL, len, len);
                                SendMessageW(edit, EM_REPLACESEL, TRUE,
                                             reinterpret_cast<LPARAM>(L"\r\n"));
                            }
                            SendMessageW(edit, EM_REPLACESEL, TRUE,
                                         reinterpret_cast<LPARAM>(buf));
                        }
                        CoTaskMemFree(pidl);
                    }
                    return 0;
                }
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

void MainWindow::OnDevCacheRoots() {
    constexpr wchar_t kClass[] = L"MiniSysRootsDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &RootsDlgProc;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT or_{};
    GetWindowRect(hwnd_, &or_);
    const int W = UiScale(hwnd_, 520), H = UiScale(hwnd_, 300);
    RootsDlgState st;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass,
        L"开发构建缓存的额外扫描根目录",
        WS_POPUPWINDOW | WS_CAPTION,
        or_.left + ((or_.right - or_.left) - W) / 2,
        or_.top + ((or_.bottom - or_.top) - H) / 3,
        W, H, hwnd_, nullptr, hInst_, nullptr);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                  int x, int y, int w, int h, int id) {
        HWND c = CreateWindowExW(
            wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls,
            text, style, x, y, w, h, dlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    mk(L"STATIC", L"每行一个绝对路径（惯例目录 src/code/dev 等自动探测之外的额外根）：",
       WS_CHILD | WS_VISIBLE, UiScale(hwnd_, 10), UiScale(hwnd_, 10),
       W - UiScale(hwnd_, 30), UiScale(hwnd_, 18), 0);
    std::wstring lines;
    for (wchar_t ch : settings_.devCacheRoots) {
        lines += (ch == L';') ? L'\n' : ch;
    }
    HWND edit = mk(L"EDIT", lines.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
            WS_VSCROLL,
        UiScale(hwnd_, 10), UiScale(hwnd_, 32), W - UiScale(hwnd_, 30),
        H - UiScale(hwnd_, 100), 1);
    mk(L"BUTTON", L"浏览添加…",
       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
       UiScale(hwnd_, 10), H - UiScale(hwnd_, 60), UiScale(hwnd_, 110),
       UiScale(hwnd_, 26), 100);
    mk(L"BUTTON", L"确定",
       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
       W - UiScale(hwnd_, 195), H - UiScale(hwnd_, 60), UiScale(hwnd_, 88),
       UiScale(hwnd_, 26), IDOK);
    mk(L"BUTTON", L"取消",
       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
       W - UiScale(hwnd_, 100), H - UiScale(hwnd_, 60), UiScale(hwnd_, 90),
       UiScale(hwnd_, 26), IDCANCEL);

    EnableWindow(hwnd_, FALSE);   // modal
    ShowWindow(dlg, SW_SHOW);
    SetFocus(edit);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(hwnd_, TRUE);
    SetActiveWindow(hwnd_);

    if (!st.ok) return;
    // Validate: keep absolute existing directories only.
    std::wstring kept;
    std::wstring cur;
    std::error_code ec;
    size_t dropped = 0;
    auto flush = [&] {
        if (cur.empty()) return;
        if (cur.size() >= 3 && cur[1] == L':' && cur[2] == L'\\' &&
            std::filesystem::is_directory(std::filesystem::path(cur), ec)) {
            if (!kept.empty()) kept += L';';
            kept += cur;
        } else {
            ++dropped;
        }
        cur.clear();
    };
    for (wchar_t ch : st.joined) {
        if (ch == L';') flush;
        else cur += ch;
    }
    flush();
    settings_.devCacheRoots = kept;
    SaveSettings();
    ShowHint(dropped
        ? FormatW(L"✓ 已保存开发缓存扫描根（忽略了 %zu 个无效/不存在的路径）。", dropped)
        : L"✓ 已保存开发缓存扫描根，下次垃圾清理扫描生效。");
}
std::wstring MainWindow::ComposeScanTimeLine() const {
    auto& svc = SessionService::Instance();
    uint64_t at = svc.LastScanAt(CurrentTab());
    if (at == 0) return {};
    std::wstring line = L"上次扫描: " + FormatAge(at);
    if (svc.ResultsFromCache(CurrentTab())) line += L"（上次会话的缓存结果）";
    line += L" — 执行前会逐项复验文件是否已变化。\n";
    return line;
}

// v2.5 cached results: while the user has been idle for 5+ minutes and the
// active tab's data is older than 30 minutes, rescan it automatically.
// review-07 X-10 (v2.10): with a 10 s grace countdown — any input anywhere
// in the system cancels, so the rescan never ambushes a returning user.
void MainWindow::OnIdleCheck() {
    if (taskMode_ != TaskMode::None) return;
    if (idleCountdown_ > 0) return;   // countdown already armed
    LASTINPUTINFO li{ sizeof(li) };
    if (!GetLastInputInfo(&li)) return;
    DWORD idleMs = GetTickCount() - li.dwTime;
    if (idleMs < 5u * 60u * 1000u) return;
    if (!IsWindowVisible(hwnd_)) return;

    auto t = CurrentTab();
    if (t != TabId::Junk && t != TabId::LargeFiles && t != TabId::Apps &&
        t != TabId::FolderTree) {
        return;
    }
    auto& svc = SessionService::Instance();
    uint64_t at = svc.LastScanAt(t);
    uint64_t ageMs = 0;
    if (at != 0) {
        FILETIME ftNow{};
        SYSTEMTIME st{};
        GetLocalTime(&st);
        SystemTimeToFileTime(&st, &ftNow);
        uint64_t now = (static_cast<uint64_t>(ftNow.dwHighDateTime) << 32) |
                       ftNow.dwLowDateTime;
        ageMs = (now > at) ? (now - at) / 10000 : 0;
    }
    if (at != 0 && ageMs < 30ULL * 60 * 1000) {
        // v2.11: not stale enough for a full rescan, but stale enough for
        // idle maintenance — journal deltas + ghost prune through the index
        // (near-zero cost, keeps every tab's data fresh without rescanning).
        if (ageMs >= 10ULL * 60 * 1000) {
            svc.IdleMaintenanceAsync();
        }
        return;
    }

    idleCountdown_ = 10;
    idleArmInput_ = li.dwTime;
    ShowHint(L"ℹ 系统空闲 — 10 秒后自动刷新本页扫描结果（动一下鼠标或键盘即取消）");
    SetTimer(hwnd_, TIMER_IDLE_COUNTDOWN, 1000, nullptr);
}

void MainWindow::OnTabChanged() {
    auto t = CurrentTab();
    bool isHistory    = (t == TabId::History);
    bool isFolderTree = (t == TabId::FolderTree);
    bool isApps       = (t == TabId::Apps);
    bool isLargeFiles = (t == TabId::LargeFiles);
    bool isSearch     = (t == TabId::Search);
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

    // Search row (v2.3) / LargeFiles settings panel
    for (HWND h : { h_.lblSearch, h_.editSearch, h_.chkMatchPath }) {
        ShowWindow(h, isSearch ? SW_SHOW : SW_HIDE);
    }
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

    // REVIEW-UI P1 (L-25): the scan button on the search tab rebuilds the
    // index — relabel it.
    SetWindowTextW(h_.scan, t == TabId::Search ? L"重建索引" : L"扫描");

    // REVIEW-UI P1 (L-3): remember the static per-tab description so
    // OnScanDone can compose dashboard + notices + description in order.
    switch (t) {
        case TabId::Junk:
            tabInfoText_ =
                L"扫描系统/浏览器/开发缓存等可清理项；勾选后执行将移入隔离区，可在“操作历史”一键还原。\n"
                L"· 含程序员构建缓存（bin/obj/.vs/ipch/x64 等，仅限项目目录内；24 小时内编译过的跳过）。\n"
                L"⚠ 危险项（清空回收站、WinSxS 等）默认不勾选；“清空隔离区”后才真正释放空间。";
            break;
        case TabId::Search:
            tabInfoText_ =
                L"输入即搜（多词为“并且”，支持 * ? 通配符，Enter 立即刷新）；双击定位文件，勾选后可移入隔离区。\n"
                L"· 点“快速筛选”一键过滤：.pdb / .obj / Debug 文件夹等；手动过滤词：folder: / file: / ext:cpp;h。\n"
                L"ℹ 范围：所有固定磁盘的文件索引；上次的搜索词与结果会原样恢复；系统空闲时自动增量维护。";
            break;
        case TabId::LargeFiles:
            tabInfoText_ =
                L"按最小大小、文件类型（.ext;..）与磁盘过滤查找大文件与重复文件（默认全部固定磁盘）；点列标题排序。\n"
                L"⚠ 重复文件默认保留最新一份、预选其余副本；执行前会复验文件是否已变化。";
            break;
        case TabId::Apps:
            tabInfoText_ =
                L"扫描 C 盘已安装应用；先点“选择迁移目标盘…”，勾选后执行迁移，原位置以 Junction 保持可用。\n"
                L"⚠ 迁移前自动检查目标空间与文件占用；UWP/商店应用不支持迁移，已自动排除。";
            break;
        case TabId::FolderTree:
            tabInfoText_ =
                L"按磁盘显示顶层文件夹及其大小，快速定位空间大户。\n"
                L"⚠ 右键文件夹可移入隔离区（可撤销），系统目录受保护名单拦截。";
            break;
        case TabId::History:
            tabInfoText_ =
                L"操作历史：迁移与隔离区操作可一键撤销；“清空隔离区”将永久删除并释放空间。\n"
                L"⚠ 撤销时若原路径已被占用，文件将还原为 *.restored。";
            break;
        default: tabInfoText_.clear(); break;
    }
    // v2.5: provenance line for tabs with cached/persisted results.
    SetWindowTextW(h_.info, (ComposeScanTimeLine() + tabInfoText_).c_str());

    if (t == TabId::Search) {
        // v2.3: auto-build the index on first visit.
        // REVIEW-UI P1 (L-5): Indexing mode — tabs stay switchable.
        if (!VolumeIndex::AnyValid() &&
            !SessionService::Instance().IsBusy()) {
            if (SessionService::Instance().BuildIndexAsync()) {
                SetTaskBusy(TaskMode::Indexing);
            }
        } else {
            // v2.9: opening the tab NEVER re-runs the search — the cached
            // rows from the last query display as-is (restore-on-open);
            // Enter in the box or editing the query refreshes explicitly.
            UpdateSearchStatus();
        }
    }
    if (auto* p = ActivePresenter()) p->Refresh();
    UpdateStatusBar();
    UpdateExecButton();
    SaveSettings();   // v2.9: remember the active tab
    // Re-layout for the new tab (settings row shown/hidden dynamically).
    OnSize();
}

void MainWindow::UpdateStatusBar() {
    // Four status-bar parts: disk | quarantine/migrate-target | progress
    // text | (empty strip the progress BAR overlays — v2.5: the bar used to
    // float over the list's scrollbar at the bottom-right).
    RECT rc; GetClientRect(h_.status, &rc);
    int W = rc.right;
    int barW = UiScale(hwnd_, 220);
    int e0 = UiScale(hwnd_, 340), e1 = UiScale(hwnd_, 660);
    if (e1 > W - barW - 160) { e0 = W / 4; e1 = W / 2; }
    int e2 = (W - barW > e1 + 40) ? (W - barW) : (3 * W / 4);
    int edges[4] = { e0, e1, e2, -1 };
    SendMessageW(h_.status, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(edges));

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
    SendMessageW(h_.status, SB_SETTEXTW, 3,
                 reinterpret_cast<LPARAM>(L""));

    // Overlay the progress bar on the reserved 4th pane. review-07 X-20:
    // keep clear of the status bar's size grip at the far right.
    RECT pane{};
    if (SendMessageW(h_.status, SB_GETRECT, 3,
                     reinterpret_cast<LPARAM>(&pane))) {
        POINT tl{ pane.left, pane.top };
        POINT br{ pane.right, pane.bottom };
        MapWindowPoints(h_.status, hwnd_, &tl, 1);
        MapWindowPoints(h_.status, hwnd_, &br, 1);
        int grip = (W >= static_cast<int>(GetSystemMetrics(SM_CXVSCROLL)))
                       ? GetSystemMetrics(SM_CXVSCROLL) : 0;
        SetWindowPos(h_.progress, nullptr, tl.x + 2, tl.y + 2,
                     (br.x - tl.x) - 4 - grip, (br.y - tl.y) - 4, SWP_NOZORDER);
    }
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
    if (!scanner) {
        // v2.3: on the search tab the scan button rebuilds the index.
        // v2.10: FORCED — a "valid" but truncated index (the 703-entry
        // defect) used to make this a silent no-op (EnsureBuilt refreshed
        // incrementally and returned within milliseconds).
        if (CurrentTab() == TabId::Search && svc.BuildIndexAsync(/*forceRebuild=*/true)) {
            SetTaskBusy(TaskMode::Indexing);
        }
        return;
    }
    if (!svc.StartScan(CurrentTab(), std::move(scanner))) return;

    SetTaskBusy(TaskMode::Scanning);
    UpdateStatusBar();
}

// v2.3: result-count line for the search tab.
void MainWindow::UpdateSearchStatus() {
    auto* sp = dynamic_cast<SearchPresenter*>(ActivePresenter());
    if (!sp) return;
    // v2.11: aggregates over ALL indexed volumes (磁盘瘦身助手).
    if (!VolumeIndex::AnyValid()) {
        ShowHint(L"ℹ 文件索引构建中/不可用——完成后即可搜索（垃圾扫描也可用，较慢）。");
        return;
    }
    size_t total = VolumeIndex::TotalEntries();
    size_t shown = sp->Snapshot().size();
    wchar_t buf[512] = {};
    GetWindowTextW(h_.editSearch, buf, 512);
    if (buf[0] == L'\0') {
        // REVIEW-UI P2 (L-24, search-tab half): a <10k-entry "ready" index is
        // broken — the junk tab said so since v2.4; the search tab claimed
        // "索引就绪" regardless.
        if (total < 10000) {
            ShowHint(FormatW(
                L"⚠ 索引条目异常少（%s 项），搜索结果可能不完整，建议点“重建索引”。",
                FormatCount(total).c_str()));
        } else {
            ShowHint(FormatW(L"索引就绪：%s 项（%zu 个磁盘）。输入关键词即可开始搜索。",
                             FormatCount(total).c_str(),
                             VolumeIndex::ValidVolumeCount()));
        }
    } else {
        ShowHint(FormatW(L"匹配 %s 项（索引共 %s 项，最多显示前 1,000）。",
                         FormatCount(shown).c_str(),
                         FormatCount(total).c_str()));
    }
}

void MainWindow::OnScanDone() {
    auto& svc = SessionService::Instance();
    SetTaskBusy(TaskMode::None);

    auto t = CurrentTab();
    if (auto* p = ActivePresenter()) p->OnScanDone();
    // v2.3: after an index build, re-apply the pending search query.
    if (t == TabId::Search) {
        if (VolumeIndex::AnyValid()) {
            // v2.6: never auto-fire an empty search (it blanked the list
            // right after the index finished building).
            wchar_t buf[512] = {};
            GetWindowTextW(h_.editSearch, buf, 512);
            if (buf[0]) RunSearch();   // REVIEW-UI P1: worker-side re-filter
            else UpdateSearchStatus();
        } else {
            UpdateSearchStatus();
        }
        UpdateStatusBar();
        UpdateExecButton();
        return;
    }

    // REVIEW-UI P1 (L-3): compose the info area fresh — dashboard first,
    // then degradation notices, then the static tab description. The old
    // "append to the tail" order put the notices below the 3-line fold.
    std::wstring composed;
    if (t != TabId::History && svc.Results(t).empty()) {
        composed += L"✓ 未发现可处理项，系统状况良好。\n";
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
                composed += FormatW(
                    L"可释放：安全 %llu 项 / %s，谨慎 %llu 项 / %s（勾选后执行，进入隔离区）\n",
                    safe, FormatSize(safeB).c_str(),
                    cautious, FormatSize(cautiousB).c_str());
            }
        }
    }

    // REVIEW P1-5 (07-X10 / 05-T-C3): silent degradation made visible —
    // now placed BEFORE the static description so it stays above the fold.
    if (t == TabId::Junk) {
        const auto& rl = JunkRules::LastLoad();
        if (!rl.usedExternal && !rl.externalError.empty()) {
            composed += L"ℹ 外置规则不可用（" + rl.externalError + L"），已使用内置规则。\n";
        } else if (!rl.rejected.empty()) {
            composed += FormatW(L"ℹ 已拒绝 %zu 条不安全的外置规则（详见日志）。\n",
                                rl.rejected.size());
        }
        if (!VolumeIndex::AnyValid()) {
            composed += L"ℹ 快速索引未启用，本次为全量扫描（较慢）。\n";
        }
        // REVIEW-UI P1 (L-24): an index this small is broken — say so
        // instead of showing a confident "索引就绪".
        if (VolumeIndex::AnyValid() &&
            VolumeIndex::TotalEntries() < 10000) {
            composed += FormatW(L"⚠ 索引条目异常少（%zu 项），搜索结果可能不完整，建议重建索引。\n",
                                VolumeIndex::Instance().EntryCount());
        }
    }

    if (!composed.empty()) {
        SetWindowTextW(h_.info, (composed + ComposeScanTimeLine() + tabInfoText_).c_str());
    } else {
        SetWindowTextW(h_.info, (ComposeScanTimeLine() + tabInfoText_).c_str());
    }
    UpdateStatusBar();
    UpdateExecButton();
}

void MainWindow::SetTaskBusy(TaskMode mode) {
    taskMode_ = mode;
    bool busy = mode != TaskMode::None;
    bool scanning = mode == TaskMode::Scanning || mode == TaskMode::Indexing;

    // Everything that could race the worker on the results vector goes off;
    // the scan button doubles as Cancel while scanning (REVIEW P1-5 /
    // 07-X8: CancelScan existed with no caller).
    // REVIEW-UI P1 (L-5): Indexing keeps the TAB control enabled —
    // switching tabs is browsing, not acting.
    // review-07 X-12: Indexing doesn't touch results_ either — the cached
    // lists stay browsable/sortable/checkable while the index builds (the
    // same read-only-browsing reasoning as verify/preview in v2.8).
    // REVIEW-UI P1 (04-2): editSearch/chkMatchPath join the matrix.
    bool lockBrowse = (mode == TaskMode::Scanning || mode == TaskMode::Executing);
    for (HWND h : { h_.list, h_.tree, h_.btnSortSize, h_.btnSortTime }) {
        if (h) EnableWindow(h, lockBrowse ? FALSE : TRUE);
    }
    for (HWND h : { h_.exec, h_.undo, h_.emptyQ, h_.open,
                    h_.targetBtn, h_.advancedChk,
                    h_.editSearch, h_.chkMatchPath, h_.btnQuickFilter }) {
        if (h) EnableWindow(h, scanning ? FALSE : (busy ? FALSE : TRUE));
    }
    EnableWindow(h_.tab, mode == TaskMode::Indexing ? TRUE : (busy ? FALSE : TRUE));
    EnableWindow(h_.scan, scanning ? TRUE : (busy ? FALSE : TRUE));
    // REVIEW-UI P1 (L-25): on the search tab the button means "rebuild
    // index", not "scan".
    bool searchTab = (CurrentTab() == TabId::Search);
    SetWindowTextW(h_.scan, scanning ? L"取消"
                                     : (searchTab ? L"重建索引" : L"扫描"));
    // v2.5 (L-22, live-confirmed): while the button means CANCEL it must not
    // keep the magnifying-glass icon — swap to the red ✕ (this SDK has no
    // SIID_STOP; SIID_DELETE's glyph is the standard cancel cross) and back.
    if (scanning != scanIconIsStop_) {
        icons::SetStockButtonIcon(h_.scan, scanning ? SIID_DELETE : SIID_FIND);
        scanIconIsStop_ = scanning;
    }

    if (mode == TaskMode::None) {
        progDeterminate_ = false;
        SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(h_.progress, SW_HIDE);
        UpdateExecButton();
    } else if (scanning) {
        progDeterminate_ = false;
        SendMessageW(h_.progress, PBM_SETPOS, 0, 0);
        SendMessageW(h_.progress, PBM_SETMARQUEE, TRUE, 25);
        ShowWindow(h_.progress, SW_SHOW);
    } else {
        SendMessageW(h_.progress, PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(h_.progress, PBM_SETPOS, 0, 0);
        ShowWindow(h_.progress, SW_SHOW);
    }
}

// REVIEW-UI P1 (04-1/U-1): apply the pending search on the worker. An
// in-flight search is cancelled and retried via a short timer; other busy
// tasks simply defer the retry.
void MainWindow::RunSearch() {
    auto* sp = dynamic_cast<SearchPresenter*>(ActivePresenter());
    if (!sp) return;
    auto& svc = SessionService::Instance();
    wchar_t buf[512] = {};
    GetWindowTextW(h_.editSearch, buf, 512);
    sp->SetQuery(buf, Button_GetCheck(h_.chkMatchPath) == BST_CHECKED);

    if (svc.IsSearching()) {
        svc.CancelScan();   // abort the in-flight search at its checkpoint
    }
    if (!svc.SearchAsync(sp->Query(), sp->MatchPath())) {
        // Still busy (draining the cancelled search or another task) —
        // retry shortly; the UI stays responsive meanwhile.
        SetTimer(hwnd_, TIMER_SEARCH_RETRY, 80, nullptr);
        return;
    }
    UpdateSearchStatus();
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
        // review-07 X-15 (v2.10): compact form — the old label clipped at
        // the fixed button width ("…（123 项 · 123.4GB）" lost the tail).
        SetWindowTextW(h_.exec, FormatW(L"执行（%zu 项·%s）",
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
    // REVIEW-UI P1 (L-7): search results are arbitrary files — the
    // "cautious cache" wording was nonsense for a personal document.
    if (gCautious.count) {
        if (t == TabId::Search) {
            groups += line(L"ℹ 普通文件/文件夹", gCautious.count, gCautious.bytes,
                           L"将整体移入隔离区（原路径消失，相关快捷方式可能失效），可随时还原");
        } else {
            groups += line(L"ℹ 谨慎", gCautious.count, gCautious.bytes,
                           L"清理后系统/程序需重新下载或重建");
        }
    }
    if (gDelegate.count) groups += line(L"⚠ 系统组件", gDelegate.count, gDelegate.bytes,
                                        L"通过系统命令处理，不可自动撤销");
    if (hasRecycleBin) groups += L"⚠ 清空回收站 — 不可逆\n";
    if (infoOnly) {
        groups += FormatW(L"⊘ 仅提示 %zu 项 — 不可执行，已自动排除\n", infoOnly);
    }
    if (!dangerousNames.empty()) {
        groups += L"\n涉及: " + dangerousNames + L"\n";
    }
    // review-07 X-13 (v2.10): the actual item list — "🛡 安全 28 项" without
    // names asked users to confirm blind. First 12 + overflow line.
    {
        std::wstring names;
        size_t shown = 0;
        for (auto idx : selected) {
            if (shown >= 12) break;
            const auto& it = items[idx];
            names += FormatW(L"· %s — %s\n", it.title.c_str(),
                             FormatSize(it.sizeBytes).c_str());
            ++shown;
        }
        if (selected.size() > shown) {
            names += FormatW(L"…（共 %zu 项）\n", selected.size());
        }
        groups += L"\n【项目清单】\n" + names;
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
        // REVIEW-UI P2 (L-9): unified TaskDialog confirm (default CANCEL).
        confirm += L"\n确定继续？";
        if (!dialogs::ConfirmTask(hwnd_, L"确认操作",
                FormatW(L"将处理 %zu 项，合计 %s", selected.size(),
                        FormatSize(totalSize).c_str()),
                confirm,
                (gDelegate.count || hasRecycleBin))) {
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
    // defaults to CANCEL. REVIEW-UI P2 (L-9): unified TaskDialog.
    if (!dialogs::ConfirmTask(hwnd_, L"清空隔离区",
            L"永久删除隔离区中的所有文件？", confirm,
            /*warning=*/true, L"清空（不可恢复）")) {
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
    // REVIEW-UI P2 (L-18): version + the search tab finally documented.
    tc.pszMainInstruction = L"MiniSys — 磁盘瘦身助手  " MINISYS_VERSION_TITLE;
    tc.pszContent =
        L"安全、可逆的磁盘清理与迁移工具（所有固定磁盘）。所有文件操作先经安全闸复验，"
        L"默认移入隔离区、可一键还原。\n"
        L"\n【使用说明】\n"
        L"· 垃圾清理：扫描后勾选执行，项目移入隔离区；“清空隔离区”才真正释放空间。"
        L"含程序员构建缓存（bin/obj/.vs 等）。\n"
        L"· 文件搜索：输入即搜全盘（参考 Everything）；“快速筛选”一键列出 .pdb/.obj/Debug 文件夹"
        L"等；双击定位文件；索引缓存加速启动并自动增量更新。\n"
        L"· 大文件/去重：按大小、类型、磁盘过滤；重复文件保留最新一份，预选其余副本。\n"
        L"· 应用迁移：先选目标盘再执行；原位置以 Junction 保持路径可用，历史页可撤销。\n"
        L"· 文件夹分析：右键顶层文件夹可移入隔离区。\n"
        L"· 扫描结果会跨会话缓存并显示“上次扫描”时间；系统空闲时自动刷新过期结果"
        L"（执行前仍会逐项复验）。\n"
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
    // v2.9: session restore — search box, match-path toggle, active tab.
    // The cached result list (incl. the last search) is loaded separately
    // in OnCreate; together the app reopens exactly where it was left.
    SetWindowTextW(h_.editSearch, settings_.lastSearchQuery.c_str());
    Button_SetCheck(h_.chkMatchPath,
                    settings_.searchMatchPath ? BST_CHECKED : BST_UNCHECKED);
    int tab = settings_.lastTab;
    if (tab < 0 || tab >= static_cast<int>(TabId::Count)) tab = 0;
    TabCtrl_SetCurSel(h_.tab, tab);
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
    // v2.9: session-restore fields.
    GetWindowTextW(h_.editSearch, buf, 512);
    settings_.lastSearchQuery = buf;
    settings_.searchMatchPath = (Button_GetCheck(h_.chkMatchPath) == BST_CHECKED);
    settings_.lastTab = static_cast<int>(CurrentTab());
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
    // REVIEW-UI P1 (L-6/04-7): suppress the per-row event storm, then
    // recount once.
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (lp) lp->SetBatchUpdate(true);
    for (int i = 0; i < n; ++i) {
        ListView_SetCheckState(h_.list, i, target ? TRUE : FALSE);
    }
    if (lp) lp->SetBatchUpdate(false);
    UpdateExecButton();
}

// REVIEW P2 (08-F7): dry-run preview — batch GuardRails::Validate over the
// checked rows and show which would pass / be denied, with reasons. Pure
// (no side effects); state can drift before a real execute, which planHash
// and the per-item re-verification still guard.
// v2.8: the validation loop (canonical-path I/O per item) moved to the
// worker — the dialog now appears from WM_APP_PREVIEW_DONE.
void MainWindow::OnPreviewExecution() {
    auto t = CurrentTab();
    auto* lp = dynamic_cast<ListTabPresenter*>(ActivePresenter());
    if (!lp) return;
    auto checked = lp->CollectChecked();
    if (checked.empty()) {
        ShowHint(L"ℹ 请先勾选要预览的项目。");
        return;
    }
    if (!SessionService::Instance().PreviewAsync(t, lp->Snapshot(), std::move(checked))) {
        ShowHint(L"ℹ 已有任务在进行中，请稍候。");
    }
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
    // REVIEW-UI P2 (L-15): the Enter shortcut hint is only true on the
    // search tab (Enter activates the row → locate). "此文件夹" was wrong
    // for file rows — files and folders both get "此项目".
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_OPEN,
        t == TabId::Search ? L"打开所在位置\tEnter" : L"打开所在位置");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_INFO,       L"说明（这是什么？）…");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_SELECTALL,  L"全选\tCtrl+A");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_SELECTNONE, L"全不选");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_PREVIEW,    L"预览执行（安全闸检查）…");
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_EXCLUDE,    L"永不清理此项目");
    // v2.7: hand-deleted rows — full rescan is ~30 s, existence-checking
    // the current list is <100 ms.
    AppendMenuW(hMenu, MF_STRING, IDM_LIST_VERIFY,     L"刷新列表（移除已手动删除的项）");
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
