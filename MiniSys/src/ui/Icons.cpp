#include "ui/Icons.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace minisys {
namespace icons {

bool SetStockButtonIcon(HWND button, int stockIconId) {
    if (!button) return false;

    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);
    if (FAILED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(stockIconId),
                                  SHGSI_ICON | SHGSI_SMALLICON, &sii)) ||
        !sii.hIcon) {
        return false;
    }

    HIMAGELIST himl = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
    if (!himl) {
        DestroyIcon(sii.hIcon);
        return false;
    }
    if (ImageList_AddIcon(himl, sii.hIcon) < 0) {
        DestroyIcon(sii.hIcon);
        ImageList_Destroy(himl);
        return false;
    }
    DestroyIcon(sii.hIcon);   // the imagelist holds its own copy

    BUTTON_IMAGELIST bil{};
    bil.himl = himl;
    bil.margin = {2, 0, 3, 0};
    bil.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    // The button borrows the imagelist; it must stay alive for the control's
    // lifetime (freed implicitly at process exit).
    if (SendMessageW(button, BCM_SETIMAGELIST, 0,
                     reinterpret_cast<LPARAM>(&bil)) == 0) {
        ImageList_Destroy(himl);
        return false;
    }
    return true;
}

} // namespace icons
} // namespace minisys
