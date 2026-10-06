#include "core/VolumeIndex.h"

#include "util/Logger.h"
#include "util/PathUtils.h"
#include "core/JunkRules.h"
#include "util/StringUtils.h"
#include <string_view>

#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <share.h>

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
    valid_.store(false, std::memory_order_release);
    entryCount_.store(0, std::memory_order_release);
    ready_ = false;
    walkBuilt_ = false;
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
    valid_.store(false, std::memory_order_release);   // not volume-backed, queries work
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
    if (!(valid_ && drive == drive_)) {
        // v2.5: disk cache first — the journal refresh below brings it
        // current (or rejects it) exactly like an in-memory index.
        if (TryLoadCache(drive) && progress) {
            progress(L"已加载索引缓存: " + std::to_wstring(nodes_.size()) + L" 项");
        }
    }
    if (valid_ && drive == drive_) {
        // Cheap revalidation: volume serial + journal identity.
        DWORD serial = 0;
        if (GetVolumeInformationW(RootPath(drive).c_str(), nullptr, 0, &serial,
                                  nullptr, nullptr, nullptr, 0)) {
            if (static_cast<uint64_t>(serial) == volumeSerial_) {
                // Same volume — try an incremental refresh (M3) before
                // considering a rebuild.
                if (RefreshFromUsn(cancel)) {
                    SaveCacheIfWorthwhile();
                    return true;
                }
                valid_.store(false, std::memory_order_release);
                return BuildFull(drive, progress, cancel);
            }
        }
        valid_.store(false, std::memory_order_release);
    }
    bool ok = BuildFull(drive, progress, cancel);
    if (ok) SaveCacheIfWorthwhile();
    return ok;
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
        std::vector<char> buf(64 * 1024);
        // v2.2 (REVIEW P0-7 / 03-B1): MFT_ENUM_DATA is an alias of V1 on the
        // current SDK — Min/MaxMajorVersion must be 2..3 or the FSCTL fails
        // with Win32 87.
        //
        // v2.5: two live machines report a deterministic TRUNCATED enum
        // (27 / 456 records instead of millions, no error — plain EOF). Docs
        // confirm zeroed LowUsn/HighUsn mean "no bound", so the V1 request
        // below is textbook-correct; the defect is not reproducible by
        // inspection. Strategy: (a) log per-batch diagnostics so the next
        // run pinpoints exactly where the stream stops, (b) when the V1
        // result looks truncated for a system volume, transparently retry
        // with the classic V0 request (V2 records only).
        auto enumerate = [&](bool useV1) -> bool {
            nodes_.clear();
            names_.clear();
            MFT_ENUM_DATA_V1 medV1{};
            MFT_ENUM_DATA_V0 medV0{};
            if (useV1) {
                medV1.MinMajorVersion = 2;   // V2 (NTFS) + V3 (ReFS/128-bit IDs)
                medV1.MaxMajorVersion = 3;
            }
            void* med   = useV1 ? static_cast<void*>(&medV1) : static_cast<void*>(&medV0);
            DWORD medSz = useV1 ? sizeof(medV1) : sizeof(medV0);
            DWORD ret = 0;
            size_t batches = 0, skipped = 0, reported = 0;
            for (;;) {
                if (cancel.load()) return false;
                if (!DeviceIoControl(vol, FSCTL_ENUM_USN_DATA, med, medSz,
                                     buf.data(), static_cast<DWORD>(buf.size()),
                                     &ret, nullptr)) {
                    DWORD e = GetLastError();
                    if (e == ERROR_HANDLE_EOF) break;     // enumeration complete
                    MS_LOG_WARN(L"VolumeIndex: FSCTL_ENUM_USN_DATA(V%d) failed "
                                L"after %zu records (Win32 %lu)",
                                useV1 ? 1 : 0, nodes_.size(), e);
                    return false;
                }
                ++batches;
                if (ret <= sizeof(DWORDLONG)) break;      // only the next-FRN marker
                DWORDLONG nextFrn = *reinterpret_cast<DWORDLONG*>(buf.data());
                size_t off = sizeof(DWORDLONG);
                size_t batchRecords = 0;
                while (off + sizeof(DWORD) <= ret) {
                    auto* hdr = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(
                        buf.data() + off);
                    if (hdr->RecordLength < sizeof(*hdr) ||
                        off + hdr->RecordLength > ret) {
                        ++skipped;
                        break;
                    }
                    ParsedRecord pr;
                    if (ParseUsnRecord(
                            reinterpret_cast<const BYTE*>(buf.data() + off), pr) &&
                        pr.nameLenBytes > 0 &&
                        (pr.nameLenBytes % sizeof(WCHAR)) == 0) {
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
                        ++batchRecords;
                    } else {
                        ++skipped;
                    }
                    off += hdr->RecordLength;
                }
                // Diagnostics: the first batches + a heartbeat every 200.
                if (batches <= 3 || (batches % 200) == 0) {
                    MS_LOG_INFO(L"VolumeIndex: batch %zu: %lu bytes, %zu records "
                                L"(total %zu, skipped %zu)",
                                batches, ret, batchRecords, nodes_.size(), skipped);
                }
                if (reported + 100000 <= nodes_.size()) {
                    reported = nodes_.size();
                    if (progress) {
                        progress(L"索引: " + std::to_wstring(nodes_.size()) + L" 项");
                    }
                }
                if (useV1) medV1.StartFileReferenceNumber = nextFrn;
                else       medV0.StartFileReferenceNumber = nextFrn;
            }
            MS_LOG_INFO(L"VolumeIndex: enum done (V%d): %zu records in %zu batches, "
                        L"%zu skipped",
                        useV1 ? 1 : 0, nodes_.size(), batches, skipped);
            return true;
        };
        ok = enumerate(true);
        if (ok && nodes_.size() < 10000) {
            size_t v1Count = nodes_.size();
            MS_LOG_WARN(L"VolumeIndex: V1 enumeration returned only %zu records — "
                        L"retrying with the V0 request", v1Count);
            if (enumerate(false) && nodes_.size() > v1Count) {
                MS_LOG_INFO(L"VolumeIndex: V0 retry recovered %zu records (V1 gave "
                            L"%zu) — the V1 path is broken on this machine",
                            nodes_.size(), v1Count);
            }
        }
    }

    // v2.6: both USN shapes still look truncated → the MFT stream is broken
    // on this machine. Fall back to a filesystem walk so search has REAL
    // data instead of a confident 262-entry index.
    if (ok && !cancel.load() && nodes_.size() < 10000) {
        MS_LOG_WARN(L"VolumeIndex: USN enumeration truncated after both request "
                    L"shapes (%zu records) — falling back to a filesystem walk",
                    nodes_.size());
        if (progress) progress(L"USN 枚举异常，回退为文件系统遍历（较慢）…");
        ok = WalkBuild(drive, progress, cancel);
    }

    if (vol != INVALID_HANDLE_VALUE) CloseHandle(vol);

    if (ok && !nodes_.empty()) {
        Finalize();
        entryCount_.store(nodes_.size(), std::memory_order_release);
        valid_.store(true, std::memory_order_release);
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

// v2.6: filesystem-walk fallback (see header). Synthetic FRNs are unique
// within the session; the journal machinery is intentionally disabled.
bool VolumeIndex::WalkBuild(wchar_t drive,
                            const std::function<void(const std::wstring&)>& progress,
                            const std::atomic<bool>& cancel) {
    nodes_.clear();
    names_.clear();
    walkBuilt_ = false;
    journalId_ = 0;
    nextUsn_ = 0;

    uint64_t nextFrn = 1;
    Node root{};
    root.frn = nextFrn;
    root.parentFrn = nextFrn;   // self-parent → Finalize treats it as root
    root.isDir = true;
    nodes_.push_back(root);
    uint64_t rootFrn = nextFrn++;

    struct Frame { std::wstring path; uint64_t parentFrn; };
    std::vector<Frame> stack{ { RootPath(drive), rootFrn } };

    std::vector<WIN32_FIND_DATAW> entries;
    size_t reported = 0;
    while (!stack.empty()) {
        if (cancel.load()) return false;
        Frame fr = stack.back();
        stack.pop_back();

        entries.clear();
        std::wstring search = fr.path;
        if (!search.empty() && search.back() != L'\\') search += L'\\';
        search += L'*';
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(
            LongPath(std::filesystem::path(search)).c_str(),
            FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            Node node{};
            node.frn       = nextFrn++;
            node.parentFrn = fr.parentFrn;
            node.attrs     = fd.dwFileAttributes;
            node.lastWrite = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                             static_cast<uint64_t>(fd.ftLastWriteTime.dwLowDateTime);
            node.isDir     = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            node.nameOff   = static_cast<uint32_t>(names_.size());
            node.nameLen   = static_cast<uint32_t>(wcslen(n));
            names_ += n;
            uint64_t frn = node.frn;
            bool isDir = node.isDir;
            nodes_.push_back(std::move(node));

            if (isDir) {
                stack.push_back({ fr.path + L"\\" + n, frn });
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        if (reported + 100000 <= nodes_.size()) {
            reported = nodes_.size();
            if (progress) {
                progress(L"遍历: " + std::to_wstring(nodes_.size()) + L" 项");
            }
        }
    }
    walkBuilt_ = !nodes_.empty();
    MS_LOG_INFO(L"VolumeIndex: walk fallback built %zu entries on %c:",
                nodes_.size(), drive);
    return walkBuilt_;
}

bool VolumeIndex::RefreshFromUsn(const std::atomic<bool>& cancel) {
    if (walkBuilt_) return true;   // no journal for walk-built indexes
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
    entryCount_.store(nodes_.size(), std::memory_order_release);
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

VolumeIndex::SearchFilter VolumeIndex::ParseFilterTerms(const std::wstring& query,
                                                         std::wstring& freeQuery) {
    SearchFilter f;
    freeQuery.clear();
    for (const auto& tok : SplitQuery(query)) {
        std::wstring lower = ToLower(tok);
        if (lower == L"folder:" || lower == L"is:folder" || lower == L"dir:") {
            f.dirsOnly = true;
        } else if (lower == L"file:" || lower == L"is:file") {
            f.filesOnly = true;
        } else if (lower.rfind(L"ext:", 0) == 0 && tok.size() > 4) {
            std::wstring cur;
            for (wchar_t ch : tok.substr(4)) {
                if (ch == L';' || ch == L',' || ch == L'|') {
                    if (!cur.empty()) { f.exts.push_back(ToLower(cur)); cur.clear(); }
                } else {
                    cur += ch;
                }
            }
            if (!cur.empty()) f.exts.push_back(ToLower(cur));
        } else {
            if (!freeQuery.empty()) freeQuery += L' ';
            freeQuery += tok;
        }
    }
    for (auto& e : f.exts) {
        if (!e.empty() && e[0] == L'.') e.erase(0, 1);   // "ext:.cpp" → "cpp"
    }
    return f;
}

std::wstring VolumeIndex::ToggleQueryToken(const std::wstring& current,
                                           const std::wstring& newToken) {
    auto colon = newToken.find(L':');
    std::wstring prefix = (colon == std::wstring::npos)
        ? ToLower(newToken)
        : ToLower(newToken.substr(0, colon + 1));   // e.g. "ext:"
    std::wstring out;
    for (const auto& tok : SplitQuery(current)) {
        if (!prefix.empty() &&
            ToLower(tok).rfind(prefix, 0) == 0) {
            continue;   // same category — replaced by the new token
        }
        if (!out.empty()) out += L' ';
        out += tok;
    }
    if (!newToken.empty()) {
        if (!out.empty()) out += L' ';
        out += newToken;
    }
    return out;
}

size_t VolumeIndex::Search(const std::wstring& query, bool matchPath,
                           size_t maxResults,
                           const std::function<bool(const SearchHit&)>& sink,
                           const SearchFilter& filter) const {
    if (!ready_) return 0;
    auto terms = SplitQuery(query);
    bool hasFilter = filter.dirsOnly || filter.filesOnly || !filter.exts.empty();
    // Empty terms match everything — allowed only when a filter token
    // constrains the stream ("folder:" alone lists directories, Everything-style).
    if ((terms.empty() && !hasFilter) || maxResults == 0) return 0;

    // Pass 1: collect matching nodes (name-first matching avoids path
    // construction for the common case; a term that fails on the name is
    // retried against the full lowercased path only when matchPath is on).
    struct RawHit { uint32_t nodeIdx; std::wstring name; };
    std::vector<RawHit> hits;
    hits.reserve(maxResults < 4096 ? maxResults : 4096);

    auto extMatches = [&](std::wstring_view name) {
        auto dot = name.rfind(L'.');
        if (dot == std::wstring_view::npos) return false;
        std::wstring_view ext = name.substr(dot + 1);
        for (const auto& e : filter.exts) {
            if (ext.size() != e.size()) continue;
            size_t i = 0;
            for (; i < ext.size(); ++i) {
                if (FoldCh(ext[i]) != e[i]) break;
            }
            if (i == ext.size()) return true;
        }
        return false;
    };

    for (uint32_t i = 0; i < nodes_.size() && hits.size() < maxResults; ++i) {
        const Node& n = nodes_[i];
        if (n.frn == 0 || n.parentFrn == n.frn) continue;   // tombstone/root
        // v2.5 Everything-style filters — applied during the scan so filtered
        // entries never consume a result slot.
        if (filter.dirsOnly && !n.isDir) continue;
        if (filter.filesOnly && n.isDir) continue;
        if (!filter.exts.empty() && (n.isDir || !extMatches(
                std::wstring_view(names_.data() + n.nameOff, n.nameLen)))) {
            continue;
        }
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

// ---- v2.5: disk cache ------------------------------------------------------
// Binary layout (little-endian, packed rows — see SaveCacheIfWorthwhile):
//   magic u32 "MISX" | version u32 | drive wchar | serial u64 | journal u64
//   nextUsn i64 | nodeCount u64 | nameBytes u64
//   nodeCount × 40-byte rows | nameBytes bytes of UTF-16 name pool

namespace {
constexpr uint32_t kCacheMagic   = 0x5853494D;   // "MISX"
constexpr uint32_t kCacheVersion = 1;
constexpr size_t   kMinCachedEntries = 10000;    // never persist a truncated index

#pragma pack(push, 1)
struct CacheRow {
    uint64_t frn;
    uint64_t parentFrn;
    uint64_t lastWrite;
    uint32_t nameOff;
    uint32_t nameLen;
    uint32_t attrs;
    uint8_t  isDir;
    uint8_t  pad[3];
};
#pragma pack(pop)
static_assert(sizeof(CacheRow) == 40, "cache row layout");

std::filesystem::path CacheFilePath() {
    return std::filesystem::path(AppDataDir()) / L"index-cache.bin";
}
} // namespace

bool VolumeIndex::TryLoadCache(wchar_t drive) {
    auto path = CacheFilePath();
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return false;
    bool ok = false;
    do {
        uint32_t magic = 0, version = 0;
        if (fread(&magic, 4, 1, f) != 1 || magic != kCacheMagic) break;
        if (fread(&version, 4, 1, f) != 1 || version != kCacheVersion) break;
        wchar_t cDrive = 0;
        uint64_t cSerial = 0, cJournal = 0, cNodeCount = 0, cNameBytes = 0;
        int64_t cNextUsn = 0;
        if (fread(&cDrive, sizeof(wchar_t), 1, f) != 1) break;
        if (fread(&cSerial, 8, 1, f) != 1) break;
        if (fread(&cJournal, 8, 1, f) != 1) break;
        if (fread(&cNextUsn, 8, 1, f) != 1) break;
        if (fread(&cNodeCount, 8, 1, f) != 1) break;
        if (fread(&cNameBytes, 8, 1, f) != 1) break;
        if (cDrive != drive || cNodeCount < kMinCachedEntries ||
            cNodeCount > 50000000 || cNameBytes == 0 ||
            cNameBytes % sizeof(wchar_t) != 0) {
            break;
        }
        // The cache is only valid for the same volume.
        DWORD serial = 0;
        if (!GetVolumeInformationW(RootPath(drive).c_str(), nullptr, 0, &serial,
                                   nullptr, nullptr, nullptr, 0)) break;
        if (static_cast<uint64_t>(serial) != cSerial) break;

        nodes_.resize(static_cast<size_t>(cNodeCount));
        CacheRow row{};
        bool rowsOk = true;
        for (auto& n : nodes_) {
            if (fread(&row, sizeof(row), 1, f) != 1) { rowsOk = false; break; }
            n.frn        = row.frn;
            n.parentFrn  = row.parentFrn;
            n.lastWrite  = row.lastWrite;
            n.nameOff    = row.nameOff;
            n.nameLen    = row.nameLen;
            n.attrs      = row.attrs;
            n.isDir      = row.isDir != 0;
        }
        if (!rowsOk) break;
        names_.resize(static_cast<size_t>(cNameBytes) / sizeof(wchar_t));
        if (!names_.empty() &&
            fread(names_.data(), sizeof(wchar_t), names_.size(), f) != names_.size()) {
            break;
        }
        // Name offsets must land inside the pool — a corrupt file is
        // silently discarded rather than trusted.
        bool boundsOk = true;
        for (const auto& n : nodes_) {
            if (n.nameOff > names_.size() ||
                names_.size() - n.nameOff < n.nameLen) {
                boundsOk = false;
                break;
            }
        }
        if (!boundsOk) break;

        drive_        = drive;
        volumeSerial_ = cSerial;
        journalId_    = cJournal;
        nextUsn_      = cNextUsn;
        Finalize();
        entryCount_.store(nodes_.size(), std::memory_order_release);
        valid_.store(true, std::memory_order_release);
        ok = true;
        MS_LOG_INFO(L"VolumeIndex: loaded cache for %c: — %zu entries "
                    L"(journal cursor %lld)", drive_, nodes_.size(),
                    static_cast<long long>(nextUsn_));
    } while (false);
    fclose(f);
    if (!ok) {
        nodes_.clear();
        names_.clear();
    }
    return ok;
}

void VolumeIndex::SaveCacheIfWorthwhile() const {
    if (!valid_.load(std::memory_order_acquire)) return;
    if (walkBuilt_) return;   // walk-built indexes are session-only
    if (nodes_.size() < kMinCachedEntries) return;   // truncated indexes stay volatile

    auto final = CacheFilePath();
    auto tmp = final;
    tmp += L".tmp";
    FILE* f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYWR);
    if (!f) return;
    bool ok = fwrite(&kCacheMagic, 4, 1, f) == 1 &&
              fwrite(&kCacheVersion, 4, 1, f) == 1 &&
              fwrite(&drive_, sizeof(wchar_t), 1, f) == 1 &&
              fwrite(&volumeSerial_, 8, 1, f) == 1 &&
              fwrite(&journalId_, 8, 1, f) == 1 &&
              fwrite(&nextUsn_, 8, 1, f) == 1;
    uint64_t nodeCount = nodes_.size();
    uint64_t nameBytes = names_.size() * sizeof(wchar_t);
    ok = ok && fwrite(&nodeCount, 8, 1, f) == 1 && fwrite(&nameBytes, 8, 1, f) == 1;

    CacheRow row{};
    for (const auto& n : nodes_) {
        row.frn       = n.frn;
        row.parentFrn = n.parentFrn;
        row.lastWrite = n.lastWrite;
        row.nameOff   = n.nameOff;
        row.nameLen   = n.nameLen;
        row.attrs     = n.attrs;
        row.isDir     = n.isDir ? 1 : 0;
        if (fwrite(&row, sizeof(row), 1, f) != 1) { ok = false; break; }
    }
    ok = ok && (names_.empty() ||
                fwrite(names_.data(), sizeof(wchar_t), names_.size(), f) ==
                    names_.size());
    fclose(f);
    if (ok) {
        std::error_code ec;
        std::filesystem::remove(final, ec);
        std::filesystem::rename(tmp, final, ec);
        if (ec) MS_LOG_WARN(L"VolumeIndex: cache rename failed (%hs)", ec.message().c_str());
    } else {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        MS_LOG_WARN(L"VolumeIndex: cache write failed — keeping previous");
    }
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
