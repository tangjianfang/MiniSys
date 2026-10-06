#pragma once
#include <string>
#include <string_view>

namespace minisys {

enum class LogLevel { Debug, Info, Warn, Error };

class Logger {
public:
    static Logger& Instance();
    void Log(LogLevel lvl, std::wstring_view msg);
    void LogF(LogLevel lvl, const wchar_t* fmt, ...);

    // REVIEW P2 (03-B13): unit tests used to write "Scanner threw: boom"
    // noise into the user's real minisys.log — redirect for test binaries.
    static void SetLogPathForTesting(const std::wstring& path);
private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    struct Impl;
    Impl* impl_;
};

} // namespace minisys

#define MS_LOG_INFO(...)  ::minisys::Logger::Instance().LogF(::minisys::LogLevel::Info,  __VA_ARGS__)
#define MS_LOG_WARN(...)  ::minisys::Logger::Instance().LogF(::minisys::LogLevel::Warn,  __VA_ARGS__)
#define MS_LOG_ERROR(...) ::minisys::Logger::Instance().LogF(::minisys::LogLevel::Error, __VA_ARGS__)
#define MS_LOG_DEBUG(...) ::minisys::Logger::Instance().LogF(::minisys::LogLevel::Debug, __VA_ARGS__)
