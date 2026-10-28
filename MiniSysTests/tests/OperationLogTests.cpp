#include <gtest/gtest.h>

#include "core/OperationLog.h"

#include <windows.h>
#include <filesystem>
#include <fstream>

namespace minisys {
namespace {

class OperationLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                (L"minisys_test_history_" +
                 std::to_wstring(::GetTickCount()) + L".jsonl");
        DeleteAll();
        OperationLog::SetHistoryPathForTesting(path_);
    }
    void TearDown() override {
        DeleteAll();
        OperationLog::SetHistoryPathForTesting({});
    }
    void DeleteAll() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(LegacyTsv(), ec);
        std::filesystem::remove(std::wstring(LegacyTsv()) + L".bak", ec);
    }
    std::filesystem::path LegacyTsv() {
        auto p = path_;
        p.replace_filename(L"history.tsv");
        return p;
    }

    std::filesystem::path path_;
};

TEST_F(OperationLogTest, AppendAndLoadRoundTrip) {
    OpRecord r;
    r.id = L"id-1";
    r.timestamp = L"2026-09-29 12:00:00";
    r.type = OpType::Quarantine;
    r.status = OpStatus::Success;
    r.sizeBytes = 123456;
    r.isReversible = true;
    r.source = L"C:\\Users\\t\\缓存";
    r.target = L"C:\\MiniSys.Quarantine\\缓存";
    r.note = L"中文 note with \"quotes\" and \\backslashes";

    OperationLog::Instance().Append(r);
    auto all = OperationLog::Instance().LoadAll();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, L"id-1");
    EXPECT_EQ(all[0].type, OpType::Quarantine);
    EXPECT_EQ(all[0].status, OpStatus::Success);
    EXPECT_EQ(all[0].sizeBytes, 123456u);
    EXPECT_TRUE(all[0].isReversible);
    EXPECT_EQ(all[0].source, L"C:\\Users\\t\\缓存");
    EXPECT_EQ(all[0].target, L"C:\\MiniSys.Quarantine\\缓存");
    EXPECT_EQ(all[0].note, L"中文 note with \"quotes\" and \\backslashes");
}

TEST_F(OperationLogTest, LoadAllReturnsNewestFirst) {
    for (int i = 0; i < 3; ++i) {
        OpRecord r;
        r.id = L"id-" + std::to_wstring(i);
        r.type = OpType::Quarantine;
        r.status = OpStatus::Success;
        OperationLog::Instance().Append(r);
    }
    auto all = OperationLog::Instance().LoadAll();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].id, L"id-2");   // newest first
    EXPECT_EQ(all[2].id, L"id-0");
}

TEST_F(OperationLogTest, UpdateStatusPersists) {
    OpRecord r;
    r.id = L"id-upd";
    r.type = OpType::Quarantine;
    r.status = OpStatus::Success;
    OperationLog::Instance().Append(r);

    OperationLog::Instance().UpdateStatus(L"id-upd", OpStatus::Reverted, L"已还原");

    auto all = OperationLog::Instance().LoadAll();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].status, OpStatus::Reverted);
    EXPECT_EQ(all[0].note, L"已还原");
    // No temp file left behind by the atomic rewrite.
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(std::wstring(path_) + L".tmp", ec));
}

TEST_F(OperationLogTest, LegacyTsvIsReadAndMigrated) {
    // v1 TSV line: id ts type status size reversible source target note
    std::wofstream f(LegacyTsv(), std::ios::trunc);
    ASSERT_TRUE(f.is_open());
    f << L"20260101-120000-0\t2026-01-01 12:00:00\tMOVE_JUNCTION\tSUCCESS\t"
         L"1000\t1\tC:\\App\tD:\\Mig\tok" << std::endl;
    f.close();

    // First Instance() call performs the one-time migration.
    auto all = OperationLog::Instance().LoadAll();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].type, OpType::MoveAndJunction);
    EXPECT_EQ(all[0].status, OpStatus::Success);
    EXPECT_EQ(all[0].source, L"C:\\App");
    EXPECT_EQ(all[0].target, L"D:\\Mig");
    EXPECT_TRUE(all[0].isReversible);

    // The original was renamed to .bak.
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(std::wstring(LegacyTsv()) + L".bak", ec));
    EXPECT_FALSE(std::filesystem::exists(LegacyTsv(), ec));
}

TEST_F(OperationLogTest, QuarantineTypeSerialization) {
    EXPECT_EQ(OperationLog::TypeToStr(OpType::Quarantine), L"QUARANTINE");
    EXPECT_EQ(OperationLog::StrToType(L"QUARANTINE"), OpType::Quarantine);
}

// v2.24 (r7 X-3/U-1): disk-truth reconcile — the healing step that ends
// the "status bar advertises gigabytes that are already free" state. Uses
// the REAL quarantine root on C: (the reconcile probes drive roots), with
// test-only subtrees; skipped silently on machines where C:\ is locked.
TEST_F(OperationLogTest, MarkReleasedWhereTargetsGoneFollowsDisk) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::path(L"C:\\MiniSys.Quarantine");
    fs::path t1 = root / L"__minisys_reconcile_1__";
    fs::path t2 = root / L"__minisys_reconcile_2__";
    fs::remove_all(t1, ec);
    fs::remove_all(t2, ec);
    fs::create_directories(t1, ec);
    fs::create_directories(t2, ec);
    if (!fs::exists(t1, ec) || ec) return;   // locked-down machine

    auto addQuarantine = [](const wchar_t* id, const fs::path& target,
                            unsigned long long size, OpStatus st) {
        OpRecord r;
        r.id = id;
        r.type = OpType::Quarantine;
        r.status = st;
        r.source = L"C:\\src";
        r.target = target.wstring();
        r.sizeBytes = size;
        r.isReversible = true;
        OperationLog::Instance().Append(r);
    };
    addQuarantine(L"rc_a", t1, 400, OpStatus::Success);
    addQuarantine(L"rc_b", t2, 200, OpStatus::Success);
    addQuarantine(L"rc_failed", t2, 999, OpStatus::Failed);   // never counts

    // Both targets exist → nothing to release.
    auto r1 = OperationLog::Instance().MarkReleasedWhereTargetsGone();
    EXPECT_EQ(r1.marked, 0u);

    // One target vanishes (user deleted it / partial empty) → exactly that
    // record is released, with its recorded bytes.
    fs::remove_all(t2, ec);
    auto r2 = OperationLog::Instance().MarkReleasedWhereTargetsGone();
    EXPECT_EQ(r2.marked, 1u);
    EXPECT_EQ(r2.freedBytes, 200u);

    bool sawReleased = false, sawLive = false, sawFailed = false;
    for (const auto& r : OperationLog::Instance().LoadAll()) {
        if (r.id == L"rc_a") sawLive = !OperationLog::IsReleased(r);
        if (r.id == L"rc_b") sawReleased = OperationLog::IsReleased(r);
        if (r.id == L"rc_failed") sawFailed = !OperationLog::IsReleased(r);
    }
    EXPECT_TRUE(sawLive);
    EXPECT_TRUE(sawReleased);
    EXPECT_TRUE(sawFailed);   // failed records are never "released"

    // Idempotent: a second reconcile finds nothing new.
    auto r3 = OperationLog::Instance().MarkReleasedWhereTargetsGone();
    EXPECT_EQ(r3.marked, 0u);
    EXPECT_EQ(r3.freedBytes, 0u);

    fs::remove_all(t1, ec);
}

} // namespace
} // namespace minisys
