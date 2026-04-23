#include "platform/Privilege.h"
#include <windows.h>

namespace minisys {

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION te{}; DWORD got = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &te, sizeof(te), &got);
    CloseHandle(token);
    return ok && te.TokenIsElevated;
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) != 0;
    if (ok) {
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        ok = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

} // namespace minisys
