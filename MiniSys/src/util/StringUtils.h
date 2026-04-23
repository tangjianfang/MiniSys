#pragma once
#include <string>
#include <string_view>

namespace minisys {

std::wstring Utf8ToWide(std::string_view s);
std::string  WideToUtf8(std::wstring_view s);

// Format human-readable size: 1.23 MB / 456 KB / etc.
std::wstring FormatSize(unsigned long long bytes);

// Lowercase ASCII helpers
std::wstring ToLower(std::wstring s);

// Case-insensitive ends with
bool IEndsWith(std::wstring_view s, std::wstring_view suffix);
bool IEquals(std::wstring_view a, std::wstring_view b);

// printf-style wide formatting
std::wstring FormatW(const wchar_t* fmt, ...);

} // namespace minisys
