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
    // REVIEW-UI P2 (L-12): request the 32 px stock icon and scale it to the
    // DPI-aware small-icon metric — the old SHGSI_SMALLICON path stayed
    // 16 px at 150% next to 45 px buttons.
    if (FAILED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(stockIconId),
                                  SHGSI_ICON, &sii)) ||
        !sii.hIcon) {
        return false;
    }
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx < 16 || cy < 16) { cx = 16; cy = 16; }
    HICON icon = static_cast<HICON>(CopyImage(sii.hIcon, IMAGE_ICON, cx, cy,
                                              LR_COPYDELETEORG));
    // review-04 R-3: LR_COPYDELETEORG already destroyed the source on
    // success — destroying it again here was a double free (harmless by
    // luck, wrong by contract). On failure the source stays alive as the
    // fallback.
    if (!icon) icon = sii.hIcon;

    HIMAGELIST himl = ImageList_Create(cx, cy, ILC_COLOR32, 1, 1);
    if (!himl) {
        DestroyIcon(icon);
        return false;
    }
    if (ImageList_AddIcon(himl, icon) < 0) {
        DestroyIcon(icon);
        ImageList_Destroy(himl);
        return false;
    }
    DestroyIcon(icon);   // the imagelist holds its own copy

    BUTTON_IMAGELIST bil{};
    bil.himl = himl;
    bil.margin = {2, 0, 3, 0};
    bil.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
    // The button borrows the imagelist; the PREVIOUS one (set by an earlier
    // call — the scan button flips between find/stop icons, v2.5 L-22) is
    // destroyed here instead of leaking one per task.
    BUTTON_IMAGELIST old{};
    if (SendMessageW(button, BCM_GETIMAGELIST, 0,
                    reinterpret_cast<LPARAM>(&old)) && old.himl) {
        ImageList_Destroy(old.himl);
    }
    if (SendMessageW(button, BCM_SETIMAGELIST, 0,
                     reinterpret_cast<LPARAM>(&bil)) == 0) {
        ImageList_Destroy(himl);
        return false;
    }
    return true;
}

} // namespace icons
} // namespace minisys
