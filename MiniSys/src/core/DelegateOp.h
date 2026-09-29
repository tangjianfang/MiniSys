#pragma once
#include "core/Operation.h"
#include <string>

namespace minisys {

// Delegates a cleanup to an official system command (DISM component cleanup,
// powercfg /h off, cleanmgr — DESIGN-v2 ADR-008: system components are never
// hand-deleted). Captures the child's stdout/stderr for the report and maps a
// non-zero exit code to Failed.
class DelegateOp : public Operation {
public:
    // `onOutput` (optional) receives each output line — used for progress.
    DelegateOp(std::wstring title, std::wstring commandLine);

    bool Execute(std::wstring& errOut) override;
    bool Undo(std::wstring& errOut) override;
    const OpRecord& Record() const override { return rec_; }
    OpRecord& MutableRecord() override { return rec_; }

    // Split a command line into executable + arguments (quotes-aware).
    static bool SplitCommandLine(const std::wstring& cmd,
                                 std::wstring& exeOut, std::wstring& argsOut);

private:
    std::wstring title_;
    std::wstring command_;
    OpRecord rec_;
};

} // namespace minisys
