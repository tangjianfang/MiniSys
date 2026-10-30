#pragma once
#include "ui/UiHandles.h"

#include <string>
#include <vector>

namespace minisys {

// v2.25 (win32-gui-tool-dev §3 layer ②): NON-VISUAL layout audit of the
// controls that are visible right now. Pure measurement — it can prove a
// control is clipped, covered, overlapping or out of bounds; it says nothing
// about whether the window looks good.
//
// Checks (each failure is one line in `problems`):
//   bounds   – inside the client area and non-empty
//   overlap  – no two visible controls intersect (documented pairs excepted)
//   covered  – hit-test at the control's centre lands on the control itself
//   caption  – the current caption AND the worst-case dynamic captions
//              (exec button with a 5-digit count, drive picker with 5
//              drives, scan↔取消/重建索引) fit on one line
//   content  – list/tree keeps a usable size; info text fits its 6-line cap
//   backdrop – every plain label paints the window's chrome colour behind its
//              text (printed off-screen with WM_PRINTCLIENT; a stock-grey
//              slab on the near-white chrome shows up as a mismatch)
struct AuditReport {
    std::vector<std::wstring> inventory;   // one line per visible control
    std::vector<std::wstring> problems;
};

// chromeBg: the window background labels must match; CLR_INVALID skips the
// backdrop check (high-contrast themes keep the system surface).
AuditReport AuditVisibleControls(const UiHandles& ui,
                                 COLORREF chromeBg = CLR_INVALID);

// Does `text` fit `ctl` on one line at the control's current size and font?
// (push button: text + margins [+ icon]; checkbox: text + glyph; label: text.)
bool CaptionFits(HWND ctl, const std::wstring& text,
                 int* needed = nullptr, int* avail = nullptr);

} // namespace minisys
