#include <gtest/gtest.h>

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::platform {
namespace {

struct NormalizeCase {
    std::wstring_view input;
    std::wstring_view expected;
};

constexpr NormalizeCase kNormalize[] = {
    {LR"("C:\Program Files\App\")", LR"(C:\Program Files\App)"},
    {LR"(C:/A//B/./C/../D)", LR"(C:\A\B\D)"},
    {LR"(C:)", LR"(C:\)"},
    {LR"(c:\)", LR"(c:\)"},
    {LR"(\\?\C:\A\B)", LR"(C:\A\B)"},
    {LR"(\??\C:\Windows\x.sys)", LR"(C:\Windows\x.sys)"},
    {LR"(\\?\UNC\server\share\x)", LR"(\\server\share\x)"},
    {LR"(\\server\\share\\x\)", LR"(\\server\share\x)"},
    {LR"(C:\..\..\A)", LR"(C:\A)"},
    {LR"(..\A)", LR"(..\A)"},
    {LR"(  "  C:\A  "  )", LR"(C:\A)"},
    {LR"(\Windows\System32)", LR"(\Windows\System32)"},
    {LR"()", LR"()"},
};

TEST(PathsTest, NormalizesPathsWithoutTouchingTheDisk) {
    for (const auto& item : kNormalize) {
        EXPECT_EQ(normalize_path(item.input), item.expected) << to_utf8(item.input);
    }
}

struct UnderCase {
    std::wstring_view path;
    std::wstring_view ancestor;
    bool under;
    bool inside;
};

constexpr UnderCase kUnder[] = {
    {LR"(C:\Foo\Bar)", LR"(C:\Foo)", true, true},
    {LR"(C:\Foobar)", LR"(C:\Foo)", false, false},
    {LR"(C:\Foo)", LR"(C:\Foo)", true, false},
    {LR"(c:\foo\)", LR"(C:\FOO)", true, false},
    {LR"(C:\Foo)", LR"(C:\Foo\Bar)", false, false},
    {LR"(C:\Anything)", LR"(C:\)", true, true},
    {LR"(C:\Anything)", LR"(C:)", true, true},
    {LR"(D:\Foo)", LR"(C:\)", false, false},
    {LR"(\\server\share\x)", LR"(\\server\share)", true, true},
    {LR"(\\server\shared)", LR"(\\server\share)", false, false},
    {LR"(\\?\C:\Foo\Bar)", LR"(C:\Foo)", true, true},
    {LR"(C:\Foo\..\Bar)", LR"(C:\Foo)", false, false},
    {LR"(C:\Foo)", LR"()", false, false},
};

TEST(PathsTest, ContainmentRespectsComponentBoundaries) {
    for (const auto& item : kUnder) {
        const std::string label = to_utf8(item.path) + " / " + to_utf8(item.ancestor);
        EXPECT_EQ(path_is_under(item.path, item.ancestor), item.under) << label;
        EXPECT_EQ(path_is_inside(item.path, item.ancestor), item.inside) << label;
    }
}

TEST(PathsTest, EqualityIgnoresFormAndCase) {
    EXPECT_TRUE(paths_equal(LR"(C:\Program Files\App\)", LR"("c:/program files/app")"));
    EXPECT_FALSE(paths_equal(LR"(C:\App)", LR"(C:\App2)"));
    EXPECT_FALSE(paths_equal(L"", L""));
}

TEST(PathsTest, ParentAndLastComponent) {
    EXPECT_EQ(parent_path(LR"(C:\A\B)"), LR"(C:\A)");
    EXPECT_EQ(parent_path(LR"(C:\A)"), LR"(C:\)");
    EXPECT_EQ(parent_path(LR"(C:\)"), L"");
    EXPECT_EQ(parent_path(LR"(\\server\share\x)"), LR"(\\server\share)");
    EXPECT_EQ(parent_path(L"A"), L"");
    EXPECT_EQ(last_path_component(LR"(C:\A\B\)"), L"B");
    EXPECT_EQ(last_path_component(L"B"), L"B");
}

TEST(PathsTest, LongPathExpandsShortNamesOfExistingPaths) {
    wchar_t windows[MAX_PATH] = {};
    ASSERT_GT(::GetWindowsDirectoryW(windows, MAX_PATH), 0u);
    EXPECT_TRUE(iequals(long_path(windows), normalize_path(windows)));
    EXPECT_EQ(long_path(LR"(C:\Avel.Tests\does\not\exist\)"), LR"(C:\Avel.Tests\does\not\exist)");
}

class ReferencedPathsTest : public ::testing::Test {
protected:
    void SetUp() override {
        wchar_t buffer[MAX_PATH] = {};
        ASSERT_GT(::GetWindowsDirectoryW(buffer, MAX_PATH), 0u);
        windows_ = buffer;
    }

    // Nothing exists, as after an uninstall.
    static std::vector<std::wstring> refs(std::wstring_view command, const Expander& expand = {}) {
        return referenced_paths(command, expand, [](const std::wstring&) { return false; });
    }

    std::wstring windows_;
};

TEST_F(ReferencedPathsTest, QuotedAndUnquotedExecutables) {
    EXPECT_EQ(refs(LR"("C:\Program Files\App\svc.exe" -k run)"),
              std::vector<std::wstring>{LR"(C:\Program Files\App\svc.exe)"});
    EXPECT_EQ(refs(LR"(C:\Program Files\App\svc.exe -k run)"),
              std::vector<std::wstring>{LR"(C:\Program Files\App\svc.exe)"});
    EXPECT_TRUE(refs(L"   ").empty());
}

TEST_F(ReferencedPathsTest, ServiceImagePathForms) {
    EXPECT_EQ(refs(LR"(\SystemRoot\System32\drivers\x.sys)"),
              std::vector<std::wstring>{windows_ + LR"(\System32\drivers\x.sys)"});
    EXPECT_EQ(refs(LR"(System32\drivers\x.sys)"),
              std::vector<std::wstring>{windows_ + LR"(\System32\drivers\x.sys)"});
    EXPECT_EQ(refs(LR"(\??\C:\Tools\x.sys)"), std::vector<std::wstring>{LR"(C:\Tools\x.sys)"});
}

TEST_F(ReferencedPathsTest, Rundll32ReferencesItsDll) {
    EXPECT_EQ(refs(LR"(rundll32.exe "C:\Program Files\App\x.dll",Entry)"),
              (std::vector<std::wstring>{windows_ + LR"(\System32\rundll32.exe)", LR"(C:\Program Files\App\x.dll)"}));
    EXPECT_EQ(refs(LR"(rundll32 C:\App\x.dll,Entry arg)"),
              (std::vector<std::wstring>{windows_ + LR"(\System32\rundll32)", LR"(C:\App\x.dll)"}));
}

TEST_F(ReferencedPathsTest, VariablesUseTheGivenExpander) {
    const Expander expand = [](std::wstring_view text) {
        std::wstring result(text);
        const std::wstring token = L"%ProgramFiles%";
        if (const auto at = result.find(token); at != std::wstring::npos) {
            result.replace(at, token.size(), LR"(C:\PF)");
        }
        return result;
    };
    EXPECT_EQ(refs(LR"(%ProgramFiles%\App\a.exe /min)", expand), std::vector<std::wstring>{LR"(C:\PF\App\a.exe)"});
}

}  // namespace
}  // namespace avel::platform
