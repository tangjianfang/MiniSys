#include "util/StringUtils.h"
#include <windows.h>
#include <cwchar>
#include <cstdarg>
#include <cwctype>
#include <vector>

namespace minisys {

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring FormatSize(unsigned long long bytes) {
    constexpr const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < 5) { v /= 1024.0; ++idx; }
    wchar_t buf[64];
    if (idx == 0) {
        swprintf_s(buf, L"%llu %s", bytes, units[idx]);
    } else {
        swprintf_s(buf, L"%.2f %s", v, units[idx]);
    }
    return buf;
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool IEndsWith(std::wstring_view s, std::wstring_view suffix) {
    if (suffix.size() > s.size()) return false;
    auto a = s.substr(s.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(suffix[i])) return false;
    }
    return true;
}

bool IEquals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

std::wstring FormatW(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n <= 0) { va_end(ap2); return {}; }
    std::wstring r(static_cast<size_t>(n), L'\0');
    vswprintf_s(r.data(), static_cast<size_t>(n) + 1, fmt, ap2);
    va_end(ap2);
    return r;
}

} // namespace minisys
