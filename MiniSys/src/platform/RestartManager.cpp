#include "platform/RestartManager.h"

#include <windows.h>
#include <restartmanager.h>

#pragma comment(lib, "rstrtmgr.lib")

namespace minisys {

std::vector<std::wstring> LockingProcesses(const std::filesystem::path& dir) {
    std::vector<std::wstring> out;
    DWORD session = 0;
    WCHAR sessKey[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, sessKey) != ERROR_SUCCESS) return out;
    auto path = dir.wstring();
    LPCWSTR files[1] = { path.c_str() };
    if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) != ERROR_SUCCESS) {
        RmEndSession(session);
        return out;
    }
    UINT nProcInfoNeeded = 0, nProcInfo = 0;
    DWORD reboot = 0;
    DWORD st = RmGetList(session, &nProcInfoNeeded, &nProcInfo, nullptr, &reboot);
    if (st == ERROR_MORE_DATA && nProcInfoNeeded > 0) {
        std::vector<RM_PROCESS_INFO> infos(nProcInfoNeeded);
        nProcInfo = nProcInfoNeeded;
        if (RmGetList(session, &nProcInfoNeeded, &nProcInfo, infos.data(), &reboot) == ERROR_SUCCESS) {
            for (UINT i = 0; i < nProcInfo; ++i) {
                out.emplace_back(infos[i].strAppName);
            }
        }
    }
    RmEndSession(session);
    return out;
}

} // namespace minisys
