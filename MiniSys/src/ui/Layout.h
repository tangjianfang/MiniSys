#pragma once
#include "ui/UiHandles.h"

namespace minisys {

// Positions all child controls for the current window size.
// showSettings = whether the LargeFiles settings row is visible;
// showSearch  = whether the instant-search row is visible (v2.3).
void LayoutWindow(const UiHandles& ui, int clientW, int clientH,
                  bool showSettings, bool showSearch = false);

} // namespace minisys
