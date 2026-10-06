#include "core/VolumeIndex.h"

#include "util/Logger.h"
#include "core/JunkRules.h"
#include "util/StringUtils.h"
#include <string_view>

#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstring>

namespace minisys {

namespace {

// v2.2 (REVIEW P0-7 / 03-B2): the hand-declared record structs were
// misaligned with the real on-disk layout field-by-field (+8 bytes) — the
// SDK's USN_RECORD_V2/V3 from winioctl.h are used directly now.

// Parse one USN record header + payload from a raw buffer.
struct ParsedRecord {
    DWORDLONG frn = 0;
    DWORDLONG pfrn = 0;
    uint64_t  mtime = 0;
    DWORD     attrs = 0;
    DWORDLONG reason = 0;
    const WCHAR* name = nullptr;
    DWORD     nameLenBytes = 0;   // bytes
};

bool ParseUsnRecord(const BYTE* base, ParsedRecord& out) {
    auto* hdr = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(base);
    if (hdr->MajorVersion >= 3) {
        auto* r = reinterpret_cast<const USN_RECORD_V3*>(base);
        // FILE_ID_128: use the low 8 bytes as the tree key (high bytes are
        // zero on NTFS).
        DWORDLONG frn = 0, pfrn = 0;
        memcpy(&frn, r->FileReferenceNumber.Identifier, 8);
        memcpy(&pfrn, r->ParentFileReferenceNumber.Identifier, 8);
        out.frn = frn;
        out.pfrn = pfrn;
        out.mtime = static_cast<uint64_t>(r->TimeStamp.QuadPart);
        out.reason = r->Reason;
        out.attrs = r->FileAttributes;
        out.name = reinterpret_cast<const WCHAR*>(base + r->FileNameOffset);
        out.nameLenBytes = r->FileNameLength;
        return true;
    }
    auto* r = reinterpret_cast<const USN_RECORD_V2*>(base);
    out.frn = r->FileReferenceNumber;
    out.pfrn = r->ParentFileReferenceNumber;
    out.mtime = static_cast<uint64_t>(r->TimeStamp.QuadPart);
    out.reason = r->Reason;
    out.attrs = r->FileAttributes;
    out.name = reinterpret_cast<const WCHAR*>(base + r->FileNameOffset);
    out.nameLenBytes = r->FileNameLength;
    return true;
}

std::wstring VolumeHandlePath(wchar_t drive) {
    return std::wstring(L"\\\\.\\") + drive + L":";
}

std::wstring RootPath(wchar_t drive) {
    return std::wstring{ drive, L':', L'\\' };
}

// Normalized lowercase lookup key: no trailing separator except the root.
std::wstring NormKey(const std::wstring& path) {
    std::wstring s = ToLower(path);
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

} // namespace

VolumeIndex& VolumeIndex::Instance() {
    static VolumeIndex inst;
    return inst;
}

std::wstring VolumeIndex::NodeName(const Node& n, const std::wstring& names) {
    return names.substr(n.nameOff, n.nameLen);
}

void VolumeIndex::ResetForTesting() {
    valid_ = false;
    ready_ = false;
    drive_ = 0;
    volumeSerial_ = 0;
    journalId_ = 0;
    nextUsn_ = 0;
    nodes_.clear();
    names_.clear();
    byFrn_.clear();
    childrenOf_.clear();
    dirPaths_.clear();
}

void VolumeIndex::AddNodeForTesting(uint64_t frn, uint64_t parentFrn,
                                    const std::wstring& name, bool isDir,
                                    uint64_t lastWrite) {
    Node n;
    n.frn = frn;
    n.parentFrn = parentFrn;
    n.lastWrite = lastWrite;
    n.isDir = isDir;
    n.attrs = isDir ? FILE_ATTRIBUTE_DIRECTORY : 0;
    n.nameOff = static_cast<uint32_t>(names_.size());
    n.nameLen = static_cast<uint32_t>(name.size());
    names_ += name;
    nodes_.push_back(n);
}

void VolumeIndex::FinalizeForTesting(wchar_t drive) {
    drive_ = drive;
    valid_ = false;   // not volume-backed, but queries work (ready_ set below)
    Finalize();
}

void VolumeIndex::Finalize() {
    byFrn_.clear();
    childrenOf_.clear();
    dirPaths_.clear();
    frnToDirPath_.clear();
    byFrn_.reserve(nodes_.size() * 2);
    childrenOf_.reserve(nodes_.size());
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].frn == 0) continue;   // tombstoned (deleted via delta)
        byFrn_[nodes_[i].frn] = i;
    }
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        const auto& n = nodes_[i];
        if (n.frn == 0) continue;           // tombstoned
        if (n.parentFrn == n.frn) continue; // root self-reference
        childrenOf_[n.parentFrn].push_back(i);
    }

    // DFS from the root (self-parented node) building lowercase dir paths.
    std::wstring rootLower = ToLower(RootPath(drive_));
    for (const auto& n : nodes_) {
        if (n.frn == 0) continue;           // tombstoned
        if (n.isDir && n.parentFrn == n.frn) {
            dirPaths_[rootLower] = static_cast<uint32_t>(&n - nodes_.data());
            auto rootIns = dirPaths_.find(rootLower);
            if (rootIns != dirPaths_.end()) {
                frnToDirPath_[n.frn] = &(rootIns->first);   // v2.3: search
            }
            // Walk children.
            std::vector<std::pair<uint32_t, std::wstring>> stack;
            auto it = childrenOf_.find(n.frn);
            if (it != childrenOf_.end()) {
                for (uint32_t ci : it->second) {
                    stack.push_back({ci, rootLower});
                }
            }
            while (!stack.empty()) {
                auto [idx, parentPath] = stack.back();
                stack.pop_back();
                const auto& cn = nodes_[idx];
                std::wstring path = parentPath;
                if (path.empty() || (path.back() != L'\\' && path.back() != L'/')) {
                    path += L'\\';
                }
                path += NodeName(cn, names_);
                if (cn.isDir) {
                    auto ins = dirPaths_.emplace(ToLower(path), idx);   // lowercase keys
                    if (ins.second) {
                        frnToDirPath_[cn.frn] = &(ins.first->first);    // v2.3: search
                    }
                    auto cit = childrenOf_.find(cn.frn);
                    if (cit != childrenOf_.end()) {
                        for (uint32_t cci : cit->second) {
                            stack.push_back({cci, path});
                        }
                    }
                }
            }
            break;   // single root
        }
    }
    ready_ = true;
}

bool VolumeIndex::EnsureBuilt(wchar_t drive,
                              const std::function<void(const std::wstring&)>& progress,
                              const std::atomic<bool>& cancel) {
    if (valid_ && drive == drive_) {
        // Cheap revalidation: volume serial + journal identity.
        DWORD serial = 0;
        if (GetVolumeInformationW(RootPath(drive).c_str(), nullptr, 0, &serial,
                                  nullptr, nullptr, nullptr, 0)) {
            if (static_cast<uint64_t>(serial) == volumeSerial_) {
                // Same volume — try an incremental refresh (M3) before
                // considering a rebuild.
                if (RefreshFromUsn(cancel)) return true;
                valid_ = false;
                return BuildFull(drive, progress, cancel);
            }
        }
        valid_ = false;
    }
    return BuildFull(drive, progress, cancel);
}

bool VolumeIndex::BuildFull(wchar_t drive,
                            const std::function<void(const std::wstring&)>& progress,
                            const std::atomic<bool>& cancel) {
    ResetForTesting();
    drive_ = drive;

    // I/O politeness (DESIGN-v2 §5): background-priority I/O while indexing.
    HANDLE thisThread = GetCurrentThread();
    bool backgrounded =
        SetThreadPriority(thisThread, THREAD_MODE_BACKGROUND_BEGIN) != FALSE;

    HANDLE vol = CreateFileW(VolumeHandlePath(drive).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = vol != INVALID_HANDLE_VALUE;
    if (!ok) {
        MS_LOG_WARN(L"VolumeIndex: cannot open volume %c: (Win32 %lu)", drive,
                    GetLastError());
    }

    if (ok) {
        DWORD serial = 0;
        ok = GetVolumeInformationW(RootPath(drive).c_str(), nullptr, 0, &serial,
                                   nullptr, nullptr, nullptr, 0);
        if (ok) volumeSerial_ = serial;
    }

    if (ok) {
        // Journal query doubles as the "USN supported" probe.
        USN_JOURNAL_DATA_V1 jd{};
        DWORD br = 0;
        if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd,
                             sizeof(jd), &br, nullptr)) {
            MS_LOG_WARN(L"VolumeIndex: no USN journal on %c: (Win32 %lu) — "
                        L"falling back to FastWalk", drive, GetLastError());
            ok = false;
        } else {
            journalId_ = jd.UsnJournalID;
            nextUsn_ = jd.NextUsn;   // incremental-refresh cursor (M3)
        }
    }

    if (ok) {
        // v2.2 (REVIEW P0-7 / 03-B1): on the current SDK MFT_ENUM_DATA is an
        // alias of MFT_ENUM_DATA_V1 — zero-init left Min/MaxMajorVersion at
        // 0/0, an illegal version range, so FSCTL_ENUM_USN_DATA failed with
        // Win32 87 on every machine and the index never built.
        MFT_ENUM_DATA_V1 med{};
        med.MinMajorVersion = 2;   // V2 (NTFS) + V3 (ReFS/128-bit IDs)
        med.MaxMajorVersion = 3;
        std::vector<char> buf(64 * 1024);
        DWORD ret = 0;
        size_t reported = 0;
        for (;;) {
            if (cancel.load()) { ok = false; break; }
            if (!DeviceIoControl(vol, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                                 buf.data(), static_cast<DWORD>(buf.size()),
                                 &ret, nullptr)) {
                DWORD e = GetLastError();
                if (e == ERROR_HANDLE_EOF) break;         // enumeration complete
                MS_LOG_WARN(L"VolumeIndex: FSCTL_ENUM_USN_DATA failed (Win32 %lu)",
                            e);
                ok = false;
                break;
            }
            if (ret <= sizeof(DWORDLONG)) break;          // only the next-USN marker
            DWORDLONG nextFrn = *reinterpret_cast<DWORDLONG*>(buf.data());
            size_t off = sizeof(DWORDLONG);
            while (off + sizeof(DWORD) <= ret) {
                auto* hdr = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(
                    buf.data() + off);
                if (hdr->RecordLength < sizeof(*hdr) || off + hdr->RecordLength > ret) {
                    break;
                }
                ParsedRecord pr;
                if (ParseUsnRecord(reinterpret_cast<const BYTE*>(buf.data() + off),
                                   pr) &&
                    pr.nameLenBytes > 0 && (pr.nameLenBytes % sizeof(WCHAR)) == 0) {
                    Node n;
                    n.frn = pr.frn;
                    n.parentFrn = pr.pfrn;
                    n.attrs = pr.attrs;
                    n.lastWrite = pr.mtime;
                    n.isDir = (pr.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    n.nameOff = static_cast<uint32_t>(names_.size());
                    n.nameLen = pr.nameLenBytes / sizeof(WCHAR);
                    names_.append(pr.name, n.nameLen);
                    nodes_.push_back(n);
                }
                off += hdr->RecordLength;
            }
            if (reported + 100000 <= nodes_.size()) {
                reported = nodes_.size();
                if (progress) {
                    progress(L"索引: " + std::to_wstring(nodes_.size()) + L" 项");
                }
            }
            med.StartFileReferenceNumber = nextFrn;
        }
    }

    if (vol != INVALID_HANDLE_VALUE) CloseHandle(vol);

    if (ok && !nodes_.empty()) {
        Finalize();
        valid_ = true;
        MS_LOG_INFO(L"VolumeIndex: %c: indexed %zu entries (serial %llx, journal %llx)",
                    drive_, nodes_.size(), volumeSerial_, journalId_);
    } else {
        ResetForTesting();
    }

    if (backgrounded) {
        SetThreadPriority(thisThread, THREAD_MODE_BACKGROUND_END);
    }
    return valid_;
}

bool VolumeIndex::RefreshFromUsn(const std::atomic<bool>& cancel) {
    if (!valid_ || nextUsn_ == 0) return false;

    HANDLE vol = CreateFileW(VolumeHandlePath(drive_).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (vol == INVALID_HANDLE_VALUE) return false;

    bool keepIncremental = false;
    std::vector<Change> changes;

    // Journal identity must match the one we built against.
    USN_JOURNAL_DATA_V1 jd{};
    DWORD br = 0;
    if (DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd,
                        sizeof(jd), &br, nullptr) &&
        jd.UsnJournalID == journalId_) {
        READ_USN_JOURNAL_DATA_V1 ruj{};
        ruj.StartUsn = nextUsn_;
        ruj.ReasonMask = 0xFFFFFFFF;   // all reasons
        ruj.BytesToWaitFor = 0;
        ruj.Timeout = 0;
        ruj.UsnJournalID = journalId_;          // REVIEW P0-7 / 03-B3
        ruj.MinMajorVersion = 2;
        ruj.MaxMajorVersion = 3;

        std::vector<char> buf(64 * 1024);
        DWORD ret = 0;
        constexpr size_t kMaxIncrementalChanges = 20000;
        bool readOk = true;
        for (;;) {
            if (cancel.load()) { readOk = false; break; }
            if (!DeviceIoControl(vol, FSCTL_READ_USN_JOURNAL, &ruj, sizeof(ruj),
                                 buf.data(), static_cast<DWORD>(buf.size()),
                                 &ret, nullptr)) {
                DWORD e = GetLastError();
                // ERROR_HANDLE_EOF: no more records — normal completion.
                readOk = (e == ERROR_HANDLE_EOF);
                if (!readOk) {
                    MS_LOG_WARN(L"VolumeIndex: READ_USN_JOURNAL failed (Win32 %lu) "
                                L"— rebuilding", e);
                }
                break;
            }
            if (ret <= sizeof(USN)) break;   // no records left in this batch
            USN nextUsn = *reinterpret_cast<USN*>(buf.data());
            size_t off = sizeof(USN);
            while (off + sizeof(DWORD) <= ret) {
                auto* hdr = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(
                    buf.data() + off);
                if (hdr->RecordLength < sizeof(*hdr) || off + hdr->RecordLength > ret) {
                    break;
                }
                ParsedRecord pr;
                if (ParseUsnRecord(reinterpret_cast<const BYTE*>(buf.data() + off),
                                   pr) &&
                    pr.nameLenBytes > 0 && (pr.nameLenBytes % sizeof(WCHAR)) == 0) {
                    constexpr DWORDLONG kRenameNew = 0x00002000;   // USN_REASON_RENAME_NEW_NAME
                    constexpr DWORDLONG kFileDelete = 0x00000010;  // USN_REASON_FILE_DELETE
                    Change c;
                    c.frn = pr.frn;
                    c.parentFrn = pr.pfrn;
                    c.attrs = pr.attrs;
                    c.lastWrite = pr.mtime;
                    c.deleted = (pr.reason & kFileDelete) && !(pr.reason & kRenameNew);
                    c.name.assign(pr.name, pr.nameLenBytes / sizeof(WCHAR));
                    changes.push_back(std::move(c));
                }
                off += hdr->RecordLength;
            }
            nextUsn_ = nextUsn;
            ruj.StartUsn = nextUsn;
            if (changes.size() > kMaxIncrementalChanges) {
                MS_LOG_INFO(L"VolumeIndex: %zu journal changes — too many, "
                            L"rebuilding instead", changes.size());
                readOk = false;
                break;
            }
        }
        if (readOk && nextUsn_ >= static_cast<int64_t>(jd.NextUsn) - 1) {
            // Applied everything currently in the journal. With zero
            // changes the lookup rebuild (Finalize) is skipped entirely —
            // it costs seconds at the 1M-entry scale (REVIEW 03-B3).
            if (changes.empty()) {
                keepIncremental = true;
            } else if (ApplyChanges(changes)) {
                keepIncremental = true;
            }
        }
    }
    CloseHandle(vol);
    return keepIncremental;
}

bool VolumeIndex::ApplyChanges(const std::vector<Change>& changes) {
    constexpr size_t kMaxChanges = 20000;
    if (changes.size() > kMaxChanges) return false;

    for (const auto& c : changes) {
        auto it = byFrn_.find(c.frn);
        if (c.deleted) {
            if (it != byFrn_.end()) {
                nodes_[it->second].frn = 0;   // tombstone; Finalize skips it
            }
            continue;
        }
        if (it != byFrn_.end()) {
            // Update in place (name appended to the pool; pool grows only).
            Node& n = nodes_[it->second];
            n.parentFrn = c.parentFrn;
            n.attrs = c.attrs;
            n.lastWrite = c.lastWrite;
            n.isDir = (c.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            n.nameOff = static_cast<uint32_t>(names_.size());
            n.nameLen = static_cast<uint32_t>(c.name.size());
            names_ += c.name;
        } else {
            AddNodeForTesting(c.frn, c.parentFrn, c.name,
                              (c.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0,
                              c.lastWrite);
            // Keep byFrn_ in sync without a full Finalize yet.
            byFrn_[c.frn] = static_cast<uint32_t>(nodes_.size() - 1);
        }
    }
    Finalize();
    return true;
}

bool VolumeIndex::ApplyChangesForTesting(const std::vector<Change>& changes) {
    if (!ready_) return false;
    return ApplyChanges(changes);
}

const VolumeIndex::Node* VolumeIndex::FindByPath(const std::wstring& path,
                                                 bool wantDir) const {
    if (!ready_) return nullptr;   // lookups not built (synthetic index ok)
    auto key = NormKey(path);
    if (wantDir) {
        auto it = dirPaths_.find(key);
        return it == dirPaths_.end() ? nullptr : &nodes_[it->second];
    }
    // File: locate parent dir, then scan its children for a matching name.
    auto slash = key.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return nullptr;
    auto parent = dirPaths_.find(key.substr(0, slash));
    if (parent == dirPaths_.end()) return nullptr;
    const std::wstring name = key.substr(slash + 1);
    auto cit = childrenOf_.find(nodes_[parent->second].frn);
    if (cit == childrenOf_.end()) return nullptr;
    for (uint32_t ci : cit->second) {
        const auto& n = nodes_[ci];
        if (!n.isDir && IEquals(NodeName(n, names_), name)) return &n;
    }
    return nullptr;
}

bool VolumeIndex::HasDir(const std::wstring& dirPath) const {
    return FindByPath(dirPath, true) != nullptr;
}

bool VolumeIndex::HasFile(const std::wstring& filePath) const {
    return FindByPath(filePath, false) != nullptr;
}

bool VolumeIndex::TryGetEntry(const std::wstring& path, FileEntry& out) const {
    const Node* dir = FindByPath(path, true);
    if (dir) {
        out.path = path;
        out.lastWrite = dir->lastWrite;
        out.isDirectory = true;
        return true;
    }
    const Node* file = FindByPath(path, false);
    if (file) {
        out.path = path;
        out.lastWrite = file->lastWrite;
        out.isDirectory = false;
        return true;
    }
    return false;
}

void VolumeIndex::WalkSubtree(const Node& dir, const std::wstring& dirPath,
                              const std::function<void(const FileEntry&)>& cb) const {
    auto it = childrenOf_.find(dir.frn);
    if (it == childrenOf_.end()) return;
    for (uint32_t ci : it->second) {
        const auto& n = nodes_[ci];
        std::wstring path = dirPath + L"\\" + NodeName(n, names_);
        FileEntry e;
        e.path = path;
        e.lastWrite = n.lastWrite;
        e.isDirectory = n.isDir;
        cb(e);
        if (n.isDir) WalkSubtree(n, path, cb);
    }
}

bool VolumeIndex::CollectSubtree(const std::wstring& dirPath,
                                 const std::function<void(const FileEntry&)>& cb) const {
    const Node* dir = FindByPath(dirPath, true);
    if (!dir) return false;
    WalkSubtree(*dir, dirPath, cb);   // caller's casing preserved
    return true;
}

// ---- v2.3: Everything-style instant search --------------------------------

namespace {

// Case-insensitive substring search over wchar views, allocation-free for
// the ASCII fast path (the overwhelmingly common case). Falls back to a
// lowered copy for non-ASCII needles.
inline wchar_t FoldCh(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? c + 32 : c;
}
bool IFindView(std::wstring_view hay, std::wstring_view needle) {
    bool asciiNeedle = true;
    for (wchar_t c : needle) {
        if (c > 0x7F) { asciiNeedle = false; break; }
    }
    if (asciiNeedle) {
        if (needle.empty()) return true;
        if (hay.size() < needle.size()) return false;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            size_t j = 0;
            for (; j < needle.size(); ++j) {
                if (FoldCh(hay[i + j]) != FoldCh(needle[j])) break;
            }
            if (j == needle.size()) return true;
        }
        return false;
    }
    std::wstring h(hay);
    std::wstring n(needle);
    return ToLower(h).find(ToLower(n)) != std::wstring::npos;
}

bool HasWildcard(std::wstring_view s) {
    return s.find(L'*') != std::wstring_view::npos ||
           s.find(L'?') != std::wstring_view::npos;
}

std::vector<std::wstring> SplitQuery(const std::wstring& q) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : q) {
        if (c == L' ' || c == L'\t' || c == L'\u3000') {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

} // namespace

std::wstring VolumeIndex::OriginalCasePathOf(const Node& n) const {
    // Climb parent links, prepending original-case names. Stops at the root
    // (self-parented) or when an ancestor is missing (deleted mid-session).
    std::vector<std::wstring_view> parts;
    const Node* cur = &n;
    int guard = 0;
    while (cur && cur->frn != 0 && guard++ < 128) {
        parts.push_back(std::wstring_view(names_.data() + cur->nameOff,
                                          cur->nameLen));
        if (cur->parentFrn == cur->frn) break;   // root
        auto it = byFrn_.find(cur->parentFrn);
        if (it == byFrn_.end()) return {};       // broken chain — skip hit
        cur = &nodes_[it->second];
    }
    std::wstring path = RootPath(drive_);        // "C:\"
    for (auto rit = parts.rbegin(); rit != parts.rend(); ++rit) {
        if (!(path.back() == L'\\' || path.back() == L'/')) path += L'\\';
        path.append(*rit);
    }
    return path;
}

size_t VolumeIndex::Search(const std::wstring& query, bool matchPath,
                           size_t maxResults,
                           const std::function<bool(const SearchHit&)>& sink) const {
    if (!ready_) return 0;
    auto terms = SplitQuery(query);
    if (terms.empty() || maxResults == 0) return 0;

    // Pass 1: collect matching nodes (name-first matching avoids path
    // construction for the common case; a term that fails on the name is
    // retried against the full lowercased path only when matchPath is on).
    struct RawHit { uint32_t nodeIdx; std::wstring name; };
    std::vector<RawHit> hits;
    hits.reserve(maxResults < 4096 ? maxResults : 4096);

    for (uint32_t i = 0; i < nodes_.size() && hits.size() < maxResults; ++i) {
        const Node& n = nodes_[i];
        if (n.frn == 0 || n.parentFrn == n.frn) continue;   // tombstone/root
        std::wstring_view name(names_.data() + n.nameOff, n.nameLen);

        std::wstring lowerPath;                    // built at most once per node
        bool pathMatched = true;
        for (const auto& term : terms) {
            bool ok;
            if (HasWildcard(term)) {
                ok = JunkRules::MatchWildcard(term, std::wstring(name));
                if (!ok && matchPath) {
                    if (lowerPath.empty()) {
                        auto pit = frnToDirPath_.find(n.parentFrn);
                        if (pit == frnToDirPath_.end()) { pathMatched = false; break; }
                        const std::wstring& parent = *pit->second;
                        lowerPath = parent;
                        if (!lowerPath.empty() && lowerPath.back() != L'\\') {
                            lowerPath += L'\\';   // dir keys carry no trailing sep (root does)
                        }
                        lowerPath += name;
                    }
                    ok = JunkRules::MatchWildcard(term, lowerPath);
                }
            } else {
                ok = IFindView(name, term);
                if (!ok && matchPath) {
                    if (lowerPath.empty()) {
                        auto pit = frnToDirPath_.find(n.parentFrn);
                        if (pit == frnToDirPath_.end()) { pathMatched = false; break; }
                        const std::wstring& parent = *pit->second;
                        lowerPath = parent;
                        if (!lowerPath.empty() && lowerPath.back() != L'\\') {
                            lowerPath += L'\\';   // dir keys carry no trailing sep (root does)
                        }
                        lowerPath += name;
                    }
                    ok = IFindView(lowerPath, term);
                }
            }
            if (!ok) { pathMatched = false; break; }
        }
        if (!pathMatched) continue;
        hits.push_back({ i, std::wstring(name) });
    }

    // Pass 2: name-ascending (case-insensitive) like Everything's default,
    // then emit with lazily-built original-case paths.
    std::sort(hits.begin(), hits.end(), [](const RawHit& a, const RawHit& b) {
        return ToLower(a.name) < ToLower(b.name);
    });
    size_t emitted = 0;
    for (const auto& h : hits) {
        const Node& n = nodes_[h.nodeIdx];
        std::wstring path = OriginalCasePathOf(n);
        if (path.empty()) continue;
        SearchHit hit;
        hit.name = h.name;
        hit.path = std::move(path);
        hit.lastWrite = n.lastWrite;
        hit.isDirectory = n.isDir;
        ++emitted;
        if (!sink(hit) || emitted >= maxResults) break;
    }
    return emitted;
}

bool VolumeIndex::CollectChildren(const std::wstring& dirPath,
                                  const std::function<void(const FileEntry&)>& cb) const {    const Node* dir = FindByPath(dirPath, true);
    if (!dir) return false;
    auto it = childrenOf_.find(dir->frn);
    if (it == childrenOf_.end()) return true;   // empty dir is still valid
    std::wstring base = dirPath;   // caller's casing preserved
    for (uint32_t ci : it->second) {
        const auto& n = nodes_[ci];
        FileEntry e;
        e.path = base + L"\\" + NodeName(n, names_);
        e.lastWrite = n.lastWrite;
        e.isDirectory = n.isDir;
        cb(e);
    }
    return true;
}

} // namespace minisys
