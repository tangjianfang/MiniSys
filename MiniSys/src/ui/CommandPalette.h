#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace minisys {
namespace palette {

// One runnable entry.
struct Command {
    std::wstring name;              // display name (type-to-filter matches this)
    std::function<void()> run;      // executed on the calling (UI) thread
};

// review-07 blueprint #5 (v2.10): Ctrl+K command palette — a small modal
// popup with an edit box and a filtered list. Enter / double-click runs the
// selected command, Up/Down move, Esc closes. The window is destroyed
// BEFORE the command runs (commands may open their own dialogs).
void Show(HWND owner, const std::vector<Command>& commands);

} // namespace palette
} // namespace minisys
