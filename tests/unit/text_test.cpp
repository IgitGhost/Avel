#include <gtest/gtest.h>

#include <string>

#include "core/platform/text.h"

namespace avel::platform {
namespace {

TEST(TextTest, EmptyStringsStayEmpty) {
    EXPECT_EQ(to_utf8(L""), "");
    EXPECT_EQ(to_utf16(""), L"");
}

TEST(TextTest, ArmenianAndCyrillicRoundTrip) {
    const std::wstring original = L"Ավել — Веник";
    EXPECT_EQ(to_utf16(to_utf8(original)), original);
}

TEST(TextTest, EncodesKnownUtf8Bytes) {
    // U+0531 ARMENIAN CAPITAL LETTER AYB is D4 B1 in UTF-8.
    EXPECT_EQ(to_utf8(L"Ա"), "\xD4\xB1");
}

TEST(TextTest, HandlesCharactersOutsideTheBmp) {
    // U+1F9F9 BROOM is a surrogate pair in UTF-16 and four bytes in UTF-8.
    EXPECT_EQ(to_utf8(L"\U0001F9F9"), "\xF0\x9F\xA7\xB9");
    EXPECT_EQ(to_utf16("\xF0\x9F\xA7\xB9"), L"\U0001F9F9");
}

TEST(TextTest, UnpairedSurrogateBecomesReplacementCharacter) {
    const wchar_t broken[] = {L'a', static_cast<wchar_t>(0xD800), L'b'};
    EXPECT_EQ(to_utf8(std::wstring_view(broken, 3)),
              "a\xEF\xBF\xBD"
              "b");
}

TEST(TextTest, InvalidUtf8BecomesReplacementCharacter) {
    EXPECT_EQ(to_utf16("a\xFF"
                       "b"),
              L"a�b");
}

TEST(TextTest, PreservesEmbeddedNulls) {
    const std::wstring with_null(L"a\0b", 3);
    EXPECT_EQ(to_utf8(with_null), std::string("a\0b", 3));
}

TEST(TextTest, CaseInsensitiveComparisons) {
    EXPECT_TRUE(iequals(L"UninstallString", L"uninstallstring"));
    EXPECT_TRUE(iequals(L"Ավել", L"ԱՎԵԼ"));
    EXPECT_TRUE(iequals(L"Веник", L"ВЕНИК"));
    EXPECT_FALSE(iequals(L"abc", L"abcd"));
    EXPECT_TRUE(iequals(L"", L""));

    EXPECT_TRUE(icontains(LR"(C:\Program Files\App)", L"program files"));
    EXPECT_TRUE(icontains(L"abc", L""));
    EXPECT_FALSE(icontains(L"ab", L"abc"));

    EXPECT_TRUE(istarts_with(L"S-1-5-21-123", L"s-1-5-21-"));
    EXPECT_FALSE(istarts_with(L"S-1", L"S-1-5"));
    EXPECT_TRUE(iends_with(L"setup.EXE", L".exe"));
    EXPECT_FALSE(iends_with(L"exe", L".exe"));
}

TEST(TextTest, Trim) {
    EXPECT_EQ(trim(L"  a b \t\r\n"), L"a b");
    EXPECT_EQ(trim(std::wstring_view(L"x\0\0", 3)), L"x");
    EXPECT_EQ(trim(L"   "), L"");
    EXPECT_EQ(trim(L""), L"");
}

}  // namespace
}  // namespace avel::platform
