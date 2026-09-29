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

    bool IsValid() const { return valid_; }
    wchar_t Drive() const { return drive_; }
    size_t EntryCount() const { return nodes_.size(); }

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

    bool valid_ = false;   // volume-backed (EnsureBuilt succeeded)
    bool ready_ = false;   // lookups built — synthetic test indexes too
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
};

} // namespace minisys
