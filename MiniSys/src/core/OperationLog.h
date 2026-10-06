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

    std::vector<OpRecord> LoadAllLocked();   // caller holds mu_

    static std::filesystem::path JsonlPath();
    static std::filesystem::path LegacyTsvPath();
    void MigrateLegacyIfNeeded();   // v1 TSV -> JSONL, once
};

} // namespace minisys
