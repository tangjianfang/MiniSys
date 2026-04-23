#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace minisys {

enum class OpType {
    DeleteToRecycleBin,
    EmptyRecycleBin,
    MoveAndJunction,
    MoveFilePath,
};

enum class OpStatus { Pending, Success, Failed, Reverted, Interrupted };

struct OpRecord {
    OpType type = OpType::DeleteToRecycleBin;
    OpStatus status = OpStatus::Pending;
    std::wstring id;            // unique GUID-ish
    std::wstring timestamp;     // local time string
    std::wstring source;        // primary path
    std::wstring target;        // for moves; empty for deletes
    unsigned long long sizeBytes = 0;
    std::wstring note;          // any extra context, error message, etc.
    bool isReversible = true;
};

// Abstract operation — each instance describes one unit of work and supports Undo.
class Operation {
public:
    virtual ~Operation() = default;
    virtual bool Execute(std::wstring& errOut) = 0;
    virtual bool Undo(std::wstring& errOut) = 0;
    virtual const OpRecord& Record() const = 0;
    virtual OpRecord& MutableRecord() = 0;
};

using OperationPtr = std::unique_ptr<Operation>;

} // namespace minisys
