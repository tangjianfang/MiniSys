// v2.25/v2.26 (multi-drive + dangerous-confirmation):
//  * ScanItem::dangerous used to be written by the scanners and read by
//    nobody — "require extra confirmation" never happened. IsGuarded is the
//    predicate OnExecute now acts on.
//  * The Apps tab can list apps on any drive, so a migration target may share
//    a drive with some of the selected apps (DropSameDrive).
//  * Volume artefacts ($Recycle.Bin, System Volume Information, pagefile)
//    exist on every fixed drive, not only on C:.
//  * End to end: quarantine -> undo on a NON-system drive (skipped on
//    machines that have none — it never passes vacuously).
#include <gtest/gtest.h>

#include "core/GuardRails.h"
#include "core/OperationLog.h"
#include "core/PlanBuilder.h"
#include "core/QuarantineOp.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace minisys {
namespace {

TEST(PlanGuardTest, OnlyPlainQuarantineRowsFlaggedDangerousAreGuarded) {
    ScanItem repo;   // what LargeFileScanner emits for a .git pack
    repo.path = L"D:\\r\\.git\\objects\\pack\\pack-1.pack";
    repo.dangerous = true;
    EXPECT_TRUE(PlanBuilder::IsGuarded(repo));

    ScanItem plain = repo;
    plain.dangerous = false;
    EXPECT_FALSE(PlanBuilder::IsGuarded(plain));

    // Already covered by their own, stronger warnings in OnExecute.
    ScanItem system = repo;
    system.riskLevel = RiskLevel::Advanced;
    EXPECT_FALSE(PlanBuilder::IsGuarded(system));

    ScanItem delegated = repo;
    delegated.strategy = CleanStrategy::Delegate;
    EXPECT_FALSE(PlanBuilder::IsGuarded(delegated));

    ScanItem bin = repo;
    bin.path = L"$RECYCLE.BIN";
    EXPECT_FALSE(PlanBuilder::IsGuarded(bin));
}

TEST(PlanSameDriveTest, DropsRowsAlreadyOnTheTargetDrive) {
    std::vector<ScanItem> items(3);
    items[0].path = L"C:\\Apps\\A";
    items[1].path = L"D:\\Apps\\B";
    items[2].path = L"d:\\Apps\\C";               // case-insensitive
    const std::vector<size_t> all = { 0, 1, 2 };

    size_t dropped = 99;
    auto keep = PlanBuilder::DropSameDrive(items, all, L"D:\\Migrated", &dropped);
    EXPECT_EQ(keep, (std::vector<size_t>{ 0 }));
    EXPECT_EQ(dropped, 2u);

    // A target with no drive letter cannot be compared — nothing is dropped
    // (the MoveJunctionOp preflight still has the last word).
    keep = PlanBuilder::DropSameDrive(items, all, L"relative\\dir", &dropped);
    EXPECT_EQ(keep, all);
    EXPECT_EQ(dropped, 0u);

    // Out-of-range indices are kept untouched, never dereferenced.
    keep = PlanBuilder::DropSameDrive(items, { 7 }, L"D:\\Migrated", &dropped);
    EXPECT_EQ(keep, (std::vector<size_t>{ 7 }));
}

class NonSystemDriveTest : public ::testing::Test {
protected:
    void SetUp() override {
        for (const auto& d : EnumerateDrives()) {
            if (!IEquals(d, SystemDriveRoot())) { drive_ = d; break; }
        }
        if (drive_.empty()) GTEST_SKIP() << "no non-system fixed drive on this machine";

        std::error_code ec;
        dir_ = fs::path(drive_) / (L"minisys-e2e-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(dir_, ec);
        if (ec) GTEST_SKIP() << "cannot write to " << std::string(drive_.begin(), drive_.end());

        quarantineRoot_ = fs::path(drive_) / L"MiniSys.Quarantine";
        rootExisted_ = fs::exists(quarantineRoot_, ec);

        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        hist_ = fs::path(tmp) / L"minisys-tests" / L"multidrive-history.jsonl";
        fs::create_directories(hist_.parent_path(), ec);
        fs::remove(hist_, ec);
        OperationLog::SetHistoryPathForTesting(hist_);
    }
    void TearDown() override {
        OperationLog::SetHistoryPathForTesting({});
        std::error_code ec;
        if (!dir_.empty()) fs::remove_all(dir_, ec);
        if (!hist_.empty()) fs::remove(hist_, ec);
        // Leave the user's drive as we found it: drop the quarantine root only
        // if THIS test created it and it is empty again.
        if (!quarantineRoot_.empty() && !rootExisted_ && fs::is_empty(quarantineRoot_, ec)) {
            fs::remove(quarantineRoot_, ec);
        }
    }

    std::wstring drive_;            // e.g. L"D:\\"
    fs::path dir_, quarantineRoot_, hist_;
    bool rootExisted_ = true;
};

TEST_F(NonSystemDriveTest, VolumeArtefactsAreProtectedOnEveryFixedDrive) {
    EXPECT_TRUE(GuardRails::IsProtectedPath(drive_ + L"$Recycle.Bin"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(
        drive_ + L"$Recycle.Bin\\S-1-5-21-1-2-3-1001\\$R1A2B3C.zip"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(drive_ + L"System Volume Information\\x"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(drive_ + L"pagefile.sys"));
    EXPECT_TRUE(GuardRails::IsProtectedPath(drive_ + L"MiniSys.Quarantine\\anything"));

    // Ordinary data stays cleanable. "Program Files" on a secondary drive is
    // DELIBERATELY not protected (a game library there is something users
    // clean by hand) — this documents the decision, change both together.
    EXPECT_FALSE(GuardRails::IsProtectedPath(drive_ + L"Games\\bigfile.bin"));
    EXPECT_FALSE(GuardRails::IsProtectedPath(drive_ + L"Program Files\\App\\a.dll"));
    // The volume root itself is never a legal target (v2.12).
    EXPECT_TRUE(GuardRails::IsProtectedPath(drive_));
}

TEST_F(NonSystemDriveTest, QuarantineThenUndoRoundTripStaysOnTheSameVolume) {
    const fs::path payload = dir_ / L"payload.bin";
    {
        std::ofstream f(payload, std::ios::binary);
        f << std::string(8192, 'x');
    }
    ASSERT_TRUE(fs::exists(payload));

    QuarantineOp op(payload, 8192);
    std::wstring err;
    ASSERT_TRUE(op.Execute(err)) << std::string(err.begin(), err.end());
    EXPECT_FALSE(fs::exists(payload));

    const fs::path inQuarantine = op.Record().target;
    EXPECT_TRUE(fs::exists(inQuarantine));
    // Same-volume rename is the whole design: root sits on the SOURCE drive.
    EXPECT_EQ(inQuarantine.parent_path(), quarantineRoot_);
    EXPECT_TRUE(IEquals(DriveRootOf(inQuarantine), drive_));

    ASSERT_TRUE(op.Undo(err)) << std::string(err.begin(), err.end());
    EXPECT_TRUE(fs::exists(payload));
    EXPECT_FALSE(fs::exists(inQuarantine));
}

} // namespace
} // namespace minisys
