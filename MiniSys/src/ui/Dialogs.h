#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>

namespace minisys {
namespace dialogs {

// REVIEW-UI P2 (L-9): the remaining MessageBox confirms (plan execution on
// non-Apps tabs, empty quarantine, folder-tree quarantine) unified as
// TaskDialogs with command links. Destructive actions always default to
// CANCEL (X-5) — Enter never executes.
inline bool ConfirmTask(HWND parent, const wchar_t* title,
                        const std::wstring& mainInstruction,
                        const std::wstring& content, bool warning,
                        const wchar_t* okText = L"确定执行") {
    TASKDIALOGCONFIG tc{};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = parent;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = title;
    tc.pszMainIcon = warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    tc.pszMainInstruction = mainInstruction.c_str();
    tc.pszContent = content.c_str();
    TASKDIALOG_BUTTON btns[] = {
        { IDOK,     okText },
        { IDCANCEL, L"取消（默认）" },
    };
    tc.pButtons = btns;
    tc.cButtons = 2;
    tc.nDefaultButton = IDCANCEL;
    int pressed = IDCANCEL;
    TaskDialogIndirect(&tc, &pressed, nullptr, nullptr);
    return pressed == IDOK;
}

} // namespace dialogs
} // namespace minisys
