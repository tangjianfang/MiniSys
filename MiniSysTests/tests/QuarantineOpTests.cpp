#include <gtest/gtest.h>

#include "core/GuardRails.h"
#include "core/QuarantineOp.h"

#include <windows.h>

namespace minisys {
namespace {

TEST(QuarantineOpTests, QuarantineRootIsOnSourceDrive) {
    // Same-volume rename requires the quarantine root on the source volume.
    EXPECT_EQ(QuarantineOp::QuarantineRootFor(L"C:\\Users\\x\\Cache").wstring(),
              L"C:\\MiniSys.Quarantine");
    EXPECT_EQ(QuarantineOp::QuarantineRootFor(L"D:\\Games\\Big").wstring(),
              L"D:\\MiniSys.Quarantine");
    // Relative / unknown drive falls back to the system drive.
    EXPECT_FALSE(QuarantineOp::QuarantineRootFor(L"relative\\path").empty());
}

TEST(QuarantineOpTests, UniqueTargetKeepsName) {
    auto t = QuarantineOp::UniqueTargetFor(L"C:\\Users\\x\\Cache");
    EXPECT_EQ(t.parent_path().wstring(), L"C:\\MiniSys.Quarantine");
    EXPECT_EQ(t.filename().wstring(), L"Cache");
}

TEST(QuarantineOpTests, QuarantineRootIsProtectedFromPlans) {
    // The quarantine area itself must never be a valid plan target.
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\MiniSys.Quarantine\\anything"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(L"C:\\MiniSys.Quarantine"));
}

} // namespace
} // namespace minisys
