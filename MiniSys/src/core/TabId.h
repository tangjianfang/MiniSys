#pragma once

namespace minisys {

// Identifies a main-window tab. Lives in core/ so that SessionService and
// presenters share one definition (MainWindow no longer owns it).
enum class TabId : int {
    Junk = 0,
    Search = 1,        // v2.3: Everything-style instant file search
    LargeFiles = 2,
    Apps = 3,
    FolderTree = 4,
    History = 5,
    Count
};

} // namespace minisys
