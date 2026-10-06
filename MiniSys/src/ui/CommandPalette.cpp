#include "ui/CommandPalette.h"

#include "util/StringUtils.h"

#include <windowsx.h>
#include <commctrl.h>

namespace minisys {
namespace palette {

namespace {

constexpr wchar_t kClass[] = L"MiniSysPalette";

struct State {
    std::vector<Command> all;
    std::vector<size_t> visible;    // indices into all
    HWND wnd = nullptr;
    HWND edit = nullptr;
    HWND list = nullptr;
    bool done = false;
    int runIdx = -1;                // index into visible; -1 = cancelled
};

void Refill(State& st) {
    wchar_t buf[256] = {};
    GetWindowTextW(st.edit, buf, 256);
    std::wstring needle = ToLower(buf);
    SendMessageW(st.list, LB_RESETCONTENT, 0, 0);
    st.visible.clear();
    for (size_t i = 0; i < st.all.size(); ++i) {
        if (needle.empty() ||
            ToLower(st.all[i].name).find(needle) != std::wstring::npos) {
            SendMessageW(st.list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(st.all[i].name.c_str()));
            st.visible.push_back(i);
        }
    }
    if (!st.visible.empty()) {
        SendMessageW(st.list, LB_SETCURSEL, 0, 0);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND:
            // v2.13b review fix: the OK/Cancel BUTTONS were dead — clicks
            // posted WM_COMMAND IDOK/IDCANCEL that nobody handled; only
            // Enter/Esc worked. (Note the EDIT's control id is also 1/IDOK
            // — disambiguate via the notification code.)
            if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == IDOK) {
                int sel = static_cast<int>(SendMessageW(st->list, LB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(st->visible.size())) {
                    st->runIdx = sel;
                }
                st->done = true;
                return 0;
            }
            if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == IDCANCEL) {
                st->done = true;
                return 0;
            }
            if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == 1) {
                Refill(*st);
                return 0;
            }
            if (HIWORD(wp) == LBN_DBLCLK && LOWORD(wp) == 2) {
                st->runIdx = static_cast<int>(
                    SendMessageW(st->list, LB_GETCURSEL, 0, 0));
                st->done = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            st->done = true;
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void Show(HWND owner, const std::vector<Command>& commands) {
    if (commands.empty()) return;

    WNDCLASSW wc{};
    if (!GetClassInfoW(GetModuleHandleW(nullptr), kClass, &wc)) {
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClass;
        if (!RegisterClassW(&wc)) return;
    }

    RECT or_{};   // "owner rect" — or_ is unfortunate but unambiguous here
    GetWindowRect(owner, &or_);
    const int W = 460, H = 320;
    int x = or_.left + ((or_.right - or_.left) - W) / 2;
    int y = or_.top + ((or_.bottom - or_.top) - H) / 4;   // upper third

    State st;
    st.all = commands;

    st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass,
        L"命令面板 — 输入过滤，Enter 执行，Esc 关闭",
        WS_POPUPWINDOW | WS_CAPTION,
        x, y, W, H, owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!st.wnd) return;
    SetWindowLongPtrW(st.wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));

    HINSTANCE inst = GetModuleHandleW(nullptr);
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        8, 8, W - 16 - 4, 24, st.wnd, reinterpret_cast<HMENU>(1), inst, nullptr);
    st.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            WS_VSCROLL,
        8, 40, W - 16 - 4, H - 40 - 48 - 8, st.wnd,
        reinterpret_cast<HMENU>(2), inst, nullptr);
    HWND hint = CreateWindowExW(0, L"STATIC",
        L"Enter 执行 · 双击执行 · ↑↓ 选择 · Esc 关闭",
        WS_CHILD | WS_VISIBLE, 8, H - 44, W - 24, 20, st.wnd, nullptr, inst, nullptr);
    for (HWND c : { st.edit, st.list, hint }) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    Refill(st);
    ShowWindow(st.wnd, SW_SHOW);
    SetFocus(st.edit);

    // Modal loop (main-window style: pre-dispatch interception for keys).
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) break;
            if (msg.wParam == VK_RETURN) {
                int sel = static_cast<int>(SendMessageW(st.list, LB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(st.visible.size())) {
                    st.runIdx = sel;
                }
                break;
            }
            if (msg.wParam == VK_UP || msg.wParam == VK_DOWN) {
                int sel = static_cast<int>(SendMessageW(st.list, LB_GETCURSEL, 0, 0));
                int n = static_cast<int>(SendMessageW(st.list, LB_GETCOUNT, 0, 0));
                sel += (msg.wParam == VK_DOWN) ? 1 : -1;
                if (sel >= 0 && sel < n) {
                    SendMessageW(st.list, LB_SETCURSEL, sel, 0);
                }
                continue;
            }
        }
        if (!IsDialogMessageW(st.wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Destroy BEFORE running the command — commands may open dialogs of
    // their own and must not nest inside this popup.
    Command toRun;
    bool haveRun = false;
    if (st.runIdx >= 0 && st.runIdx < static_cast<int>(st.visible.size())) {
        toRun = st.all[st.visible[st.runIdx]];
        haveRun = toRun.run != nullptr;
    }
    if (IsWindow(st.wnd)) DestroyWindow(st.wnd);
    if (haveRun) toRun.run();
}

} // namespace palette
} // namespace minisys
