#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace minisys {

// Which processes hold files open inside `dir` (via the Restart Manager).
// Empty vector = none found. Extracted from MoveJunctionOp (REVIEW P1-2)
// so QuarantineOp can reuse it for failure diagnosis.
std::vector<std::wstring> LockingProcesses(const std::filesystem::path& dir);

} // namespace minisys
