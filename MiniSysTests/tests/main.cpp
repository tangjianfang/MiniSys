// REVIEW P2 (03-B13): the stock gtest_main let every test's log output
// (e.g. ThrowingScanner's "boom") land in the user's real
// %LOCALAPPDATA%\MiniSys\logs\minisys.log, polluting diagnostics. This main
// redirects the logger to a temp file first.
// v2.13c visual-review fix (V-R5): SessionService's results-cache writes
// had the same flaw — scan tests stored REAL entries into the user's
// %LOCALAPPDATA%\MiniSys\cache\results-*.json, which session-restore then
// displayed as ghost rows ("j / 123 B") in the app. Redirect the cache
// base directory for the whole test process.
#include <gtest/gtest.h>

#include "core/SessionService.h"
#include "util/Logger.h"

#include <windows.h>
#include <filesystem>

int main(int argc, wchar_t** argv) {
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    auto logPath = std::filesystem::path(tmpDir) / L"minisys-tests.log";
    minisys::Logger::SetLogPathForTesting(logPath.wstring());
    auto cacheBase = std::filesystem::path(tmpDir) / L"minisys-tests-cache";
    minisys::SessionService::SetCacheDirForTesting(cacheBase);

    // OperationLog tests manage their own per-test history files.
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
