#pragma once

namespace minisys {

// Installs a process-wide unhandled-exception filter that writes a minidump
// to %LOCALAPPDATA%\MiniSys\dumps\. Call once at startup (M4).
void InstallCrashHandler();

} // namespace minisys
