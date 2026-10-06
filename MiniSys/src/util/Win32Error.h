#pragma once
#include <string>

namespace minisys {

// Human-readable Win32 error text with an actionable suggestion
// (REVIEW P1-2 / 07-X4 / 08-F4): "MoveFileEx failed (Win32 5)" told the
// user nothing — the npm-cache failure was unreadable.
std::wstring Win32ErrorText(unsigned long win32Error);

// Full diagnosis line: friendly text + (original code kept for the log).
std::wstring FriendlyWin32Error(const std::wstring& what,
                                unsigned long win32Error);

} // namespace minisys
