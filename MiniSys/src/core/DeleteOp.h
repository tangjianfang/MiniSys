#pragma once
#include "core/Operation.h"
#include <filesystem>

namespace minisys {

// Delete a single file or directory to the Recycle Bin (uses SHFileOperation).
class DeleteOp : public Operation {
public:
    explicit DeleteOp(std::filesystem::path path, unsigned long long sizeBytes = 0);
    bool Execute(std::wstring& errOut) override;
    bool Undo(std::wstring& errOut) override;
    const OpRecord& Record() const override { return rec_; }
    OpRecord& MutableRecord() override { return rec_; }
private:
    std::filesystem::path path_;
    OpRecord rec_;
};

// Empty the recycle bin entirely (irreversible).
class EmptyRecycleOp : public Operation {
public:
    EmptyRecycleOp();
    bool Execute(std::wstring& errOut) override;
    bool Undo(std::wstring& errOut) override;
    const OpRecord& Record() const override { return rec_; }
    OpRecord& MutableRecord() override { return rec_; }
private:
    OpRecord rec_;
};

} // namespace minisys
