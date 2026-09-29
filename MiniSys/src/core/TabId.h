#pragma once

namespace minisys {

// Identifies a main-window tab. Lives in core/ so that SessionService and
// presenters share one definition (MainWindow no longer owns it).
enum class TabId : int {
    Junk = 0,
    LargeFiles = 1,
    Apps = 2,
    FolderTree = 3,
    History = 4,
    Count
};

} // namespace minisys
