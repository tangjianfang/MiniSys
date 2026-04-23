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
    std::wofstream stream;
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
    impl_->stream.open(path, std::ios::out | std::ios::app);
}

Logger::~Logger() {
    delete impl_;
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
    std::lock_guard<std::mutex> g(impl_->mu);
    if (impl_->stream.is_open()) {
        impl_->stream << ts << L" [" << LevelTag(lvl) << L"] " << msg << L"\n";
        impl_->stream.flush();
    }
    OutputDebugStringW((std::wstring(ts) + L" [" + LevelTag(lvl) + L"] " + std::wstring(msg) + L"\n").c_str());
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
