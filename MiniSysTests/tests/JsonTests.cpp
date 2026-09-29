#include <gtest/gtest.h>

#include "util/Json.h"

namespace minisys {
namespace {

TEST(JsonTests, ParseObjectRoundTrip) {
    Json j;
    std::wstring err;
    ASSERT_TRUE(Json::Parse(L"{\"a\":1,\"b\":\"x\",\"c\":true,\"d\":null}", j, err)) << err;
    EXPECT_TRUE(j.IsObject());
    EXPECT_EQ(j.Get(L"a").AsInt(), 1);
    EXPECT_EQ(j.Get(L"b").AsString(), L"x");
    EXPECT_TRUE(j.Get(L"c").AsBool());
    EXPECT_TRUE(j.Get(L"d").IsNull());
}

TEST(JsonTests, ParseNestedArraysAndEscapes) {
    Json j;
    std::wstring err;
    ASSERT_TRUE(Json::Parse(
        L"{\"list\":[1,2,{\"k\":\"v\\\"q\\\"\"}],\"path\":\"C:\\\\Temp\\\\x\"}", j, err)) << err;
    EXPECT_TRUE(j.Get(L"list").IsArray());
    ASSERT_EQ(j.Get(L"list").Size(), 3u);
    EXPECT_EQ(j.Get(L"list").At(2).Get(L"k").AsString(), L"v\"q\"");
    EXPECT_EQ(j.Get(L"path").AsString(), L"C:\\Temp\\x");
}

TEST(JsonTests, ParseUnicodeEscape) {
    Json j;
    std::wstring err;
    ASSERT_TRUE(Json::Parse(L"{\"s\":\"\\u4e2d\\u6587\"}", j, err)) << err;
    EXPECT_EQ(j.Get(L"s").AsString(), L"中文");
}

TEST(JsonTests, RejectMalformed) {
    struct Case { const wchar_t* text; };
    const Case cases[] = {
        {L"{\"a\":}"},          // missing value
        {L"[1,2"},             // unterminated array
        {L"{\"a\":1,}"},        // trailing comma
        {L"{a:1}"},             // unquoted key
        {L"{\"a\":1} junk"},    // trailing content
        {L""},                  // empty
    };
    for (const auto& c : cases) {
        Json j;
        std::wstring err;
        EXPECT_FALSE(Json::Parse(c.text, j, err)) << "expected failure for: " << c.text;
    }
}

TEST(JsonTests, DumpRoundTrip) {
    Json j = Json::Object();
    j.Set(L"name", Json(L"测试 \"quoted\""));
    j.Set(L"size", Json(static_cast<double>(12345)));
    j.Set(L"ok", Json(true));
    Json arr = Json::Array();
    arr.Push(Json(L"a"));
    arr.Push(Json(static_cast<double>(2)));
    j.Set(L"items", arr);

    Json back;
    std::wstring err;
    ASSERT_TRUE(Json::Parse(j.Dump(), back, err)) << err;
    EXPECT_EQ(back.Get(L"name").AsString(), L"测试 \"quoted\"");
    EXPECT_EQ(back.Get(L"size").AsInt(), 12345);
    EXPECT_TRUE(back.Get(L"ok").AsBool());
    EXPECT_EQ(back.Get(L"items").Size(), 2u);
}

TEST(JsonTests, MissingKeyReturnsNull) {
    Json j;
    std::wstring err;
    ASSERT_TRUE(Json::Parse(L"{}", j, err));
    EXPECT_TRUE(j.Get(L"missing").IsNull());
    EXPECT_EQ(j.Get(L"missing").AsString(L"def"), L"def");
}

} // namespace
} // namespace minisys
