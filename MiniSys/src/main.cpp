#include "MainWindow.h"
#include "platform/CrashDump.h"
#include "platform/Privilege.h"
#include "util/Logger.h"

#include <windows.h>
#include <objbase.h>

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    // REVIEW P2 (08-F13): single instance — two elevated copies racing the
    // quarantine area and the history file is a real foot-gun.
    // v2.16c (user directive): launching with `-takeover` closes the OLD
    // instance (gracefully — settings save, worker joins) and this one
    // takes over, so every build rotation replaces the running copy.
    bool takeover = lpCmdLine && wcsstr(lpCmdLine, L"-takeover") != nullptr;
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MiniSys.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"MiniSysMainWnd", nullptr);
        if (takeover && existing) {
            // Ask the old instance to close itself; both are elevated, so
            // the message goes through. Give it up to 10 s (a walk-index
            // save tail can be slow), then fall through to just surfacing.
            RegisterWindowMessageW(L"MiniSys.Takeover");
            for (int i = 0; i < 50; ++i) {
                PostMessageW(existing, WM_CLOSE, 0, 0);
                Sleep(200);
                if (WaitForSingleObject(single, 0) == WAIT_OBJECT_0) break;
                existing = FindWindowW(L"MiniSysMainWnd", nullptr);
                if (!existing) {
                    // Process gone — retry the mutex acquisition once the
                    // OS releases it (abandon detection is immediate on
                    // Local\ handles).
                    for (int k = 0; k < 25; ++k) {
                        CloseHandle(single);
                        single = CreateMutexW(nullptr, TRUE,
                                              L"Local\\MiniSys.SingleInstance");
                        if (single && GetLastError() != ERROR_ALREADY_EXISTS) {
                            goto have_mutex;
                        }
                        Sleep(200);
                    }
                    break;
                }
            }
            if (WaitForSingleObject(single, 0) != WAIT_OBJECT_0) {
                // Old instance refused/died holding the mutex — surface it.
                if (existing) {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                }
                return 0;
            }
        } else {
            if (existing) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            return 0;
        }
    }
have_mutex:;

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
