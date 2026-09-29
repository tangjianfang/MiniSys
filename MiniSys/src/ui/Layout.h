#pragma once
#include "ui/UiHandles.h"

namespace minisys {

// Positions all child controls for the current window size.
// showSettings = whether the LargeFiles settings row is visible.
void LayoutWindow(const UiHandles& ui, int clientW, int clientH, bool showSettings);

} // namespace minisys
