#pragma once
#include "core/Operation.h"
#include <vector>
#include <mutex>
#include <filesystem>

namespace minisys {

// Persists OpRecord entries to %LOCALAPPDATA%\MiniSys\history\history.jsonl
// (one JSON object per line). Legacy history.tsv (v1, tab-separated with
// %XX-escaped fields) is read transparently and migrated once at startup
// (the old file is renamed to history.tsv.bak). UpdateStatus rewrites the
// file atomically (temp file + rename).
class OperationLog {
public:
    static OperationLog& Instance();

    // Redirect storage (unit tests only; call before first use).
    static void SetHistoryPathForTesting(const std::filesystem::path& p);

    // Append a record (also writes to disk immediately).
    void Append(const OpRecord& rec);

    // Update status of an existing record by id (atomic rewrite).
    void UpdateStatus(const std::wstring& id, OpStatus status,
                      const std::wstring& note = {});

    // REVIEW P1-7 (03-B7): bulk variant — the per-record loop re-read and
    // re-wrote the whole file N times (O(N²)); this loads once, rewrites
    // once. Records not found in `ids` keep their status/note.
    void UpdateStatusBulk(const std::vector<std::wstring>& ids,
                          OpStatus status, const std::wstring& note);

    // Load all records (newest first). Reads JSONL and legacy TSV lines.
    std::vector<OpRecord> LoadAll();

    // v2.24 (r7 X-2): a quarantine record whose files were permanently
    // deleted by 清空隔离区. The note starts with kReleasedNotePrefix and
    // the label/risk columns, undo preflight, usage text and reconcile all
    // consult THIS predicate — v2.23 had three call sites with three
    // opinions and the history rows kept advertising "可还原" after the
    // files were gone.
    static constexpr const wchar_t* kReleasedNotePrefix = L"[已释放]";
    static bool IsReleased(const OpRecord& r);

    // v2.24 (r7-05 B4): true when `canonicalLowerTarget` sits STRICTLY
    // inside an "x:\MiniSys.Quarantine\" root (≥1 real component, no ".."
    // escapes, not the root itself). QuarantineOp::UndoPaths re-checks
    // this before moving anything back — a forged record whose target IS
    // the root must not be able to carry the whole quarantine away.
    static bool IsStrictQuarantineChild(const std::wstring& canonicalLowerTarget);

    // v2.24 (r7 X-3 / U-1): disk-truth reconcile. Marks still-unreleased
    // QUARANTINE records (status Success) whose target no longer exists as
    // released, and returns how many were marked plus their recorded
    // bytes. This heals states where the quarantine root was emptied (or
    // deleted by hand) while the records kept counting — the v2.23 status
    // bar claimed 4.45 GB that was already free.
    struct ReconcileResult {
        size_t marked = 0;
        unsigned long long freedBytes = 0;
    };
    ReconcileResult MarkReleasedWhereTargetsGone();

    // v2.5 (U-5): monotonic change counter — bumped on every write. Lets
    // callers (status bar quarantine usage) cache derived data and
    // invalidate precisely instead of re-parsing per tick.
    uint64_t Generation();

    // v2.14 (review-05 T-A1): record authenticity. New records are signed
    // on Append with the admin-ACLed machine secret; VerifySignature fails
    // for unsigned (legacy) or tampered records — undo refuses them.
    static bool VerifySignature(const OpRecord& rec);
    // Admin-confirmed adoption: re-sign unsigned legacy records whose
    // quarantine target lies under a MiniSys.Quarantine root.
    size_t SignUnsignedLegacy();

    // Generate a unique record id (timestamp + counter).
    static std::wstring NewId();

    // Helpers for converting type/status to/from strings.
    static std::wstring  TypeToStr(OpType t);
    static OpType        StrToType(const std::wstring& s);
    static std::wstring  StatusToStr(OpStatus s);
    static OpStatus      StrToStatus(const std::wstring& s);

private:
    OperationLog() = default;
    std::mutex mu_;
    uint64_t gen_ = 1;   // bumped on every write (cache invalidation, v2.5)

    std::vector<OpRecord> LoadAllLocked();   // caller holds mu_

    static std::filesystem::path JsonlPath();
    static std::filesystem::path LegacyTsvPath();
    void MigrateLegacyIfNeeded();   // v1 TSV -> JSONL, once
};

} // namespace minisys
