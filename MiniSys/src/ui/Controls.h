#pragma once
#include "ui/UiHandles.h"
#include <windows.h>

namespace minisys {

// Creates all main-window child controls (tab, list, tree, buttons, edits,
// status bar, progress bar) and returns their handles. Layout is applied
// separately by LayoutWindow().
UiHandles CreateControls(HWND parent, HINSTANCE inst);

} // namespace minisys
