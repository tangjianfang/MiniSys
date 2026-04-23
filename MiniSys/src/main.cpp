#include "MainWindow.h"
#include "platform/Privilege.h"
#include "util/Logger.h"

#include <windows.h>
#include <objbase.h>

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetProcessDPIAware();
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
