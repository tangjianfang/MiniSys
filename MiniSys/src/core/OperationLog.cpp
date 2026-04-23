#include "core/OperationLog.h"
#include "util/PathUtils.h"
#include "util/StringUtils.h"
#include "util/Logger.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <atomic>

namespace minisys {

namespace {

std::wstring HistoryFile() {
    return (HistoryDir() / L"history.tsv").wstring();
}

// Escape: replace tab and newline with %09/%0A; '%' -> %25.
std::wstring Escape(const std::wstring& s) {
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

std::wstring Unescape(const std::wstring& s) {
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

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (auto c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::wstring NowTimestamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

} // namespace

OperationLog& OperationLog::Instance() {
    static OperationLog inst;
    return inst;
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
    }
    return L"UNKNOWN";
}

OpType OperationLog::StrToType(const std::wstring& s) {
    if (s == L"DELETE")        return OpType::DeleteToRecycleBin;
    if (s == L"EMPTY_RECYCLE") return OpType::EmptyRecycleBin;
    if (s == L"MOVE_JUNCTION") return OpType::MoveAndJunction;
    if (s == L"MOVE_FILE")     return OpType::MoveFilePath;
    return OpType::DeleteToRecycleBin;
}

std::wstring OperationLog::StatusToStr(OpStatus s) {
    switch (s) {
        case OpStatus::Pending:    return L"PENDING";
        case OpStatus::Success:    return L"SUCCESS";
        case OpStatus::Failed:     return L"FAILED";
        case OpStatus::Reverted:   return L"REVERTED";
        case OpStatus::Interrupted:return L"INTERRUPTED";
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

void OperationLog::Append(const OpRecord& rec) {
    std::lock_guard<std::mutex> g(mu_);
    std::wofstream f(HistoryFile(), std::ios::app);
    if (!f) {
        MS_LOG_WARN(L"Failed to open history file for append");
        return;
    }
    f << Escape(rec.id) << L'\t'
      << Escape(rec.timestamp.empty() ? NowTimestamp() : rec.timestamp) << L'\t'
      << Escape(TypeToStr(rec.type)) << L'\t'
      << Escape(StatusToStr(rec.status)) << L'\t'
      << rec.sizeBytes << L'\t'
      << (rec.isReversible ? 1 : 0) << L'\t'
      << Escape(rec.source) << L'\t'
      << Escape(rec.target) << L'\t'
      << Escape(rec.note)   << L"\n";
}

void OperationLog::UpdateStatus(const std::wstring& id, OpStatus status, const std::wstring& note) {
    auto all = LoadAll();
    {
        std::lock_guard<std::mutex> g(mu_);
        std::wofstream f(HistoryFile(), std::ios::trunc);
        if (!f) return;
        // Records were loaded newest-first; rewrite oldest-first to preserve original order.
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            auto r = *it;
            if (r.id == id) {
                r.status = status;
                if (!note.empty()) r.note = note;
            }
            f << Escape(r.id) << L'\t'
              << Escape(r.timestamp) << L'\t'
              << Escape(TypeToStr(r.type)) << L'\t'
              << Escape(StatusToStr(r.status)) << L'\t'
              << r.sizeBytes << L'\t'
              << (r.isReversible ? 1 : 0) << L'\t'
              << Escape(r.source) << L'\t'
              << Escape(r.target) << L'\t'
              << Escape(r.note) << L"\n";
        }
    }
}

std::vector<OpRecord> OperationLog::LoadAll() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<OpRecord> out;
    std::wifstream f(HistoryFile());
    if (!f) return out;
    std::wstring line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto parts = Split(line, L'\t');
        if (parts.size() < 9) continue;
        OpRecord r;
        r.id        = Unescape(parts[0]);
        r.timestamp = Unescape(parts[1]);
        r.type      = StrToType(Unescape(parts[2]));
        r.status    = StrToStatus(Unescape(parts[3]));
        try { r.sizeBytes = std::stoull(parts[4]); } catch (...) { r.sizeBytes = 0; }
        r.isReversible = (parts[5] == L"1");
        r.source    = Unescape(parts[6]);
        r.target    = Unescape(parts[7]);
        r.note      = Unescape(parts[8]);
        out.push_back(std::move(r));
    }
    // Reverse so newest first.
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace minisys
