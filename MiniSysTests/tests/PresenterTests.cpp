// v2.24 (r7 06-architecture T0): the FIRST headless UI-layer tests.
// Presenters.cpp is compiled into the test binary; UiHandles' members all
// default to nullptr, and the Win32 SendMessage-based macros are no-ops
// against NULL handles — so HistoryPresenter::Refresh / label logic /
// SupplyDispInfo / undo classification are directly testable without a
// window. The v2.23 "清空隔离区后列表仍显示可还原" bug lived exactly in
// these functions with zero coverage.
#include <gtest/gtest.h>

#include "core/OperationLog.h"
#include "core/SessionService.h"
#include "ui/Presenters.h"

#include <windows.h>
#include <commctrl.h>
#include <filesystem>

namespace minisys {
namespace {

class HistoryPresenterTest : public ::testing::Test {
protected:
    void SetUp() override {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        hist_ = std::filesystem::path(tmp) / L"minisys-tests" /
                L"presenter-history.jsonl";
        std::error_code ec;
        std::filesystem::create_directories(hist_.parent_path(), ec);
        std::filesystem::remove(hist_, ec);
        OperationLog::SetHistoryPathForTesting(hist_);
        svc_ = &SessionService::Instance();
    }
    void TearDown() override {
        OperationLog::SetHistoryPathForTesting({});
        std::error_code ec;
        std::filesystem::remove(hist_, ec);
    }

    // A SIGNED released quarantine record (via Append + UpdateStatusBulk,
    // mirroring RunEmptyQuarantine's marking) — or leave released=false.
    OpRecord AddQuarantineRecord(unsigned long long size, bool released) {
        OpRecord r;
        r.id = OperationLog::NewId();
        r.type = OpType::Quarantine;
        r.status = OpStatus::Success;
        r.source = L"C:\\Users\\t\\junk";
        r.target = L"C:\\MiniSys.Quarantine\\junk";
        r.sizeBytes = size;
        r.isReversible = true;
        OperationLog::Instance().Append(r);
        if (released) {
            OperationLog::Instance().UpdateStatus(
                r.id, OpStatus::Success,
                std::wstring(OperationLog::kReleasedNotePrefix) +
                    L" 隔离区已清空，文件已永久删除");
        }
        return r;
    }

    std::filesystem::path hist_;
    SessionService* svc_ = nullptr;
};

TEST_F(HistoryPresenterTest, ReleasedRowsLabelledReleasedNotRestorable) {
    // r7 X-2: the state column said "已隔离 · 成功" and the risk column
    // said "可还原" for rows whose files were permanently deleted.
    // Judge the SIGNED copies loaded back (see ClassifyUndo test).
    auto released = AddQuarantineRecord(100, /*released=*/true);
    auto live = AddQuarantineRecord(200, /*released=*/false);

    HistoryPresenter p(UiHandles{}, *svc_);
    p.Refresh();   // rows_ now holds both records (NULL HWNDs are no-ops)

    auto loadById = [](const std::wstring& id) -> OpRecord {
        for (const auto& r : OperationLog::Instance().LoadAll()) {
            if (r.id == id) return r;
        }
        return {};
    };
    OpRecord releasedRec = loadById(released.id);
    OpRecord liveRec = loadById(live.id);
    if (releasedRec.sig.empty()) return;   // no machine secret in this env

    EXPECT_EQ(HistoryPresenter::HistoryTypeLabel(releasedRec),
              L"已隔离 · 已释放");
    EXPECT_EQ(HistoryPresenter::HistoryRiskLabel(releasedRec), L"已释放");
    EXPECT_EQ(HistoryPresenter::HistoryTypeLabel(liveRec), L"已隔离 · 成功");
    EXPECT_EQ(HistoryPresenter::HistoryRiskLabel(liveRec), L"可还原");
}

TEST_F(HistoryPresenterTest, RowIsDimmedOnlyForReleasedRows) {
    AddQuarantineRecord(100, /*released=*/true);
    AddQuarantineRecord(200, /*released=*/false);
    HistoryPresenter p(UiHandles{}, *svc_);
    p.Refresh();
    // rows_ is newest-first → exactly one of the two rows is dimmed.
    bool dim0 = p.RowIsDimmed(0), dim1 = p.RowIsDimmed(1);
    EXPECT_TRUE(dim0 ^ dim1);
    EXPECT_FALSE(p.RowIsDimmed(99));   // out of range is never dimmed
}

TEST_F(HistoryPresenterTest, EmptyQuarantineAuditRowLabels) {
    OpRecord audit;
    audit.type = OpType::EmptyQuarantine;
    audit.status = OpStatus::Success;
    audit.isReversible = false;
    audit.id = OperationLog::NewId();
    audit.source = L"隔离区（各盘 MiniSys.Quarantine）";
    audit.sizeBytes = 123;
    OperationLog::Instance().Append(audit);

    EXPECT_EQ(HistoryPresenter::HistoryTypeLabel(audit), L"清空隔离区 · 成功");

    // Load the SIGNED copy back (Append signs when a machine secret
    // exists) — a signed audit row must never claim 可还原.
    bool sawSigned = false;
    for (const auto& r : OperationLog::Instance().LoadAll()) {
        if (r.id == audit.id && !r.sig.empty()) {
            sawSigned = true;
            EXPECT_EQ(HistoryPresenter::HistoryRiskLabel(r), L"不可逆");
        }
    }
    if (!sawSigned) {
        // No machine secret in this environment — the sig-less record at
        // least must not read 可还原 either.
        EXPECT_NE(HistoryPresenter::HistoryRiskLabel(audit), L"可还原");
    }
}

TEST_F(HistoryPresenterTest, ClassifyUndoRefusesReleasedRecords) {
    // r7 X-2b: v2.23's pre-flight waved released rows through into a
    // guaranteed "隔离区中找不到该项目" failure. Classify on the SIGNED
    // copies loaded back from the (redirected) history file — the local
    // pre-Append copies carry no signature and would all read 未签名.
    auto released = AddQuarantineRecord(100, /*released=*/true);
    auto live = AddQuarantineRecord(200, /*released=*/false);
    OpRecord recycle;   // real recycle rows keep isReversible=true
    recycle.id = OperationLog::NewId();
    recycle.type = OpType::DeleteToRecycleBin;
    recycle.status = OpStatus::Success;
    recycle.isReversible = true;
    OperationLog::Instance().Append(recycle);

    auto loadById = [](const std::wstring& id) -> OpRecord {
        for (const auto& r : OperationLog::Instance().LoadAll()) {
            if (r.id == id) return r;
        }
        return {};
    };
    OpRecord releasedRec = loadById(released.id);
    OpRecord liveRec = loadById(live.id);
    OpRecord recycleRec = loadById(recycle.id);
    if (releasedRec.sig.empty()) return;   // no machine secret here

    auto c = HistoryPresenter::ClassifyUndo({ releasedRec, liveRec, recycleRec });
    EXPECT_EQ(c.eligible, 1u);      // the live quarantine record
    EXPECT_EQ(c.released, 1u);
    EXPECT_EQ(c.deleteType, 1u);
    EXPECT_EQ(c.unsignedN, 0u);

    // Only-released selection → zero eligible (blocked with the dedicated
    // hint in UndoSelected).
    auto onlyReleased = HistoryPresenter::ClassifyUndo({ releasedRec });
    EXPECT_EQ(onlyReleased.eligible, 0u);
    EXPECT_EQ(onlyReleased.released, 1u);
}

TEST_F(HistoryPresenterTest, SupplyDispInfoServesCellsAndRejectsBounds) {
    AddQuarantineRecord(100, /*released=*/true);
    HistoryPresenter p(UiHandles{}, *svc_);
    p.Refresh();

    NMLVDISPINFOW di{};
    wchar_t buf[64];
    di.item.mask = LVIF_TEXT;
    di.item.iItem = 0;
    di.item.iSubItem = 0;
    di.item.pszText = buf;
    di.item.cchTextMax = 64;
    EXPECT_TRUE(p.SupplyDispInfo(&di));
    EXPECT_NE(buf[0], L'\0');        // a real label came back

    di.item.iItem = 99;              // out of range — must refuse cleanly
    EXPECT_FALSE(p.SupplyDispInfo(&di));
}

TEST(OperationLogStatic, IsReleasedPrefixContract) {
    OpRecord r;
    r.note = L"";
    EXPECT_FALSE(OperationLog::IsReleased(r));
    r.note = L"[已释放] 隔离区已清空，文件已永久删除";
    EXPECT_TRUE(OperationLog::IsReleased(r));
    r.note = L"xx[已释放]";          // prefix must be at position 0
    EXPECT_FALSE(OperationLog::IsReleased(r));
}

TEST(OperationLogStatic, StrictQuarantineChildShapes) {
    // r7-05 B4: root-exact and dotdot-escape shapes are NOT children.
    EXPECT_TRUE (OperationLog::IsStrictQuarantineChild(L"c:\\minisys.quarantine\\x"));
    EXPECT_TRUE (OperationLog::IsStrictQuarantineChild(
        L"c:\\minisys.quarantine\\a\\b\\c.txt"));
    EXPECT_FALSE(OperationLog::IsStrictQuarantineChild(L"c:\\minisys.quarantine"));
    EXPECT_FALSE(OperationLog::IsStrictQuarantineChild(L"c:\\minisys.quarantine\\"));
    EXPECT_FALSE(OperationLog::IsStrictQuarantineChild(L"c:\\minisys.quarantine\\..\\esc"));
    EXPECT_FALSE(OperationLog::IsStrictQuarantineChild(L"c:\\minisys.quarantine-evil\\x"));
    EXPECT_FALSE(OperationLog::IsStrictQuarantineChild(L"c:\\windows\\system32"));
    EXPECT_FALSE(OperationLog::IsStrictQuarantineChild(L""));
}

TEST(OperationLogStatic, EmptyQuarantineTypeRoundTrip) {
    EXPECT_EQ(OperationLog::TypeToStr(OpType::EmptyQuarantine),
              L"EMPTY_QUARANTINE");
    EXPECT_EQ(OperationLog::StrToType(L"EMPTY_QUARANTINE"),
              OpType::EmptyQuarantine);
    // Old files without the new type keep parsing (default arm).
    EXPECT_EQ(OperationLog::StrToType(L"QUARANTINE"), OpType::Quarantine);
}

} // namespace
} // namespace minisys
