#include <gtest/gtest.h>

#include "core/DelegateOp.h"
#include "core/OperationLog.h"

#include <windows.h>
#include <filesystem>

namespace minisys {
namespace {

class DelegateOpTest : public ::testing::Test {
protected:
    void SetUp() override {
        OperationLog::SetHistoryPathForTesting(
            std::filesystem::temp_directory_path() /
            (L"minisys_test_delegate_" + std::to_wstring(::GetTickCount()) + L".jsonl"));
    }
    void TearDown() override {
        OperationLog::SetHistoryPathForTesting({});
    }
};

TEST_F(DelegateOpTest, SplitCommandLine) {
    std::wstring exe, args;
    ASSERT_TRUE(DelegateOp::SplitCommandLine(L"Dism.exe /Online /Cleanup-Image", exe, args));
    EXPECT_EQ(exe, L"Dism.exe");
    EXPECT_EQ(args, L"/Online /Cleanup-Image");

    ASSERT_TRUE(DelegateOp::SplitCommandLine(L"\"C:\\Program Files\\x.exe\" -a", exe, args));
    EXPECT_EQ(exe, L"C:\\Program Files\\x.exe");
    EXPECT_EQ(args, L"-a");

    ASSERT_TRUE(DelegateOp::SplitCommandLine(L"powercfg", exe, args));
    EXPECT_EQ(exe, L"powercfg");
    EXPECT_TRUE(args.empty());

    EXPECT_FALSE(DelegateOp::SplitCommandLine(L"", exe, args));
}

TEST_F(DelegateOpTest, SuccessfulCommand) {
    // cmd /c exit 0 — succeeds.
    DelegateOp op(L"test", L"cmd.exe /c exit 0");
    std::wstring err;
    EXPECT_TRUE(op.Execute(err)) << err;
    EXPECT_EQ(op.Record().type, OpType::Delegate);
    EXPECT_EQ(op.Record().status, OpStatus::Success);
}

TEST_F(DelegateOpTest, FailingCommandReportsExitCode) {
    DelegateOp op(L"test", L"cmd.exe /c exit 3");
    std::wstring err;
    EXPECT_FALSE(op.Execute(err));
    EXPECT_NE(err.find(L"3"), std::wstring::npos);
    EXPECT_EQ(op.Record().status, OpStatus::Failed);
}

TEST_F(DelegateOpTest, OutputIsCaptured) {
    // Failing command with output — the message must contain the echo text.
    DelegateOp op(L"test", L"cmd.exe /c echo delegate-output-marker & exit 1");
    std::wstring err;
    EXPECT_FALSE(op.Execute(err));
    EXPECT_NE(err.find(L"delegate-output-marker"), std::wstring::npos);
}

TEST_F(DelegateOpTest, MissingExecutableFails) {
    DelegateOp op(L"test", L"C:\\definitely\\not\\an\\exe.exe --x");
    std::wstring err;
    EXPECT_FALSE(op.Execute(err));
    EXPECT_EQ(op.Record().status, OpStatus::Failed);
}

TEST_F(DelegateOpTest, UndoIsNotSupported) {
    DelegateOp op(L"test", L"cmd.exe /c exit 0");
    std::wstring err;
    EXPECT_FALSE(op.Undo(err));
    EXPECT_FALSE(op.Record().isReversible);
}

} // namespace
} // namespace minisys
