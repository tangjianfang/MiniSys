#include "MainWindow.h"
#include "platform/CrashDump.h"
#include "platform/Privilege.h"
#include "util/Logger.h"

#include <windows.h>
#include <objbase.h>

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // REVIEW P2 (08-F13): single instance — two elevated copies racing the
    // quarantine area and the history file is a real foot-gun.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MiniSys.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"MiniSysMainWnd", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetProcessDPIAware();
    minisys::InstallCrashHandler();
    minisys::Logger::Instance(); // ensure log file open
    MS_LOG_INFO(L"MiniSys starting; elevated=%d", minisys::IsElevated());
    minisys::EnablePrivilege(SE_CREATE_SYMBOLIC_LINK_NAME);
    minisys::EnablePrivilege(SE_BACKUP_NAME);
    minisys::EnablePrivilege(SE_RESTORE_NAME);

    minisys::MainWindow win;
    if (!win.Create(hInst, nCmdShow)) {
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 1;
    }
    int rc = win.RunMessageLoop();
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return rc;
}
