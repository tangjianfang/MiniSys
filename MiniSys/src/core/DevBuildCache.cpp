#include "core/DevBuildCache.h"

#include "core/Settings.h"
#include "util/DirSizeCache.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

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

    // v2.11 (磁盘瘦身助手): conventional code roots on EVERY fixed drive
    // (projects rarely all live on C:), kept only when they exist.
    static const wchar_t* kNames[] = { L"src", L"code", L"dev", L"repos",
                                       L"projects", L"work", L"github",
                                       L"golang" };
    std::error_code ec;
    for (const auto& root : EnumerateDrives()) {
        for (const wchar_t* name : kNames) {
            fs::path p = fs::path(root) / name;
            if (fs::is_directory(p, ec)) roots.push_back(p);
        }
    }

    // User-configured extra roots (settings.json "devCacheRoots", ';'/','-
    // separated).
    std::wstring extras = Settings::Load().devCacheRoots;
    std::wstring cur;
    auto flush = [&](std::wstring& tok) {
        if (tok.size() >= 2 && tok[1] == L':') {
            fs::path p(tok);
            if (fs::is_directory(p, ec)) roots.push_back(std::move(p));
        }
        tok.clear();
    };
    for (wchar_t ch : extras) {
        if (ch == L';' || ch == L',') flush(cur);
        else if (ch != L' ') cur += ch;
    }
    flush(cur);
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

    // ---- v2.13 incremental discovery (<=5 s warm-scan budget) --------------
    // The artifact DISCOVERY walk re-visited every directory on each scan
    // even when nothing changed. NTFS guarantees any direct-child
    // create/delete/rename bumps the directory's own mtime, so an unchanged
    // mtime proves the cached SUBTREE artifact list for that directory is
    // still exact. Cache entries are subtree-scoped and the walk recursive
    // (depth is capped anyway), so a hit answers for everything below.
    struct ArtifactRef { std::wstring path; uint64_t mtime; };
    struct DirEntryCache {
        uint64_t mtime = 0;                       // dir mtime at last walk
        std::vector<ArtifactRef> artifacts;       // subtree artifacts then
    };
    static std::mutex cacheMu;
    static std::map<std::wstring, DirEntryCache> dirCache;   // session-wide

    uint64_t now = NowFiletime();
    std::vector<FoundDir> found;      // artifact dirs pending size computation
    size_t visited = 0, cachedHits = 0;

    // Recursive subtree walk; returns the subtree's artifacts and keeps the
    // cache in sync. (Depth <= kMaxDepth keeps the stack tiny.)
    std::function<std::vector<ArtifactRef>(const fs::path&, int)> walk =
        [&](const fs::path& dir, int depth) -> std::vector<ArtifactRef> {
        if (cancel.load()) return {};
        ++visited;
        if ((visited & 127) == 0 && progress) {
            progress(visited, 0, L"开发缓存: " + dir.wstring());
        }

        // Directory mtime (child changes bump it — the cache key).
        WIN32_FILE_ATTRIBUTE_DATA dad{};
        uint64_t dirMtime = 0;
        if (GetFileAttributesExW(LongPath(dir).c_str(), GetFileExInfoStandard,
                                 &dad)) {
            dirMtime = FiletimeOf(dad);
        }

        std::wstring key = ToLower(dir.wstring());
        {
            std::lock_guard<std::mutex> g(cacheMu);
            auto it = dirCache.find(key);
            if (it != dirCache.end() && it->second.mtime == dirMtime) {
                ++cachedHits;
                return it->second.artifacts;   // no listing, no descending
            }
        }

        std::vector<WIN32_FIND_DATAW> entries;
        if (!ListDir(dir, entries)) return {};

        bool hasMarker = false;
        for (const auto& fd : entries) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (IEquals(fd.cFileName, L".git")) { hasMarker = true; break; }
            } else if (IsProjectMarkerName(fd.cFileName)) {
                hasMarker = true; break;
            }
        }

        std::vector<ArtifactRef> subtree;
        for (auto& fd : entries) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const std::wstring name(fd.cFileName);

            if (hasMarker && (IsSafeArtifactName(name) || IsCautiousArtifactName(name))) {
                // Skip dirs touched recently — likely an active build.
                uint64_t aMtime = FiletimeOf(fd);
                uint64_t ageMs = (aMtime > now) ? 0 : (now - aMtime) / 10000ULL;
                if (ageMs >= kRecentMs) {
                    subtree.push_back({ (dir / name).wstring(), aMtime });
                }
                continue;   // never descend into artifacts during discovery
            }
            if (depth + 1 <= kMaxDepth && !IsPrunedDirName(name)) {
                auto deeper = walk(dir / name, depth + 1);
                subtree.insert(subtree.end(), deeper.begin(), deeper.end());
            }
        }
        {
            std::lock_guard<std::mutex> g(cacheMu);
            dirCache[key] = DirEntryCache{ dirMtime, subtree };
        }
        return subtree;
    };

    for (auto& r : SearchRoots()) {
        if (found.size() >= kMaxCandidates || visited >= kMaxDirs) break;
        for (const auto& a : walk(r, 0)) {
            FoundDir f{ fs::path(a.path), 0 };
            found.push_back(std::move(f));
            if (found.size() >= kMaxCandidates) break;
        }
    }
    if (cancel.load()) return;
    MS_LOG_INFO(L"DevBuildCache: discovery %zu dirs (%zu cache hits), %zu artifacts",
                visited, cachedHits, found.size());
    if (found.empty()) return;

    // Size the artifact dirs — the expensive part. v2.8: parallel worker
    // pool (the sequential loop left cores idle while one subtree walked);
    // v2.7's (path, mtime) size cache still short-circuits unchanged dirs.
    struct Sized {
        fs::path path;
        uint64_t mtime = 0;
        unsigned long long size = 0;
    };
    std::vector<Sized> sized(found.size());
    {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int n = static_cast<int>(hw);
        if (n > 8) n = 8;
        if (n < 2) n = 2;
        if (static_cast<int>(found.size()) < n) n = static_cast<int>(found.size());

        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
        std::mutex pmu;   // progress serialization
        std::vector<std::thread> pool;
        pool.reserve(n);
        for (int t = 0; t < n; ++t) {
            pool.emplace_back([&] {
                for (;;) {
                    if (cancel.load()) return;
                    size_t i = next.fetch_add(1);
                    if (i >= found.size()) return;
                    const auto& f = found[i];
                    Sized s;
                    s.path = f.path;
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (GetFileAttributesExW(LongPath(f.path).c_str(),
                                             GetFileExInfoStandard, &fad)) {
                        s.mtime = FiletimeOf(fad);
                        s.size = DirSizeCache::Instance().SizeOf(f.path, s.mtime, 2);
                    }
                    sized[i] = std::move(s);
                    size_t d = done.fetch_add(1) + 1;
                    {
                        std::lock_guard<std::mutex> g(pmu);
                        if (progress && ((d & 7) == 0 || d == found.size())) {
                            progress(d, found.size(),
                                     L"计算大小: " + f.path.filename().wstring());
                        }
                    }
                }
            });
        }
        for (auto& t : pool) t.join();
    }
    if (cancel.load()) return;

    // Emit in discovery order (deterministic despite the parallel sizing).
    for (const auto& s : sized) {
        if (s.size == 0) continue;
        std::wstring name = s.path.filename().wstring();
        EmitArtifact(s.path, name, s.mtime ? s.mtime : now,
                     IsCautiousArtifactName(name) && !IsSafeArtifactName(name),
                     s.size, out);
    }
}

} // namespace DevBuildCache
} // namespace minisys
