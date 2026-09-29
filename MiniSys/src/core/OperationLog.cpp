#include "core/OperationLog.h"

#include "util/Json.h"
#include "util/Logger.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"

#include <windows.h>
#include <atomic>
#include <fstream>

namespace minisys {

namespace {

// Path override for unit tests (empty = default location).
std::filesystem::path g_historyOverride;

std::wstring NowTimestamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ---------- v1 legacy TSV helpers (read + migration only) ----------

std::wstring TsvEscape(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (auto c : s) {
        switch (c) {
            case L'%': r += L"%25"; break;
            case L'\t': r += L"%09"; break;
            case L'\r': r += L"%0D"; break;
            case L'\n': r += L"%0A"; break;
            default: r += c;
        }
    }
    return r;
}

std::wstring TsvUnescape(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            auto h = s.substr(i + 1, 2);
            if (h == L"25") { r += L'%'; i += 2; continue; }
            if (h == L"09") { r += L'\t'; i += 2; continue; }
            if (h == L"0D") { r += L'\r'; i += 2; continue; }
            if (h == L"0A") { r += L'\n'; i += 2; continue; }
        }
        r += s[i];
    }
    return r;
}

std::vector<std::wstring> TsvSplit(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (auto c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

bool ParseLegacyTsvLine(const std::wstring& line, OpRecord& r) {
    auto parts = TsvSplit(line, L'\t');
    if (parts.size() < 9) return false;
    r.id        = TsvUnescape(parts[0]);
    r.timestamp = TsvUnescape(parts[1]);
    r.type      = OperationLog::StrToType(TsvUnescape(parts[2]));
    r.status    = OperationLog::StrToStatus(TsvUnescape(parts[3]));
    try { r.sizeBytes = std::stoull(parts[4]); } catch (...) { r.sizeBytes = 0; }
    r.isReversible = (parts[5] == L"1");
    r.source    = TsvUnescape(parts[6]);
    r.target    = TsvUnescape(parts[7]);
    r.note      = TsvUnescape(parts[8]);
    return true;
}

// ---------- JSONL ----------

Json RecordToJson(const OpRecord& r) {
    Json j = Json::Object();
    j.Set(L"id", Json(r.id));
    j.Set(L"timestamp", Json(r.timestamp.empty() ? NowTimestamp() : r.timestamp));
    j.Set(L"type", Json(OperationLog::TypeToStr(r.type)));
    j.Set(L"status", Json(OperationLog::StatusToStr(r.status)));
    j.Set(L"size", Json(static_cast<double>(r.sizeBytes)));
    j.Set(L"reversible", Json(r.isReversible));
    j.Set(L"source", Json(r.source));
    j.Set(L"target", Json(r.target));
    j.Set(L"note", Json(r.note));
    return j;
}

bool ParseJsonlLine(const std::wstring& line, OpRecord& r) {
    Json j;
    std::wstring err;
    if (!Json::Parse(line, j, err) || !j.IsObject()) return false;
    if (!j.Has(L"id") || !j.Has(L"type")) return false;
    r.id          = j.Get(L"id").AsString();
    r.timestamp   = j.Get(L"timestamp").AsString();
    r.type        = OperationLog::StrToType(j.Get(L"type").AsString());
    r.status      = OperationLog::StrToStatus(j.Get(L"status").AsString());
    r.sizeBytes   = static_cast<unsigned long long>(j.Get(L"size").AsNumber(0));
    r.isReversible = j.Get(L"reversible").AsBool(true);
    r.source      = j.Get(L"source").AsString();
    r.target      = j.Get(L"target").AsString();
    r.note        = j.Get(L"note").AsString();
    return true;
}

std::vector<std::wstring> ReadAllLines(const std::filesystem::path& p) {
    std::vector<std::wstring> lines;
    std::ifstream f(p, std::ios::binary);
    if (!f) return lines;
    std::string utf8Line;
    while (std::getline(f, utf8Line)) {
        if (!utf8Line.empty() && utf8Line.back() == '\r') utf8Line.pop_back();
        if (utf8Line.empty()) continue;
        lines.push_back(Utf8ToWide(utf8Line));
    }
    return lines;
}

void WriteAllLines(const std::filesystem::path& p,
                   const std::vector<std::wstring>& lines) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        MS_LOG_WARN(L"Failed to open history file for rewrite");
        return;
    }
    for (const auto& l : lines) {
        f << WideToUtf8(l) << "\n";
    }
}

// Rewrite the JSONL file from records (oldest first), atomically.
void RewriteAtomic(const std::filesystem::path& path,
                   const std::vector<OpRecord>& recordsOldestFirst) {
    std::vector<std::wstring> lines;
    lines.reserve(recordsOldestFirst.size());
    for (const auto& r : recordsOldestFirst) {
        lines.push_back(RecordToJson(r).Dump());
    }
    auto tmp = path;
    tmp += L".tmp";
    WriteAllLines(tmp, lines);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MS_LOG_WARN(L"Atomic history rewrite failed (Win32 %lu)", GetLastError());
        // Fall back to direct write so the update is not lost.
        WriteAllLines(path, lines);
    }
}

} // namespace

std::filesystem::path OperationLog::JsonlPath() {
    if (!g_historyOverride.empty()) return g_historyOverride;
    return HistoryDir() / L"history.jsonl";
}

std::filesystem::path OperationLog::LegacyTsvPath() {
    if (!g_historyOverride.empty()) {
        auto p = g_historyOverride;
        p.replace_filename(L"history.tsv");
        return p;
    }
    return HistoryDir() / L"history.tsv";
}

void OperationLog::SetHistoryPathForTesting(const std::filesystem::path& p) {
    g_historyOverride = p;
}

OperationLog& OperationLog::Instance() {
    static OperationLog inst;
    inst.MigrateLegacyIfNeeded();
    return inst;
}

void OperationLog::MigrateLegacyIfNeeded() {
    std::lock_guard<std::mutex> g(mu_);
    auto tsv = LegacyTsvPath();
    auto jsonl = JsonlPath();
    if (tsv == jsonl) return;
    if (!FileExists(tsv)) return;

    auto lines = ReadAllLines(tsv);
    std::vector<std::wstring> out;
    out.reserve(lines.size());
    for (const auto& line : lines) {
        OpRecord r;
        if (ParseLegacyTsvLine(line, r)) {
            out.push_back(RecordToJson(r).Dump());
        }
    }
    if (!out.empty() && !FileExists(jsonl)) {
        WriteAllLines(jsonl, out);
        MS_LOG_INFO(L"Migrated %zu legacy TSV records to JSONL", out.size());
    }
    // Keep the original as .bak regardless (idempotent, safe to re-run).
    auto bak = tsv;
    bak += L".bak";
    MoveFileExW(tsv.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void OperationLog::Append(const OpRecord& rec) {
    std::lock_guard<std::mutex> g(mu_);
    std::ofstream f(JsonlPath(), std::ios::binary | std::ios::app);
    if (!f) {
        MS_LOG_WARN(L"Failed to open history file for append");
        return;
    }
    f << WideToUtf8(RecordToJson(rec).Dump()) << "\n";
}

void OperationLog::UpdateStatus(const std::wstring& id, OpStatus status,
                                const std::wstring& note) {
    auto all = LoadAll();   // newest first
    {
        std::lock_guard<std::mutex> g(mu_);
        // Records were loaded newest-first; rewrite oldest-first to preserve
        // original order.
        std::vector<OpRecord> oldestFirst(all.rbegin(), all.rend());
        for (auto& r : oldestFirst) {
            if (r.id == id) {
                r.status = status;
                if (!note.empty()) r.note = note;
            }
        }
        RewriteAtomic(JsonlPath(), oldestFirst);
    }
}

std::vector<OpRecord> OperationLog::LoadAll() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<OpRecord> out;
    for (const auto& line : ReadAllLines(JsonlPath())) {
        OpRecord r;
        // Lines starting with '{' are JSONL; anything else is a v1 TSV row
        // (e.g. when the user points us at an old file).
        bool ok = (!line.empty() && line[0] == L'{')
                    ? ParseJsonlLine(line, r)
                    : ParseLegacyTsvLine(line, r);
        if (ok) out.push_back(std::move(r));
    }
    std::reverse(out.begin(), out.end());   // newest first
    return out;
}

std::wstring OperationLog::NewId() {
    static std::atomic<unsigned long long> counter{0};
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d%02d%02d-%02d%02d%02d-%llu",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        counter.fetch_add(1));
    return buf;
}

std::wstring OperationLog::TypeToStr(OpType t) {
    switch (t) {
        case OpType::DeleteToRecycleBin: return L"DELETE";
        case OpType::EmptyRecycleBin:    return L"EMPTY_RECYCLE";
        case OpType::MoveAndJunction:    return L"MOVE_JUNCTION";
        case OpType::MoveFilePath:       return L"MOVE_FILE";
        case OpType::Quarantine:         return L"QUARANTINE";
        case OpType::Delegate:           return L"DELEGATE";
    }
    return L"UNKNOWN";
}

OpType OperationLog::StrToType(const std::wstring& s) {
    if (s == L"DELETE")        return OpType::DeleteToRecycleBin;
    if (s == L"EMPTY_RECYCLE") return OpType::EmptyRecycleBin;
    if (s == L"MOVE_JUNCTION") return OpType::MoveAndJunction;
    if (s == L"MOVE_FILE")     return OpType::MoveFilePath;
    if (s == L"QUARANTINE")    return OpType::Quarantine;
    if (s == L"DELEGATE")      return OpType::Delegate;
    return OpType::DeleteToRecycleBin;
}

std::wstring OperationLog::StatusToStr(OpStatus s) {
    switch (s) {
        case OpStatus::Pending:     return L"PENDING";
        case OpStatus::Success:     return L"SUCCESS";
        case OpStatus::Failed:      return L"FAILED";
        case OpStatus::Reverted:    return L"REVERTED";
        case OpStatus::Interrupted: return L"INTERRUPTED";
    }
    return L"PENDING";
}

OpStatus OperationLog::StrToStatus(const std::wstring& s) {
    if (s == L"SUCCESS")     return OpStatus::Success;
    if (s == L"FAILED")      return OpStatus::Failed;
    if (s == L"REVERTED")    return OpStatus::Reverted;
    if (s == L"INTERRUPTED") return OpStatus::Interrupted;
    return OpStatus::Pending;
}

} // namespace minisys
