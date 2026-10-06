#include "platform/SystemRestore.h"

#include "util/Logger.h"

#include <windows.h>
#include <srrestoreptapi.h>

#pragma comment(lib, "srclient.lib")

namespace minisys {

bool CreateRestorePoint(const std::wstring& description) {
    RESTOREPOINTINFOW rpi{};
    rpi.dwEventType = BEGIN_SYSTEM_CHANGE;
    rpi.dwRestorePtType = MODIFY_SETTINGS;
    rpi.llSequenceNumber = 0;
    wcsncpy_s(rpi.szDescription, description.c_str(),
              _TRUNCATE);

    STATEMGRSTATUS smg{};
    // Windows throttles restore-point creation (by default once per 24 h /
    // 1024 s window) — a failure here is normal and non-fatal.
    if (!SRSetRestorePointW(&rpi, &smg)) {
        MS_LOG_WARN(L"Restore point not created (Win32 %lu) — system policy "
                    L"throttle is normal", smg.nStatus ? smg.nStatus : GetLastError());
        return false;
    }
    // END_SYSTEM_CHANGE closes the point.
    RESTOREPOINTINFOW end{};
    end.dwEventType = END_SYSTEM_CHANGE;
    end.dwRestorePtType = MODIFY_SETTINGS;
    end.llSequenceNumber = smg.llSequenceNumber;
    STATEMGRSTATUS smg2{};
    SRSetRestorePointW(&end, &smg2);
    MS_LOG_INFO(L"Restore point created (seq %lld): %s",
                smg.llSequenceNumber, description.c_str());
    return true;
}

} // namespace minisys
