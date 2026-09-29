#pragma once
#include "core/Operation.h"
#include <filesystem>

namespace minisys {

// Moves a file or directory into the per-drive quarantine area
// (<drive>:\MiniSys.Quarantine) using a same-volume rename — instant,
// reversible, and the default execution strategy (DESIGN-v2 ADR-005).
//
// The OpRecord doubles as the quarantine manifest:
//   source = original path, target = quarantine path, status = lifecycle.
// "Empty quarantine" (the real space release) is a separate user action.
class QuarantineOp : public Operation {
public:
    QuarantineOp(std::filesystem::path source, unsigned long long sizeBytes);

    bool Execute(std::wstring& errOut) override;
    bool Undo(std::wstring& errOut) override;
    const OpRecord& Record() const override { return rec_; }
    OpRecord& MutableRecord() override { return rec_; }

    // Quarantine root for the volume containing `source`
    // (e.g. "C:\MiniSys.Quarantine").
    static std::filesystem::path QuarantineRootFor(const std::filesystem::path& source);

    // Unique target path under the root (name, name-2, name-3, ...).
    static std::filesystem::path UniqueTargetFor(const std::filesystem::path& source);

    // Undo a quarantined record from history. Restores target -> source;
    // on conflict restores to "<source>.restored".
    static bool UndoPaths(const std::wstring& recordId,
                          const std::filesystem::path& source,
                          const std::filesystem::path& quarantinePath,
                          std::wstring& errOut);

private:
    std::filesystem::path source_;
    OpRecord rec_;
};

} // namespace minisys
