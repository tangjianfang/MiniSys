#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace minisys {

// In-memory volume index built from FSCTL_ENUM_USN_DATA (DESIGN-v2 ADR-003).
// The USN record stream gives FRN / parentFRN / name / attributes / change
// time for every file on the volume — but no sizes (ADR-004: sizes come from
// a FastWalk pass over hit subtrees).
//
// Lifecycle: built once per session on first use (junk scan), revalidated
// cheaply (volume serial + USN journal ID) before every use. Any failure
// (non-NTFS, journal disabled, access denied) leaves the index empty and
// callers fall back to FastWalk — behaviour equals v1.
class VolumeIndex {
public:
    static VolumeIndex& Instance();

    struct FileEntry {
        std::wstring path;        // full path
        uint64_t     lastWrite = 0;   // FILETIME
        bool         isDirectory = false;
    };

    // Ensure an index for `drive` ('C'). Returns true when the index is
    // usable afterwards. Cheap when already valid.
    bool EnsureBuilt(wchar_t drive,
                     const std::function<void(const std::wstring&)>& progress,
                     const std::atomic<bool>& cancel);

    // REVIEW-UI 04-1: thread contract — all container access (build,
    // refresh, Search, subtree queries) happens on the SessionService
    // worker thread only. The UI thread touches NOTHING but the two
    // atomics below (status display / gating).
    bool IsValid() const { return valid_.load(std::memory_order_acquire); }
    wchar_t Drive() const { return drive_; }
    size_t EntryCount() const { return entryCount_.load(std::memory_order_acquire); }

    // Case-insensitive directory lookup by full path ("c:\windows\system32").
    bool HasDir(const std::wstring& dirPath) const;
    bool HasFile(const std::wstring& filePath) const;

    // Look up one entry (file or directory) by path; fills lastWrite/isDir.
    // Returns false when unknown to the index.
    bool TryGetEntry(const std::wstring& path, FileEntry& out) const;

    // Enumerate every entry in the subtree rooted at `dirPath`
    // (case-insensitive). Returns false when the path is unknown to the index.
    bool CollectSubtree(const std::wstring& dirPath,
                        const std::function<void(const FileEntry&)>& cb) const;

    // Enumerate direct children of a directory (for Children/Profiles rule
    // modes). Returns false when the path is unknown.
    bool CollectChildren(const std::wstring& dirPath,
                         const std::function<void(const FileEntry&)>& cb) const;

    // ---- Everything-style instant search (v2.3) ----------------------------
    // Query syntax: whitespace-separated terms, ALL must match (AND);
    // case-insensitive; '*'/'?' wildcards per term; when `matchPath` is set
    // a term may match anywhere in the full path instead of just the name.
    // v2.5 Everything-style in-query filters (matched during the scan, so
    // they never consume result slots): "folder:" / "is:folder" restricts to
    // directories, "file:" / "is:file" to files, "ext:cpp;h" to extensions.
    struct SearchHit {
        std::wstring name;      // original-case file name
        std::wstring path;      // original-case full path
        uint64_t     lastWrite = 0;
        bool         isDirectory = false;
    };
    struct SearchFilter {
        bool filesOnly = false;
        bool dirsOnly = false;
        std::vector<std::wstring> exts;   // lowercase, no leading dot
    };
    // Splits filter tokens out of `query`; the remaining free-text terms are
    // re-joined (space separated) into `freeQuery`.
    static SearchFilter ParseFilterTerms(const std::wstring& query,
                                         std::wstring& freeQuery);
    size_t Search(const std::wstring& query, bool matchPath, size_t maxResults,
                  const std::function<bool(const SearchHit&)>& sink,
                  const SearchFilter& filter = {}) const;

    // ---- test seams (unit tests build synthetic indexes) ----
    void ResetForTesting();
    void AddNodeForTesting(uint64_t frn, uint64_t parentFrn,
                           const std::wstring& name, bool isDir,
                           uint64_t lastWrite);
    void FinalizeForTesting(wchar_t drive);

    // One journal delta (test seam + RefreshFromUsn internals).
    struct Change {
        uint64_t frn = 0;
        uint64_t parentFrn = 0;
        std::wstring name;
        uint32_t attrs = 0;
        uint64_t lastWrite = 0;
        bool deleted = false;
    };
    // Apply deltas (create/rename/delete) and rebuild lookups. Returns false
    // when the delta set is too large — caller should rebuild from scratch.
    bool ApplyChangesForTesting(const std::vector<Change>& changes);

private:
    VolumeIndex() = default;

    struct Node {
        uint64_t frn = 0;
        uint64_t parentFrn = 0;
        uint32_t attrs = 0;        // FILE_ATTRIBUTE_*
        uint64_t lastWrite = 0;
        uint32_t nameOff = 0;      // offset into names_ (wchar units)
        uint32_t nameLen = 0;      // wchar units
        bool isDir = false;
    };

    bool BuildFull(wchar_t drive,
                   const std::function<void(const std::wstring&)>& progress,
                   const std::atomic<bool>& cancel);

    // v2.5 disk cache (%LOCALAPPDATA%\MiniSys\index-cache.bin): avoids the
    // ~30 s MFT walk on every start. Accuracy comes from RefreshFromUsn —
    // the journal delta is applied after loading, so a stale cache self-
    // heals (and a mismatched volume serial forces a rebuild).
    bool TryLoadCache(wchar_t drive);
    void SaveCacheIfWorthwhile() const;
    // Read the USN journal since nextUsn_ and apply the deltas. Returns false
    // when a full rebuild is the better option (journal reset / huge delta).
    bool RefreshFromUsn(const std::atomic<bool>& cancel);
    bool ApplyChanges(const std::vector<Change>& changes);
    void Finalize();               // build lookups after nodes_ are populated

    const Node* FindByPath(const std::wstring& path, bool wantDir) const;
    static std::wstring NodeName(const Node& n, const std::wstring& names);
    std::wstring PathOf(const Node& n) const;
    void WalkSubtree(const Node& dir, const std::wstring& dirPath,
                     const std::function<void(const FileEntry&)>& cb) const;

    std::atomic<bool> valid_{false};   // volume-backed (UI-readable)
    std::atomic<size_t> entryCount_{0};// nodes_.size() mirror (UI-readable)
    bool ready_ = false;   // lookups built — synthetic test indexes too (worker-only)
    wchar_t drive_ = 0;
    uint64_t volumeSerial_ = 0;
    uint64_t journalId_ = 0;
    int64_t nextUsn_ = 0;  // journal cursor for incremental refresh (M3)

    std::vector<Node> nodes_;
    std::wstring names_;                          // name pool
    std::unordered_map<uint64_t, uint32_t> byFrn_;          // FRN -> node idx
    std::unordered_map<uint64_t, std::vector<uint32_t>> childrenOf_;   // parent FRN -> child idxs
    std::unordered_map<std::wstring, uint32_t> dirPaths_;   // lowercased dir path -> node idx
    std::unordered_map<std::wstring, uint32_t> filePaths_;  // lowercased file path -> node idx
    // FRN -> lowercased dir path (pointer into dirPaths_ keys, stable) for
    // fast full-path matching during Search without per-node climbing.
    std::unordered_map<uint64_t, const std::wstring*> frnToDirPath_;

    // Search internals.
    std::wstring OriginalCasePathOf(const Node& n) const;
};

} // namespace minisys
