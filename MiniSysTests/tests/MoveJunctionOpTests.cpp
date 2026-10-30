// v2.25 (configurable scan drives): the Apps tab can now list apps on any
// fixed drive, so the migration target may coincide with an app's own drive.
// A same-volume "move + junction" frees nothing — the preflight must say so
// before anything is copied.
#include <gtest/gtest.h>

#include "core/MoveJunctionOp.h"

#include <windows.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace minisys {
namespace {

TEST(MoveJunctionPreflightTest, SameDriveTargetIsRejected) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    fs::path root = fs::path(tmp) / L"minisys-mjtest";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"app", ec);

    // Source and (not yet existing) target both live under %TEMP%.
    MoveJunctionOp op(root / L"app", root / L"moved", false);
    std::wstring msg = op.PreflightCheck();
    EXPECT_NE(msg.find(L"同在"), std::wstring::npos) << "got: " << msg.c_str();

    fs::remove_all(root, ec);
}

} // namespace
} // namespace minisys
