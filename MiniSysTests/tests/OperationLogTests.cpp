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

} // namespace
} // namespace minisys
