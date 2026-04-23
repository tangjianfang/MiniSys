#pragma once
#include "core/Operation.h"
#include <vector>
#include <mutex>

namespace minisys {

// Persists OpRecord entries to %LOCALAPPDATA%\MiniSys\history\history.tsv
// Format (tab-separated, one record per line, fields URL-escaped for tabs/newlines):
//   id  timestamp  type  status  size  reversible  source  target  note
class OperationLog {
public:
    static OperationLog& Instance();

    // Append a record (also writes to disk immediately).
    void Append(const OpRecord& rec);

    // Update status of an existing record by id.
    void UpdateStatus(const std::wstring& id, OpStatus status, const std::wstring& note = {});

    // Load all records (newest first).
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
};

} // namespace minisys
