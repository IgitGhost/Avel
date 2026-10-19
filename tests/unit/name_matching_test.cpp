// Expected values were produced by running Bulk Crap Uninstaller's original C# code (Sift4,
// StripStringFromVersionNumber, CompareStrings; copied verbatim into a throwaway harness) on the
// same inputs, so these tests check that the C++ port behaves exactly like the original.
#include <gtest/gtest.h>

#include <optional>
#include <string_view>

#include "core/inventory/name_matching.h"

namespace avel::inventory {
namespace {

struct PairCase {
    std::wstring_view a;
    std::wstring_view b;
    int distance;
    std::optional<bool> similar;
    std::optional<bool> similar_relaxed;
};

constexpr PairCase kPairs[] = {
    {LR"(kitten)", LR"(sitting)", 3, false, false},
    {LR"(Firefox)", LR"(FireFox)", 1, false, false},
    {LR"(Microsoft Edge)", LR"(Microsoft Edge WebView2 Runtime)", 17, false, true},
    {LR"()", LR"(abc)", 3, std::nullopt, std::nullopt},
    {LR"(abc)", LR"()", 3, std::nullopt, std::nullopt},
    {LR"(abcdef)", LR"(abcdef)", 0, true, true},
    {LR"(abcdef)", LR"(fedcba)", 5, false, false},
    {LR"(Mozilla Firefox)", LR"(Mozilla Thunderbird)", 10, false, false},
    {LR"(7-Zip)", LR"(7-Zip 23.01)", 6, false, true},
    {L"Ավել", L"Ավելորդ", 3, std::nullopt, true},
    {LR"(ab)", LR"(ba)", 1, std::nullopt, std::nullopt},
    {LR"(Notepad++)", LR"(Notepad++ (64-bit x64))", 13, false, true},
    {LR"(Visual Studio Code)", LR"(Visual Studio Community 2022)", 12, false, false},
    {LR"(Google Chrome)", LR"(Google Chrome Beta)", 5, false, true},
    {LR"(abcdefghijkl)", LR"(abcxefghijkl)", 1, true, true},
    {LR"(Python 3.12.1)", LR"(Python 3.11.7)", 2, false, false},
    {LR"(JetBrains Rider)", LR"(JetBrains Toolbox)", 7, false, false},
    {LR"(1.2.3)", LR"(1.2.3.4)", 2, false, true},
    {LR"(23.01)", LR"(23.1)", 2, std::nullopt, false},
    {LR"(10.0)", LR"(10.0)", 0, std::nullopt, true},
    {LR"(2.5)", LR"(3.5)", 1, std::nullopt, std::nullopt},
    {LR"(1.0.0.0)", LR"(2.0.0.0)", 1, false, false},
};

struct StripCase {
    std::wstring_view input;
    std::wstring_view expected;
};

constexpr StripCase kStrips[] = {
    {LR"(7-Zip 23.01 (x64))", LR"(7-Zip)"},
    {LR"(Foo Helper)", LR"(Foo)"},
    {LR"(Bar Application 2.0)", LR"(Bar)"},
    {LR"(Notepad++)", LR"(Notepad++)"},
    {LR"(Java 8 Update 301 (64-bit))", LR"(Java 8 Update)"},
    {LR"(Microsoft Visual C++ 2015-2022 Redistributable (x64) - 14.38.33130)",
     LR"(Microsoft Visual C++ 2015-2022 Redistributable)"},
    {LR"(VLC media player)", LR"(VLC media player)"},
    {LR"(Mozilla Firefox (x64 en-US))", LR"(Mozilla Firefox)"},
    {LR"(GIMP 2.10.36)", LR"(GIMP)"},
    {LR"(Steam)", LR"(Steam)"},
    {LR"(Ab (x))", LR"(Ab (x))"},
    {LR"(v1.2)", LR"(v)"},
    {LR"(Python 3.12.1 (64-bit))", LR"(Python)"},
    {LR"(WinRAR 6.24 (64-bit))", LR"(WinRAR)"},
    {LR"(Paint.NET v5.0)", LR"(Paint.NET)"},
    {LR"(Discord CE)", LR"(Discord)"},
    {LR"()", LR"()"},
    {LR"(   )", LR"()"},
    {LR"(1.2.3)", LR"()"},
    {LR"(Name_)", LR"(Name)"},
    {LR"(Some App - 1.0)", LR"(Some App)"},
    {LR"(Tool v)", LR"(Tool)"},
    {LR"(Helper)", LR"()"},
    {LR"(My App, 2.0.)", LR"(My App)"},
    {L"Ավել 1.0", L"Ավել"},
};

TEST(NameMatchingTest, Sift4MatchesBcu) {
    for (const auto& c : kPairs) {
        EXPECT_EQ(sift4_distance(c.a, c.b, 3), c.distance)
            << std::wstring(c.a) << L" / " << std::wstring(c.b);
    }
}

TEST(NameMatchingTest, CompareNamesMatchesBcu) {
    for (const auto& c : kPairs) {
        EXPECT_EQ(compare_names(c.a, c.b), c.similar) << std::wstring(c.a) << L" / " << std::wstring(c.b);
        EXPECT_EQ(compare_names(c.a, c.b, true), c.similar_relaxed)
            << std::wstring(c.a) << L" / " << std::wstring(c.b);
    }
}

TEST(NameMatchingTest, StripVersionNumberMatchesBcu) {
    for (const auto& c : kStrips) {
        EXPECT_EQ(strip_version_number(c.input), c.expected) << L"input: [" << std::wstring(c.input) << L"]";
    }
}

}  // namespace
}  // namespace avel::inventory
