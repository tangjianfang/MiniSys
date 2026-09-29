#include "core/VolumeIndex.h"

#include "util/Logger.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstring>

namespace minisys {

namespace {

// USN record layouts (ntifs.h is WDK-only; re-declared like MS_REPARSE_DATA_BUFFER).
#pragma pack(push, 8)
struct MSN_RECORD_V2 {
    DWORD      RecordLength;
    DWORD      MajorVersion;
    DWORD      MinorVersion;
    DWORDLONG  FileReferenceNumber;
    DWORDLONG  ParentFileReferenceNumber;
    USN        Usn;
    LARGE_INTEGER TimeChanged;
    LARGE_INTEGER Reason;
    DWORD      SourceInfo;
    DWORD      FileAttributes;
    DWORD      FileNameLength;    // bytes
    WCHAR      FileName[1];
};
struct MSN_RECORD_V3 {
    DWORD      RecordLength;
    DWORD      MajorVersion;
    DWORD      MinorVersion;
    DWORDLONG  FileReferenceNumber;
    DWORDLONG  ParentFileReferenceNumber;
    USN        Usn;
    LARGE_INTEGER TimeChanged;
    LARGE_INTEGER Reason;
    DWORD      SourceInfo;
    DWORD      FileAttributes;
    DWORD      FileNameLength;    // bytes
    DWORD      Reserved;
    WCHAR      FileName[1];
};
#pragma pack(pop)

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
                    dirPaths_[ToLower(path)] = idx;   // map keys are lowercase
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
        MFT_ENUM_DATA med{};
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
                auto* rec = reinterpret_cast<MSN_RECORD_V2*>(buf.data() + off);
                if (rec->RecordLength == 0) break;
                DWORD attrs = 0;
                DWORDLONG frn = 0, pfrn = 0;
                uint64_t mtime = 0;
                const WCHAR* name = nullptr;
                DWORD nameLen = 0;
                if (rec->MajorVersion >= 3) {
                    auto* r3 = reinterpret_cast<MSN_RECORD_V3*>(buf.data() + off);
                    attrs = r3->FileAttributes;
                    frn = r3->FileReferenceNumber;
                    pfrn = r3->ParentFileReferenceNumber;
                    mtime = static_cast<uint64_t>(r3->TimeChanged.QuadPart);
                    name = r3->FileName;
                    nameLen = r3->FileNameLength;
                } else {
                    attrs = rec->FileAttributes;
                    frn = rec->FileReferenceNumber;
                    pfrn = rec->ParentFileReferenceNumber;
                    mtime = static_cast<uint64_t>(rec->TimeChanged.QuadPart);
                    name = rec->FileName;
                    nameLen = rec->FileNameLength;
                }
                if (nameLen > 0 && (nameLen % sizeof(WCHAR)) == 0) {
                    Node n;
                    n.frn = frn;
                    n.parentFrn = pfrn;
                    n.attrs = attrs;
                    n.lastWrite = mtime;
                    n.isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    n.nameOff = static_cast<uint32_t>(names_.size());
                    n.nameLen = nameLen / sizeof(WCHAR);
                    names_.append(name, n.nameLen);
                    nodes_.push_back(n);
                }
                off += rec->RecordLength;
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
        READ_USN_JOURNAL_DATA ruj{};
        ruj.StartUsn = nextUsn_;
        ruj.ReasonMask = 0xFFFFFFFF;   // all reasons
        ruj.BytesToWaitFor = 0;
        ruj.Timeout = 0;

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
                auto* rec = reinterpret_cast<MSN_RECORD_V2*>(buf.data() + off);
                if (rec->RecordLength == 0) break;
                DWORD attrs = 0;
                DWORDLONG frn = 0, pfrn = 0;
                uint64_t mtime = 0;
                DWORDLONG reason = 0;
                const WCHAR* name = nullptr;
                DWORD nameLen = 0;
                if (rec->MajorVersion >= 3) {
                    auto* r3 = reinterpret_cast<MSN_RECORD_V3*>(buf.data() + off);
                    attrs = r3->FileAttributes;
                    frn = r3->FileReferenceNumber;
                    pfrn = r3->ParentFileReferenceNumber;
                    mtime = static_cast<uint64_t>(r3->TimeChanged.QuadPart);
                    reason = r3->Reason.QuadPart;
                    name = r3->FileName;
                    nameLen = r3->FileNameLength;
                } else {
                    attrs = rec->FileAttributes;
                    frn = rec->FileReferenceNumber;
                    pfrn = rec->ParentFileReferenceNumber;
                    mtime = static_cast<uint64_t>(rec->TimeChanged.QuadPart);
                    reason = rec->Reason.QuadPart;
                    name = rec->FileName;
                    nameLen = rec->FileNameLength;
                }
                constexpr DWORDLONG kRenameNew = 0x00002000;   // USN_REASON_RENAME_NEW_NAME
                constexpr DWORDLONG kFileDelete = 0x00000010;  // USN_REASON_FILE_DELETE
                if (nameLen > 0 && (nameLen % sizeof(WCHAR)) == 0) {
                    Change c;
                    c.frn = frn;
                    c.parentFrn = pfrn;
                    c.attrs = attrs;
                    c.lastWrite = mtime;
                    c.deleted = (reason & kFileDelete) && !(reason & kRenameNew);
                    c.name.assign(name, nameLen / sizeof(WCHAR));
                    changes.push_back(std::move(c));
                }
                off += rec->RecordLength;
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
            // Applied everything currently in the journal.
            if (ApplyChanges(changes)) {
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

bool VolumeIndex::CollectChildren(const std::wstring& dirPath,
                                  const std::function<void(const FileEntry&)>& cb) const {
    const Node* dir = FindByPath(dirPath, true);
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
