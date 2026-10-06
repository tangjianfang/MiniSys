#include "platform/CrashDump.h"

#include "util/Logger.h"
#include "util/PathUtils.h"

#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <vector>

namespace minisys {

namespace {

std::wstring DumpFileName() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"minisys-%04d%02d%02d-%02d%02d%02d.dmp",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// REVIEW P2 (05-T-C7): cap the dump folder — a crash loop (e.g. a corrupted
// rules.json before the depth limit) grew it without bound.
void PruneOldDumps(const std::filesystem::path& dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> dumps;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == L".dmp") {
            dumps.push_back(e.path());
        }
    }
    constexpr size_t kMaxDumps = 5;
    if (dumps.size() <= kMaxDumps) return;
    std::sort(dumps.begin(), dumps.end(),
              [](const auto& a, const auto& b) { return a < b; });   // timestamped names
    for (size_t i = 0; i + kMaxDumps < dumps.size(); ++i) {
        std::filesystem::remove(dumps[i], ec);
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pep) {
    auto dir = AppDataDir() / L"dumps";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    PruneOldDumps(dir);
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
