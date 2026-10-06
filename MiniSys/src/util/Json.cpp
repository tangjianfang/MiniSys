#include "util/Json.h"

#include <cmath>
#include <cstdint>
#include <sstream>

namespace minisys {

Json Json::Array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::Object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

long long Json::AsInt(long long def) const {
    if (!IsNumber()) return def;
    return static_cast<long long>(std::llround(num_));
}

size_t Json::Size() const {
    if (type_ == Type::Array)  return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const Json& Json::At(size_t i) const {
    if (type_ == Type::Array && i < arr_.size()) return arr_[i];
    static const Json nullVal;
    return nullVal;
}

void Json::Push(Json v) {
    if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
    arr_.push_back(std::move(v));
}

bool Json::Has(const std::wstring& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
}

const Json& Json::Get(const std::wstring& key) const {
    if (type_ == Type::Object) {
        auto it = obj_.find(key);
        if (it != obj_.end()) return it->second;
    }
    static const Json nullVal;
    return nullVal;
}

void Json::Set(std::wstring key, Json v) {
    if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
    obj_[std::move(key)] = std::move(v);
}

std::wstring Json::Escape(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  r += L"\\\""; break;
            case L'\\': r += L"\\\\"; break;
            case L'\b': r += L"\\b";  break;
            case L'\f': r += L"\\f";  break;
            case L'\n': r += L"\\n";  break;
            case L'\r': r += L"\\r";  break;
            case L'\t': r += L"\\t";  break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                    r += buf;
                } else {
                    r += c;
                }
        }
    }
    return r;
}

std::wstring Json::Dump() const {
    switch (type_) {
        case Type::Null:   return L"null";
        case Type::Bool:   return bool_ ? L"true" : L"false";
        case Type::Number: {
            wchar_t buf[40];
            if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                swprintf_s(buf, L"%lld", static_cast<long long>(num_));
            } else {
                swprintf_s(buf, L"%.17g", num_);
            }
            return buf;
        }
        case Type::String:
            return L"\"" + Escape(str_) + L"\"";
        case Type::Array: {
            std::wstring r = L"[";
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) r += L",";
                r += arr_[i].Dump();
            }
            return r + L"]";
        }
        case Type::Object: {
            std::wstring r = L"{";
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) r += L",";
                first = false;
                r += L"\"" + Escape(kv.first) + L"\":" + kv.second.Dump();
            }
            return r + L"}";
        }
    }
    return L"null";
}

class Json::Parser {
public:
    explicit Parser(std::wstring text) : s_(std::move(text)) {
        // Strip a UTF-8 BOM if the caller passed one through (v2.2: a BOM
        // used to fail parsing at offset 0 with a confusing message).
        if (!s_.empty() && s_[0] == static_cast<wchar_t>(0xFEFF)) s_.erase(0, 1);
    }

    bool Parse(Json& out, std::wstring& err) {
        SkipWs();
        if (!ParseValue(out)) { err = err_; return false; }
        SkipWs();
        if (pos_ != s_.size()) {
            err = L"trailing characters at offset " + std::to_wstring(pos_);
            return false;
        }
        return true;
    }

private:
    // REVIEW P0-3 (05-T-C2a): recursion used to be unbounded — a corrupted
    // rules.json with 10k '[' crashed the process with a stack overflow
    // (0xC00000FD). Depth is capped well above any legitimate document.
    static constexpr int kMaxDepth = 64;

    void SkipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == L' ' || s_[pos_] == L'\t' || s_[pos_] == L'\r' ||
                s_[pos_] == L'\n')) {
            ++pos_;
        }
    }

    bool Fail(const std::wstring& msg) {
        if (err_.empty()) err_ = msg + L" at offset " + std::to_wstring(pos_);
        return false;
    }

    bool ParseValue(Json& out) {
        if (depth_ >= kMaxDepth) return Fail(L"nesting too deep");
        if (pos_ >= s_.size()) return Fail(L"unexpected end");
        wchar_t c = s_[pos_];
        if (c == L'{') return ParseObject(out);
        if (c == L'[') return ParseArray(out);
        if (c == L'"') {
            std::wstring str;
            if (!ParseString(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (c == L't' || c == L'f') return ParseBool(out);
        if (c == L'n') return ParseNull(out);
        return ParseNumber(out);
    }

    bool ParseObject(Json& out) {
        ++pos_; // '{'
        out = Json::Object();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == L'}') { ++pos_; return true; }
        ++depth_;
        struct Guard { int& d; ~Guard() { --d; } } g{ depth_ };
        for (;;) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != L'"')
                return Fail(L"expected object key");
            std::wstring key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != L':')
                return Fail(L"expected ':'");
            ++pos_;
            // REVIEW P0-3 (03-B4 / 05-T-C1): whitespace between ':' and the
            // value was not skipped — the shipped rules.json ("version": 2)
            // failed with "bad number at offset 14" and the external rule
            // table never loaded once.
            SkipWs();
            Json val;
            if (!ParseValue(val)) return false;
            out.Set(std::move(key), std::move(val));
            SkipWs();
            if (pos_ >= s_.size()) return Fail(L"unterminated object");
            if (s_[pos_] == L',') { ++pos_; SkipWs(); continue; }
            if (s_[pos_] == L'}') { ++pos_; return true; }
            return Fail(L"expected ',' or '}'");
        }
    }

    bool ParseArray(Json& out) {
        ++pos_; // '['
        out = Json::Array();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == L']') { ++pos_; return true; }
        ++depth_;
        struct Guard { int& d; ~Guard() { --d; } } g{ depth_ };
        for (;;) {
            // REVIEW P0-3: array elements after ',' need the same whitespace
            // skip as object values.
            SkipWs();
            Json val;
            if (!ParseValue(val)) return false;
            out.Push(std::move(val));
            SkipWs();
            if (pos_ >= s_.size()) return Fail(L"unterminated array");
            if (s_[pos_] == L',') { ++pos_; continue; }
            if (s_[pos_] == L']') { ++pos_; return true; }
            return Fail(L"expected ',' or ']'");
        }
    }

    bool ParseString(std::wstring& out) {
        ++pos_; // '"'
        out.clear();
        while (pos_ < s_.size()) {
            wchar_t c = s_[pos_++];
            if (c == L'"') return true;
            if (c != L'\\') { out += c; continue; }
            if (pos_ >= s_.size()) return Fail(L"bad escape");
            wchar_t e = s_[pos_++];
            switch (e) {
                case L'"':  out += L'"';  break;
                case L'\\': out += L'\\'; break;
                case L'/':  out += L'/';  break;
                case L'b':  out += L'\b'; break;
                case L'f':  out += L'\f'; break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                case L'u': {
                    if (pos_ + 4 > s_.size()) return Fail(L"bad \\u escape");
                    unsigned v = 0;
                    for (int i = 0; i < 4; ++i) {
                        wchar_t h = s_[pos_++];
                        v <<= 4;
                        if (h >= L'0' && h <= L'9') v |= (h - L'0');
                        else if (h >= L'a' && h <= L'f') v |= (h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') v |= (h - L'A' + 10);
                        else return Fail(L"bad hex digit");
                    }
                    if (v >= 0xD800 && v <= 0xDBFF && pos_ + 6 <= s_.size() &&
                        s_[pos_] == L'\\' && s_[pos_ + 1] == L'u') {
                        // Surrogate pair
                        unsigned lo = 0;
                        bool ok = true;
                        for (int i = 0; i < 4; ++i) {
                            wchar_t h = s_[pos_ + 2 + i];
                            lo <<= 4;
                            if (h >= L'0' && h <= L'9') lo |= (h - L'0');
                            else if (h >= L'a' && h <= L'f') lo |= (h - L'a' + 10);
                            else if (h >= L'A' && h <= L'F') lo |= (h - L'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            pos_ += 6;
                            unsigned cp = 0x10000 +
                                ((v - 0xD800) << 10) + (lo - 0xDC00);
                            out += static_cast<wchar_t>(cp);
                            break;
                        }
                    }
                    out += static_cast<wchar_t>(v);
                    break;
                }
                default:
                    return Fail(L"bad escape char");
            }
        }
        return Fail(L"unterminated string");
    }

    bool ParseBool(Json& out) {
        if (s_.compare(pos_, 4, L"true") == 0) {
            pos_ += 4; out = Json(true); return true;
        }
        if (s_.compare(pos_, 5, L"false") == 0) {
            pos_ += 5; out = Json(false); return true;
        }
        return Fail(L"bad literal");
    }

    bool ParseNull(Json& out) {
        if (s_.compare(pos_, 4, L"null") == 0) {
            pos_ += 4; out = Json(); return true;
        }
        return Fail(L"bad literal");
    }

    bool ParseNumber(Json& out) {
        // REVIEW P0-3 (05-T-C2): the old scanner greedily consumed
        // [0-9.eE+-] and let stod accept any valid prefix — "1.2.3" parsed
        // as 1.2 silently. Grammar is now validated in full:
        //   '-'? int frac? exp?   with int = '0' | [1-9][0-9]*
        size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == L'-') ++pos_;   // leading '+' invalid
        auto digits = [&]() -> int {
            int n = 0;
            while (pos_ < s_.size() && s_[pos_] >= L'0' && s_[pos_] <= L'9') {
                ++pos_; ++n;
            }
            return n;
        };
        if (pos_ < s_.size() && s_[pos_] == L'0') {
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] >= L'0' && s_[pos_] <= L'9') {
                return Fail(L"leading zero in number");
            }
        } else if (digits() == 0) {
            return Fail(L"bad number");
        }
        if (pos_ < s_.size() && s_[pos_] == L'.') {
            ++pos_;
            if (digits() == 0) return Fail(L"bad number");
        }
        if (pos_ < s_.size() && (s_[pos_] == L'e' || s_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == L'+' || s_[pos_] == L'-')) ++pos_;
            if (digits() == 0) return Fail(L"bad number");
        }
        std::wstring token = s_.substr(start, pos_ - start);
        try {
            size_t end = 0;
            double d = std::stod(token, &end);
            if (end != token.size()) return Fail(L"bad number");
            out = Json(d);
            return true;
        } catch (...) {
            return Fail(L"bad number");
        }
    }

    std::wstring s_;
    size_t pos_ = 0;
    int depth_ = 0;
    std::wstring err_;
};

bool Json::Parse(const std::wstring& text, Json& out, std::wstring& err) {
    Parser p(text);
    return p.Parse(out, err);
}

} // namespace minisys
