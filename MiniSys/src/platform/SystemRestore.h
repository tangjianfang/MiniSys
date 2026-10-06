#pragma once
#include <string>

namespace minisys {

// REVIEW P3 (08-F8): create a system restore point before migrations.
// Returns true on success; on failure fills a short reason (the caller
// degrades gracefully — never blocks the migration).
bool CreateRestorePoint(const std::wstring& description);

} // namespace minisys
