#pragma once
#include <windows.h>

namespace minisys {
namespace icons {

// Attach a classic Windows shell stock icon (SHGetStockIconInfo) to a
// button, rendered left of the text via BUTTON_IMAGELIST (comctl32 v6).
// Returns false when the stock icon is unavailable (button stays text-only).
bool SetStockButtonIcon(HWND button, int stockIconId);

} // namespace icons
} // namespace minisys
