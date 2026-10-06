#pragma once
#include <windows.h>

namespace minisys {

// Snapshot of the main window's control handles. MainWindow builds it during
// OnCreate and hands copies to the presenters, which use it for rendering.
// Plain values only — the controls remain owned by MainWindow.
struct UiHandles {
    HWND main = nullptr;         // main window
    HWND tab = nullptr;          // tab control
    HWND list = nullptr;         // ListView (all tabs except FolderTree)
    HWND tree = nullptr;         // TreeView (FolderTree tab)
    HWND status = nullptr;       // status bar
    HWND progress = nullptr;     // progress bar (marquee while scanning)
    HWND scan = nullptr;         // "扫描" button
    HWND exec = nullptr;         // "执行选中操作" button
    HWND undo = nullptr;         // "撤销选中(历史)" button
    HWND emptyQ = nullptr;       // "清空隔离区" button (History)
    HWND open = nullptr;         // "打开所在位置" button
    HWND targetBtn = nullptr;    // "选择迁移目标盘…" (Apps)
    HWND advancedChk = nullptr;  // symlink checkbox (Apps)
    HWND info = nullptr;         // info label
    HWND lblMinSize = nullptr;   // LargeFiles settings labels
    HWND lblMinSizeUnit = nullptr;
    HWND lblFileType = nullptr;
    HWND lblDrives = nullptr;
    HWND editMinSize = nullptr;  // LargeFiles settings edits
    HWND editFileType = nullptr;
    HWND editDrives = nullptr;
    HWND btnSortSize = nullptr;  // sort buttons
    HWND btnSortTime = nullptr;
    HWND about = nullptr;        // 关于 button (always visible, right-aligned)
};

} // namespace minisys
