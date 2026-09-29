#include "platform/CrashDump.h"

#include "util/Logger.h"
#include "util/PathUtils.h"

#include <windows.h>
#include <dbghelp.h>

namespace minisys {

namespace {

std::wstring DumpFileName() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"minisys-%04d%02d%02d-%02d%02d%02d.dmp",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pep) {
    auto dir = AppDataDir() / L"dumps";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto file = dir / DumpFileName();
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = pep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashFilter);
}

} // namespace minisys
