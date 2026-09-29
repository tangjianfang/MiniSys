#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace minisys {

// Minimal JSON value + recursive-descent parser. Supports the full JSON
// grammar (objects, arrays, strings with escapes, numbers, bool, null) —
// enough for rules.json and the JSONL operation log. No third-party
// dependency (ADR-006).
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null) {}
    explicit Json(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Json(double n) : type_(Type::Number), num_(n) {}
    explicit Json(std::wstring s) : type_(Type::String), str_(std::move(s)) {}
    // Without this overload, Json(L"literal") would bind to Json(bool) —
    // pointer→bool is a standard conversion and wins over the wstring ctor.
    explicit Json(const wchar_t* s)
        : type_(Type::String), str_(s ? s : L"") {}

    static Json Array();
    static Json Object();

    Type Kind() const { return type_; }
    bool IsNull()   const { return type_ == Type::Null; }
    bool IsBool()   const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray()  const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool        AsBool(bool def = false) const { return IsBool() ? bool_ : def; }
    double      AsNumber(double def = 0) const { return IsNumber() ? num_ : def; }
    long long   AsInt(long long def = 0) const;
    std::wstring AsString(const std::wstring& def = {}) const {
        return IsString() ? str_ : def;
    }

    // Array access
    size_t            Size() const;
    const Json&       At(size_t i) const;               // bounds-checked, Null on OOB
    void              Push(Json v);
    const std::vector<Json>& Items() const { return arr_; }

    // Object access
    bool        Has(const std::wstring& key) const;
    const Json& Get(const std::wstring& key) const;    // Null member when missing
    void        Set(std::wstring key, Json v);
    const std::map<std::wstring, Json>& Fields() const { return obj_; }

    // Escape and serialize (compact form).
    static std::wstring Escape(const std::wstring& s);
    std::wstring Dump() const;

    // Parse a whole document. Returns false and fills err on failure.
    static bool Parse(const std::wstring& text, Json& out, std::wstring& err);

private:
    class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::wstring str_;
    std::vector<Json> arr_;
    std::map<std::wstring, Json> obj_;
};

} // namespace minisys
