#include "core/LargeFileScanner.h"
#include "util/FastWalk.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"
#include "platform/Hash.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace minisys {

namespace {

std::wstring CategoryFor(const std::wstring& ext) {
    static const std::vector<std::pair<std::wstring, std::vector<std::wstring>>> table = {
        {L"Image",       {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff",
                          L".heic", L".heif", L".webp", L".raw", L".cr2", L".nef", L".arw"}},
        {L"Video",       {L".mp4", L".mkv", L".mov", L".avi", L".wmv", L".flv", L".webm",
                          L".mpg", L".mpeg", L".m4v", L".ts"}},
        {L"Audio",       {L".mp3", L".wav", L".flac", L".aac", L".ogg", L".m4a", L".wma"}},
        {L"Archive",     {L".zip", L".rar", L".7z", L".tar", L".gz", L".bz2", L".xz", L".cab", L".iso"}},
        {L"Installer",   {L".msi", L".exe", L".msix", L".appx"}},
        {L"Document",    {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx",
                          L".chm", L".epub"}},
        {L"VirtualDisk", {L".vhd", L".vhdx", L".vmdk", L".vdi", L".qcow2"}},
    };
    auto e = ToLower(ext);
    for (auto& [name, exts] : table) {
        for (auto& x : exts) if (e == x) return name;
    }
    return L"Other";
}

std::vector<fs::path> DefaultExcludes() {
    auto sd = SystemDriveRoot();
    return {
        fs::path(sd) / L"Windows",
        fs::path(sd) / L"Program Files",
        fs::path(sd) / L"Program Files (x86)",
        fs::path(sd) / L"ProgramData",
        fs::path(sd) / L"$Recycle.Bin",
        fs::path(sd) / L"System Volume Information",
        fs::path(sd) / L"$WinREAgent",
        fs::path(sd) / L"Recovery",
    };
}

std::vector<fs::path> DefaultRoots() {
    return { UserProfileDir(), fs::path(SystemDriveRoot()) };
}

struct Entry {
    fs::path           path;
    unsigned long long size;
    unsigned long long mtime;
    unsigned long long ctime; // creation FILETIME
    std::wstring       ext;
};

template <typename HashFn>
void ParallelHash(const std::vector<size_t>& indices,
                  std::vector<Entry>& entries,
                  std::vector<std::wstring>& outHashes,
                  HashFn hashFn,
                  const std::atomic<bool>& cancel) {
    outHashes.assign(indices.size(), L"");
    if (indices.empty()) return;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int n = static_cast<int>(hw);
    if (n > 8) n = 8;
    if (n < 2) n = 2;
    if ((int)indices.size() < n) n = (int)indices.size();

    std::atomic<size_t> next{0};
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (int t = 0; t < n; ++t) {
        ts.emplace_back([&] {
            for (;;) {
                if (cancel.load()) return;
                size_t i = next.fetch_add(1);
                if (i >= indices.size()) return;
                outHashes[i] = hashFn(entries[indices[i]].path);
            }
        });
    }
    for (auto& t : ts) t.join();
}

} // namespace

LargeFileScanner::LargeFileScanner() {
    cfg_.roots = DefaultRoots();
    cfg_.excludes = DefaultExcludes();
}

LargeFileScanner::LargeFileScanner(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.roots.empty())    cfg_.roots = DefaultRoots();
    if (cfg_.excludes.empty()) cfg_.excludes = DefaultExcludes();
}

void LargeFileScanner::Scan(std::vector<ScanItem>& out,
                            ProgressFn progress,
                            const std::atomic<bool>& cancel) {
    std::vector<std::wstring> excludesLower;
    excludesLower.reserve(cfg_.excludes.size());
    for (const auto& p : cfg_.excludes) {
        excludesLower.push_back(ToLower(p.wstring()));
    }

    std::vector<Entry> entries;
    entries.reserve(16384);
    std::mutex entriesMu;

    const unsigned long long minBytes = cfg_.minBytes;

    // Build lowercase extension set for fast lookup (empty = accept all)
    std::vector<std::wstring> extFilterLower;
    extFilterLower.reserve(cfg_.extFilter.size());
    for (const auto& e : cfg_.extFilter) {
        extFilterLower.push_back(ToLower(e));
    }

    std::atomic<unsigned long long> reportedAt{0};
    auto onProgress = [&](unsigned long long visited, const std::wstring& dir) {
        unsigned long long last = reportedAt.load();
        if (visited == 0 || visited - last >= 1024) {
            reportedAt.store(visited);
            if (progress) progress(visited, 0, dir);
        }
    };

    auto onFile = [&](const FastFileInfo& fi) {
        if (fi.sizeBytes < minBytes) return;
        std::wstring ext = ToLower(fi.path.extension().wstring());
        if (!extFilterLower.empty()) {
            bool found = false;
            for (const auto& fe : extFilterLower) {
                if (ext == fe) { found = true; break; }
            }
            if (!found) return;
        }
        Entry en;
        en.path  = fi.path;
        en.size  = fi.sizeBytes;
        en.mtime = fi.lastWriteFiletime;
        en.ctime = fi.createFiletime;
        en.ext   = fi.path.extension().wstring();
        std::lock_guard<std::mutex> g(entriesMu);
        entries.push_back(std::move(en));
    };

    FastWalk(cfg_.roots, excludesLower, onFile, cancel, 0, onProgress);
    if (cancel.load()) return;

    size_t topN = std::min(entries.size(), cfg_.topN);
    if (topN > 0) {
        std::partial_sort(entries.begin(), entries.begin() + topN, entries.end(),
            [](const Entry& a, const Entry& b) { return a.size > b.size; });
        for (size_t i = 0; i < topN; ++i) {
            const auto& en = entries[i];
            ScanItem it;
            it.category    = CategoryFor(en.ext);
            it.title       = en.path.filename().wstring();
            it.path        = en.path;
            it.sizeBytes   = en.size;
            it.createTime  = en.ctime;
            it.detail      = en.path.parent_path().wstring();
            it.recommended = false;
            out.push_back(std::move(it));
        }
    }

    if (cfg_.detectDuplicates && entries.size() >= 2) {
        if (progress) progress(0, 0, L"Grouping by size...");

        std::unordered_map<unsigned long long, std::vector<size_t>> bySize;
        bySize.reserve(entries.size() * 2);
        for (size_t i = 0; i < entries.size(); ++i) {
            bySize[entries[i].size].push_back(i);
        }

        std::vector<size_t> candidates;
        candidates.reserve(entries.size() / 4 + 4);
        for (auto& [sz, idxs] : bySize) {
            if (idxs.size() >= 2) candidates.insert(candidates.end(), idxs.begin(), idxs.end());
        }
        if (candidates.empty()) return;

        if (progress) {
            progress(0, 0, FormatW(L"Pre-hashing %zu candidates (head 64KB)...", candidates.size()));
        }
        std::vector<std::wstring> headHashes;
        ParallelHash(candidates, entries, headHashes,
            [](const fs::path& p) { return Sha256OfFileHead(p, 64ULL * 1024); },
            cancel);
        if (cancel.load()) return;

        std::map<std::pair<unsigned long long, std::wstring>, std::vector<size_t>> stage2;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (headHashes[i].empty()) continue;
            stage2[{entries[candidates[i]].size, headHashes[i]}].push_back(candidates[i]);
        }

        std::vector<size_t> stage2Indices;
        for (auto& [k, v] : stage2) {
            if (v.size() >= 2) stage2Indices.insert(stage2Indices.end(), v.begin(), v.end());
        }

        std::vector<std::wstring> fullHashes;
        if (!stage2Indices.empty()) {
            if (progress) {
                progress(0, 0, FormatW(L"Full-hashing %zu duplicate candidates...", stage2Indices.size()));
            }
            ParallelHash(stage2Indices, entries, fullHashes,
                [](const fs::path& p) { return Sha256OfFile(p); },
                cancel);
            if (cancel.load()) return;
        }

        std::unordered_map<std::wstring, std::vector<size_t>> finalGroups;
        for (size_t i = 0; i < stage2Indices.size(); ++i) {
            if (fullHashes[i].empty()) continue;
            finalGroups[fullHashes[i]].push_back(stage2Indices[i]);
        }

        for (auto& [hash, group] : finalGroups) {
            if (group.size() < 2) continue;
            std::sort(group.begin(), group.end(), [&](size_t a, size_t b) {
                return entries[a].mtime > entries[b].mtime;
            });
            std::wstring groupKey = L"DUP-" + hash.substr(0, 12);
            for (size_t gi = 0; gi < group.size(); ++gi) {
                const auto& en = entries[group[gi]];
                ScanItem it;
                it.category    = L"Duplicate (" + CategoryFor(en.ext) + L")";
                it.title       = en.path.filename().wstring();
                it.path        = en.path;
                it.sizeBytes   = en.size;
                it.detail      = (gi == 0 ? L"[KEEP] " : L"[DELETE] ")
                                 + en.path.parent_path().wstring()
                                 + L"  sha256:" + hash.substr(0, 16);
                it.recommended = (gi != 0);
                it.groupKey    = groupKey;
                out.push_back(std::move(it));
            }
        }
    }
}

} // namespace minisys
