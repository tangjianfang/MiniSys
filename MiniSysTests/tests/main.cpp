// REVIEW P2 (03-B13): the stock gtest_main let every test's log output
// (e.g. ThrowingScanner's "boom") land in the user's real
// %LOCALAPPDATA%\MiniSys\logs\minisys.log, polluting diagnostics. This main
// redirects the logger to a temp file first.
#include <gtest/gtest.h>

#include "util/Logger.h"

#include <windows.h>
#include <filesystem>

int main(int argc, wchar_t** argv) {
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    auto logPath = std::filesystem::path(tmpDir) / L"minisys-tests.log";
    minisys::Logger::SetLogPathForTesting(logPath.wstring());

    // OperationLog tests manage their own per-test history files.
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
