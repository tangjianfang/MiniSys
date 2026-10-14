#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstdarg>

namespace minisys {

struct Logger::Impl {
    std::mutex mu;
    // B-2 (review r6-03, P0 · experimentally confirmed): this used to be a
    // default-locale std::wofstream — wchar→char conversion via the "C"
    // locale set fail+bad on the FIRST non-ASCII character of a message
    // (every "—"/CJK format string) and every subsequent write in the
    // process was silently dropped. That is why no walk/search diagnostics
    // ever reached the field log. A narrow stream + explicit UTF-8
    // conversion cannot enter that state.
    std::ofstream stream;
};

static const wchar_t* LevelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return L"DEBUG";
        case LogLevel::Info:  return L"INFO ";
        case LogLevel::Warn:  return L"WARN ";
        case LogLevel::Error: return L"ERROR";
    }
    return L"     ";
}

Logger::Logger() : impl_(new Impl) {
    auto dir = LogsDir();
    auto path = dir / L"minisys.log";

    // REVIEW P3: rotation — one .old generation at 5 MB, so years of
    // session logs cannot grow unbounded.
    std::error_code ec;
    if (auto size = std::filesystem::file_size(path, ec);
        !ec && size > 5ull * 1024 * 1024) {
        auto old = path;
        old += L".old";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    impl_->stream.open(path, std::ios::out | std::ios::app |
                                  std::ios::binary);
}

Logger::~Logger() {
    delete impl_;
}

void Logger::SetLogPathForTesting(const std::wstring& path) {
    // Must run before Instance() is first touched.
    auto& inst = Instance();
    if (inst.impl_->stream.is_open()) inst.impl_->stream.close();
    inst.impl_->stream.open(path, std::ios::out | std::ios::trunc |
                                      std::ios::binary);
}

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

void Logger::Log(LogLevel lvl, std::wstring_view msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring wide(ts);
    wide += L" [";
    wide += LevelTag(lvl);
    wide += L"] ";
    wide += msg;
    std::lock_guard<std::mutex> g(impl_->mu);
    if (impl_->stream.is_open()) {
        impl_->stream << WideToUtf8(wide) << "\n";
        impl_->stream.flush();
    }
    wide += L"\n";
    OutputDebugStringW(wide.c_str());
}

void Logger::LogF(LogLevel lvl, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    std::wstring buf(static_cast<size_t>(n), L'\0');
    va_start(ap, fmt);
    vswprintf_s(buf.data(), static_cast<size_t>(n) + 1, fmt, ap);
    va_end(ap);
    Log(lvl, buf);
}

} // namespace minisys
