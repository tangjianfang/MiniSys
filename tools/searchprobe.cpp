// Elevated search probe (v2.16b): dissects the REAL volume index.
// Build: cl /std:c++17 /EHsc /I MiniSys\src tools\searchprobe.cpp
//        MiniSys\src\core\VolumeIndex.cpp MiniSys\src\core\JunkRules.cpp
//        MiniSys\src\core\GuardRails.cpp ... (link via the test list minus gtest)
// Runs elevated (rotate-style), writes findings to %TEMP%\minisys-search-probe.txt
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "core/VolumeIndex.h"
#include "util/StringUtils.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace minisys;

int wmain() {
    std::ofstream out(getenv("TEMP") ? (std::string(getenv("TEMP")) + "\\minisys-search-probe.txt")
                                     : "C:\\Windows\\Temp\\minisys-search-probe.txt",
                      std::ios::binary);
    auto& vi = VolumeIndex::For(L'C');
    // Load the on-disk cache if possible, else do a full build.
    // (TryLoadCache is private; go through the public path with no progress.)
    std::atomic<bool> cancel{false};
    vi.EnsureBuilt(L'C', nullptr, cancel);

    out << "valid=" << vi.IsValid() << " entries=" << vi.EntryCount() << "\n";
    size_t n = vi.Search(L"pdb", false, 20, [&](const VolumeIndex::SearchHit& h) {
        out << "hit: " << h.path << "\n";
        return true;
    });
    out << "Search(pdb) returned " << n << "\n";

    // Control query: single letter that must match millions.
    n = vi.Search(L"e", false, 5, [&](const VolumeIndex::SearchHit& h) {
        out << "hit-e: " << h.path << "\n";
        return true;
    });
    out << "Search(e) returned " << n << "\n";
    return 0;
}
