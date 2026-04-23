#pragma once

namespace minisys {

// Returns true if process is running elevated.
bool IsElevated();

// Try to enable a named privilege (e.g. SE_CREATE_SYMBOLIC_LINK_NAME).
bool EnablePrivilege(const wchar_t* name);

} // namespace minisys
