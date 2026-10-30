#include "ui/UiAudit.h"
#include "ui/Layout.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <commctrl.h>
#include <algorithm>

namespace minisys {

namespace {

struct Named { HWND h; const wchar_t* name; };

std::vector<Named> AllControls(const UiHandles& u) {
    std::vector<Named> v = {
        { u.tab, L"tab" },               { u.about, L"about" },
        { u.scan, L"scan" },             { u.exec, L"exec" },
        { u.undo, L"undo" },             { u.emptyQ, L"emptyQ" },
        { u.open, L"open" },             { u.targetBtn, L"targetBtn" },
        { u.advancedChk, L"advancedChk" },{ u.info, L"info" },
        { u.lblMinSize, L"lblMinSize" }, { u.editMinSize, L"editMinSize" },
        { u.lblMinSizeUnit, L"lblMinSizeUnit" },
        { u.lblFileType, L"lblFileType" },{ u.editFileType, L"editFileType" },
        { u.btnDrives, L"btnDrives" },   { u.lblSearch, L"lblSearch" },
        { u.editSearch, L"editSearch" }, { u.chkMatchPath, L"chkMatchPath" },
        { u.btnQuickFilter, L"btnQuickFilter" },
        { u.list, L"list" },             { u.tree, L"tree" },
        { u.status, L"status" },         { u.progress, L"progress" },
    };
    v.erase(std::remove_if(v.begin(), v.end(), [](const Named& n) {
        return !n.h || !IsWindowVisible(n.h);
    }), v.end());
    return v;
}

RECT RectInMain(HWND main, HWND c) {
    RECT r{};
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, main, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

bool Intersects(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

// Pairs that overlap on purpose: the progress bar rides on the status bar's
// 4th pane; 关于 sits on the tab strip's right end.
bool OverlapIsByDesign(const wchar_t* a, const wchar_t* b) {
    auto pair = [&](const wchar_t* x, const wchar_t* y) {
        return (wcscmp(a, x) == 0 && wcscmp(b, y) == 0) ||
               (wcscmp(a, y) == 0 && wcscmp(b, x) == 0);
    };
    return pair(L"status", L"progress") || pair(L"tab", L"about");
}

bool ClassIs(HWND h, const wchar_t* cls) {
    wchar_t buf[32] = {};
    GetClassNameW(h, buf, 32);
    return _wcsicmp(buf, cls) == 0;
}

// Line count the info card needs at its current width (same maths as
// LayoutWindow: DT_CALCRECT inside 8px side padding, 17px per line).
int InfoLinesNeeded(HWND info) {
    wchar_t buf[2048] = {};
    int len = GetWindowTextW(info, buf, 2048);
    if (len <= 0) return 0;
    RECT cr{}; GetClientRect(info, &cr);
    HDC dc = GetDC(info);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HGDIOBJ>(
        SendMessageW(info, WM_GETFONT, 0, 0)));
    RECT rc{ 0, 0, cr.right - UiScale(info, 16), 0 };
    DrawTextW(dc, buf, len, &rc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(info, dc);
    int lineH = UiScale(info, 17);
    return (rc.bottom + lineH - 1) / lineH;
}

// Print `ctl` off-screen onto a sentinel-filled bitmap and look at its four
// inset corners. Returns the first painted (non-sentinel) corner that is NOT
// `want`, or CLR_INVALID when the backdrop is fine: at least one corner is
// `want` (text can hide some corners, never all four), or nothing was
// painted at all (a transparent label shows the parent's chrome).
COLORREF BackdropMismatch(HWND ctl, COLORREF want) {
    RECT cr{}; GetClientRect(ctl, &cr);
    const int w = cr.right, h = cr.bottom;
    if (w < 4 || h < 4) return CLR_INVALID;

    const COLORREF kSentinel = RGB(255, 0, 255);
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH sb = CreateSolidBrush(kSentinel);
    FillRect(mem, &cr, sb);
    DeleteObject(sb);
    SendMessageW(ctl, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem),
                 PRF_CLIENT | PRF_ERASEBKGND);

    const POINT corners[4] = { {1, 1}, {w - 2, 1}, {1, h - 2}, {w - 2, h - 2} };
    COLORREF firstOther = CLR_INVALID;
    bool matches = false;
    for (const auto& p : corners) {
        COLORREF c = GetPixel(mem, p.x, p.y);
        if (c == want) matches = true;
        else if (c != kSentinel && firstOther == CLR_INVALID) firstOther = c;
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return matches ? CLR_INVALID : firstOther;
}

struct Variant { const wchar_t* ctl; const wchar_t* text; };

// Worst-case DYNAMIC captions, next to the widths they must fit
// (Layout.cpp: execW 204, scanW 96, drive picker 168 at 96 dpi).
const Variant kVariants[] = {
    { L"exec",      L"执行（99999 项·999.99 GB）" },
    { L"scan",      L"重建索引" },
    { L"scan",      L"取消" },
    { L"btnDrives", L"扫描盘: A B C D E ▾" },
    { L"btnDrives", L"扫描盘: 全部固定盘 ▾" },
    { L"btnDrives", L"扫描盘: 系统盘 ▾" },
};

} // namespace

bool CaptionFits(HWND ctl, const std::wstring& text, int* needed, int* avail) {
    HDC dc = GetDC(ctl);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HGDIOBJ>(
        SendMessageW(ctl, WM_GETFONT, 0, 0)));
    RECT r{};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(ctl, dc);

    int need = r.right - r.left;
    if (ClassIs(ctl, L"Button")) {
        DWORD type = GetWindowLongW(ctl, GWL_STYLE) & BS_TYPEMASK;
        if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
            type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) {
            need += UiScale(ctl, 22);                 // glyph + gap
        } else {
            need += UiScale(ctl, 16);                 // push-button margins
            if (SendMessageW(ctl, BM_GETIMAGE, IMAGE_ICON, 0)) {
                need += GetSystemMetrics(SM_CXSMICON) + UiScale(ctl, 8);
            }
        }
    }
    RECT cr{}; GetClientRect(ctl, &cr);
    if (needed) *needed = need;
    if (avail) *avail = cr.right;
    return need <= cr.right;
}

AuditReport AuditVisibleControls(const UiHandles& ui, COLORREF chromeBg) {
    AuditReport rep;
    RECT client{}; GetClientRect(ui.main, &client);
    auto ctls = AllControls(ui);

    std::vector<RECT> rects;
    rects.reserve(ctls.size());
    for (const auto& c : ctls) rects.push_back(RectInMain(ui.main, c.h));

    for (size_t i = 0; i < ctls.size(); ++i) {
        const RECT& r = rects[i];
        const int w = r.right - r.left, h = r.bottom - r.top;
        rep.inventory.push_back(FormatW(L"%-15s (%4ld,%4ld)-(%4ld,%4ld)  %4dx%-4d",
            ctls[i].name, r.left, r.top, r.right, r.bottom, w, h));

        // bounds
        if (w <= 0 || h <= 0) {
            rep.problems.push_back(FormatW(L"bounds: %s is empty (%dx%d)",
                                           ctls[i].name, w, h));
        } else if (r.left < 0 || r.top < 0 ||
                   r.right > client.right || r.bottom > client.bottom) {
            rep.problems.push_back(FormatW(
                L"bounds: %s (%ld,%ld)-(%ld,%ld) leaves the %ldx%ld client area",
                ctls[i].name, r.left, r.top, r.right, r.bottom,
                client.right, client.bottom));
        }

        // overlap
        for (size_t j = i + 1; j < ctls.size(); ++j) {
            if (Intersects(r, rects[j]) &&
                !OverlapIsByDesign(ctls[i].name, ctls[j].name)) {
                rep.problems.push_back(FormatW(L"overlap: %s and %s intersect",
                                               ctls[i].name, ctls[j].name));
            }
        }

        // covered: what the mouse would actually reach at the centre must be
        // the control (or its own child, e.g. a list header). WindowFromPoint
        // sends WM_NCHITTEST, so a click-through window such as the tab
        // strip's blank area (HTTRANSPARENT) is correctly looked through; it
        // skips DISABLED windows, for those fall back to the rect-only query.
        // A hit outside our own window (another app on top) proves nothing.
        if (w > 4 && h > 4) {
            POINT pt{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            HWND hit;
            if (IsWindowEnabled(ctls[i].h)) {
                POINT sp = pt;
                ClientToScreen(ui.main, &sp);
                hit = WindowFromPoint(sp);
                if (hit && GetAncestor(hit, GA_ROOT) != GetAncestor(ui.main, GA_ROOT)) {
                    hit = ctls[i].h;   // foreign window on top: undecidable, not a defect
                }
                // Landing on the main window itself means every child here is
                // click-through (STATIC labels, the tab strip's blank area):
                // nothing intercepts — and a parent never "covers" a child.
                if (hit == ui.main) hit = ctls[i].h;
            } else {
                hit = ChildWindowFromPointEx(ui.main, pt, CWP_SKIPINVISIBLE);
            }
            if (hit != ctls[i].h && !(hit && IsChild(ctls[i].h, hit))) {
                wchar_t cls[32] = {};
                if (hit) GetClassNameW(hit, cls, 32);
                rep.problems.push_back(FormatW(
                    L"covered: centre of %s is hit by %s", ctls[i].name,
                    hit ? cls : L"(nothing)"));
            }
        }

        // caption: current text of single-line captions.
        const bool isCaption =
            (ClassIs(ctls[i].h, L"Button") || ClassIs(ctls[i].h, L"Static")) &&
            ctls[i].h != ui.info;
        if (isCaption) {
            wchar_t txt[256] = {};
            GetWindowTextW(ctls[i].h, txt, 256);
            int need = 0, avail = 0;
            if (txt[0] && !CaptionFits(ctls[i].h, txt, &need, &avail)) {
                rep.problems.push_back(FormatW(
                    L"caption: %s \"%s\" needs %d px, has %d px",
                    ctls[i].name, txt, need, avail));
            }
        }
    }

    // backdrop: plain labels must sit on the chrome, not on stock grey.
    if (chromeBg != CLR_INVALID) {
        for (const auto& c : ctls) {
            if (!ClassIs(c.h, L"Static") || c.h == ui.info) continue;
            COLORREF bad = BackdropMismatch(c.h, chromeBg);
            if (bad != CLR_INVALID) {
                rep.problems.push_back(FormatW(
                    L"backdrop: %s paints RGB(%d,%d,%d) behind its text, chrome is RGB(%d,%d,%d)",
                    c.name, GetRValue(bad), GetGValue(bad), GetBValue(bad),
                    GetRValue(chromeBg), GetGValue(chromeBg), GetBValue(chromeBg)));
            }
        }
    }

    // caption: worst-case dynamic text for the controls that are visible.
    for (const auto& v : kVariants) {
        for (const auto& c : ctls) {
            if (wcscmp(c.name, v.ctl) != 0) continue;
            int need = 0, avail = 0;
            if (!CaptionFits(c.h, v.text, &need, &avail)) {
                rep.problems.push_back(FormatW(
                    L"caption: %s worst case \"%s\" needs %d px, has %d px",
                    c.name, v.text, need, avail));
            }
        }
    }

    // content: usable list/tree, info text within its 6-line cap.
    for (const auto& c : ctls) {
        if (wcscmp(c.name, L"list") != 0 && wcscmp(c.name, L"tree") != 0) continue;
        RECT r = RectInMain(ui.main, c.h);
        if (r.right - r.left < UiScale(ui.main, 400) ||
            r.bottom - r.top < UiScale(ui.main, 150)) {
            rep.problems.push_back(FormatW(
                L"content: %s is only %ldx%ld", c.name,
                r.right - r.left, r.bottom - r.top));
        }
    }
    if (ui.info && IsWindowVisible(ui.info)) {
        int lines = InfoLinesNeeded(ui.info);
        if (lines > 6) {
            rep.problems.push_back(FormatW(
                L"content: info text needs %d lines but the card caps at 6", lines));
        }
    }
    return rep;
}

} // namespace minisys
