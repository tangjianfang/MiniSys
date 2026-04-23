#pragma once
#include "core/Operation.h"
#include <filesystem>

namespace minisys {

// Move an installed application (or any directory) to a target location and
// leave a directory junction (or symlink in advanced mode) at the original
// path so paths remain transparent to the OS / app.
//
// Steps:
//   1. Pre-check: target volume free space >= source size * 1.1, source not
//      in use (RmGetList), source dir is not itself a reparse point, neither
//      source nor target are in protected paths.
//   2. Copy source directory tree to target using SHFileOperation (FO_COPY).
//   3. Delete source directory contents.
//   4. Create directory junction at source pointing to target.
//
// Undo: remove junction; move target back to source.
class MoveJunctionOp : public Operation {
public:
    MoveJunctionOp(std::filesystem::path source,
                   std::filesystem::path target,
                   bool useSymlink = false);

    bool Execute(std::wstring& errOut) override;
    bool Undo(std::wstring& errOut) override;
    const OpRecord& Record() const override { return rec_; }
    OpRecord& MutableRecord() override { return rec_; }

    // Pre-flight checks (no side effects). Returns empty string on OK,
    // otherwise a human-readable message describing the blocker.
    std::wstring PreflightCheck() const;

private:
    std::filesystem::path source_;
    std::filesystem::path target_;
    bool useSymlink_;
    OpRecord rec_;
};

} // namespace minisys
