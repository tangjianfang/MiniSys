#include "core/DevBuildCache.h"

#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <set>

namespace fs = std::filesystem;

namespace minisys {
namespace DevBuildCache {

namespace {

// Regenerable by the next build — deleting these can never lose user data.
bool IsSafeArtifactNameImpl(const std::wstring& n) {
    static const std::set<std::wstring> kSafe = {
        L"bin", L"obj", L".vs", L"ipch",
        L"debug", L"release", L"debug64", L"release64",
        L"x64", L"x86", L"arm", L"arm64",
        L".nuget", L"packages-build",
    };
    return kSafe.find(ToLower(n)) != kSafe.end();
}

// Reinstallable/rebuildable but with a cost (network fetch, custom config in
// the output dir) — offered unchecked by default.
bool IsCautiousArtifactNameImpl(const std::wstring& n) {
    static const std::set<std::wstring> kCautious = {
        L"build", L"out", L"output", L"target",
        L"node_modules", L"cmake-build-debug", L"cmake-build-release",
        L"build-debug", L"build-release",
    };
    return kCautious.find(ToLower(n)) != kCautious.end();
}

bool IsProjectMarkerNameImpl(const std::wstring& n) {
    static const wchar_t* kExts[] = {
        L".sln", L".slnx", L".vcxproj", L".csproj", L".fsproj", L".vbproj",
        L".vcxproj.filters",
    };
    std::wstring lower = ToLower(n);
    for (const wchar_t* ext : kExts) {
        if (lower.size() > wcslen(ext) &&
            lower.compare(lower.size() - wcslen(ext), wcslen(ext), ext) == 0) {
            return true;
        }
    }
    static const std::set<std::wstring> kNames = {
        L"cmakelists.txt", L"makefile", L"gnu makefile", L"meson.build",
        L"build.gradle", L"build.gradle.kts", L"pom.xml", L"cargo.toml",
        L"package.json", L"cmakecache.txt",
    };
    return kNames.find(lower) != kNames.end();
}

// Directory names never descended into during marker discovery (huge, slow,
// or reparse-heavy). ".git" itself is also treated as a project marker when
// seen as a direct child of the directory being inspected.
bool IsPrunedDirName(const std::wstring& n) {
    static const std::set<std::wstring> kPrune = {
        L"appdata", L"application data", L"local settings",
        L"onedrive", L"onedrivetemp", L"icloud~", L"dropbox",
        L"google drive", L"node_modules", L".git", L"$recycle.bin",
        L"minisys.quarantine", L"windows.old",
    };
    return kPrune.find(ToLower(n)) != kPrune.end();
}

uint64_t NowFiletime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t FiletimeOf(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
           static_cast<uint64_t>(ft.dwLowDateTime);
}
uint64_t FiletimeOf(const WIN32_FIND_DATAW& fd) { return FiletimeOf(fd.ftLastWriteTime); }
uint64_t FiletimeOf(const WIN32_FILE_ATTRIBUTE_DATA& fad) {
    return FiletimeOf(fad.ftLastWriteTime);
}

// One artifact directory → one quarantinable item.
void EmitArtifact(const fs::path& dir, const std::wstring& name,
                  uint64_t lastWrite, bool cautious,
                  unsigned long long sizeBytes, std::vector<ScanItem>& out) {
    ScanItem it;
    it.category        = L"Dev Build";
    it.title           = (cautious ? L"构建产物（谨慎） — " : L"构建缓存 — ") + name;
    it.title          += L"  [" + dir.parent_path().filename().wstring() + L"]";
    it.path            = dir;
    it.sizeBytes       = sizeBytes;
    it.lastWriteFiletime = lastWrite;
    it.createTime      = lastWrite;
    it.detail          = dir.wstring() + L"\n下次编译会自动重建；移入隔离区可随时还原";
    it.recommended     = !cautious;
    it.riskLevel       = cautious ? RiskLevel::Cautious : RiskLevel::Safe;
    it.strategy        = CleanStrategy::Quarantine;
    out.push_back(std::move(it));
}

struct FoundDir {
    fs::path path;
    int depth;
};

// List one directory (names only, LARGE_FETCH). Returns false on error.
bool ListDir(const fs::path& dir, std::vector<WIN32_FIND_DATAW>& out) {
    std::wstring search = dir.wstring();
    if (!search.empty() && search.back() != L'\\') search += L'\\';
    search += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(LongPath(fs::path(search)).c_str(),
                                FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        out.push_back(fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

} // namespace

bool IsSafeArtifactName(const std::wstring& n)    { return IsSafeArtifactNameImpl(n); }
bool IsCautiousArtifactName(const std::wstring& n){ return IsCautiousArtifactNameImpl(n); }
bool IsProjectMarkerName(const std::wstring& n)   { return IsProjectMarkerNameImpl(n); }

std::vector<fs::path> SearchRoots() {
    std::vector<fs::path> roots;
    auto profile = UserProfileDir();
    if (!profile.empty()) roots.push_back(profile);

    // Conventional code roots (kept only when they exist).
    auto sysRoot = SystemDriveRoot();   // "C:/"
    for (const wchar_t* name : { L"src", L"code", L"dev", L"repos",
                                 L"projects", L"work", L"github", L"golang" }) {
        fs::path p = fs::path(sysRoot) / name;
        std::error_code ec;
        if (fs::is_directory(p, ec)) roots.push_back(p);
    }
    return roots;
}

void Scan(std::vector<ScanItem>& out,
          const std::function<void(unsigned long long, unsigned long long,
                                   const std::wstring&)>& progress,
          const std::atomic<bool>& cancel) {
    constexpr int    kMaxDepth = 4;          // marker search depth
    constexpr size_t kMaxCandidates = 2000;  // hard stop for pathological trees
    constexpr size_t kMaxDirs = 150000;
    constexpr uint64_t kRecentMs = 24ULL * 3600ULL * 1000ULL;  // skip hot dirs

    uint64_t now = NowFiletime();
    std::vector<FoundDir> found;      // artifact dirs pending size computation
    size_t visited = 0;

    std::vector<std::pair<fs::path, int>> stack;
    for (auto& r : SearchRoots()) stack.push_back({ r, 0 });

    std::vector<WIN32_FIND_DATAW> entries;
    while (!stack.empty() && found.size() < kMaxCandidates && visited < kMaxDirs) {
        if (cancel.load()) return;
        auto [dir, depth] = stack.back();
        stack.pop_back();
        ++visited;
        if ((visited & 127) == 0 && progress) {
            progress(visited, 0, L"开发缓存: " + dir.wstring());
        }

        entries.clear();
        if (!ListDir(dir, entries)) continue;

        bool hasMarker = false;
        for (const auto& fd : entries) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (IEquals(fd.cFileName, L".git")) { hasMarker = true; break; }
            } else if (IsProjectMarkerName(fd.cFileName)) {
                hasMarker = true; break;
            }
        }

        for (auto& fd : entries) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const std::wstring name(fd.cFileName);

            if (hasMarker && (IsSafeArtifactName(name) || IsCautiousArtifactName(name))) {
                // Skip dirs touched recently — likely an active build.
                uint64_t ageMs = (FiletimeOf(fd) > now)
                    ? 0 : (now - FiletimeOf(fd)) / 10000ULL;
                if (ageMs >= kRecentMs) {
                    found.push_back({ dir / name, 0 });
                    if (found.size() >= kMaxCandidates) break;
                }
                continue;   // never descend into artifacts during discovery
            }
            if (depth + 1 <= kMaxDepth && !IsPrunedDirName(name)) {
                stack.push_back({ dir / name, depth + 1 });
            }
        }
    }
    if (cancel.load()) return;
    if (found.empty()) return;

    // Size the artifact dirs (the expensive part) with progress.
    size_t done = 0;
    for (const auto& f : found) {
        if (cancel.load()) return;
        ++done;
        if (progress && ((done & 7) == 0 || done == found.size())) {
            progress(done, found.size(), L"计算大小: " + f.path.filename().wstring());
        }
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(LongPath(f.path).c_str(),
                                  GetFileExInfoStandard, &fad)) continue;
        auto size = DirectorySizeParallel(f.path, 2);
        if (size == 0) continue;
        std::wstring name = f.path.filename().wstring();
        EmitArtifact(f.path, name, FiletimeOf(fad) ? FiletimeOf(fad) : now,
                     IsCautiousArtifactName(name) && !IsSafeArtifactName(name),
                     size, out);
    }
    MS_LOG_INFO(L"DevBuildCache: %zu artifact dirs in %zu visited dirs",
                found.size(), visited);
}

} // namespace DevBuildCache
} // namespace minisys
