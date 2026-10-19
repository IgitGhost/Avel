// Expected values for match_product_name, item_name_equals_company_name, trim_publisher and
// name_without_extension were produced by running Bulk Crap Uninstaller's original C# code
// (MatchStringToProductName, ItemNameEqualsCompanyName, PublisherTrimmed and .NET's
// Path.GetFileNameWithoutExtension; copied verbatim into a throwaway harness) on the same inputs.
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "core/inventory/name_matching.h"
#include "core/leftovers/confidence.h"
#include "core/leftovers/product_name.h"
#include "core/platform/text.h"

namespace avel::leftovers {
namespace {

struct MatchCase {
    std::wstring_view display_name;
    std::wstring_view publisher;
    std::wstring_view item;
    int match;
    bool equals_company;
};

constexpr MatchCase kMatches[] = {
    {LR"(Notepad++ (64-bit x64))", LR"(Notepad++ Team)", LR"(Notepad++)", 0, true},
    {LR"(7-Zip 26.00 (x64 edition))", LR"(Igor Pavlov)", LR"(7-Zip)", 0, false},
    {LR"(7-Zip 26.00 (x64 edition))", LR"(Igor Pavlov)", LR"(7-Zip 26)", 2, false},
    {LR"(Mozilla Firefox (x64 en-US))", LR"(Mozilla)", LR"(Mozilla Firefox)", 0, false},
    {LR"(Mozilla Firefox (x64 en-US))", LR"(Mozilla)", LR"(Firefox)", 0, false},
    {LR"(Mozilla Firefox (x64 en-US))", LR"(Mozilla)", LR"(Mozilla)", 2, true},
    {LR"(Adobe AIR)", LR"(Adobe Systems Incorporated)", LR"(Adobe)", 2, true},
    {LR"(Inno Setup version 7.1.0)", LR"(jrsoftware.org)", LR"(Inno Setup 7)", -1, false},
    {LR"(Microsoft Visual C++ 2015-2022 Redistributable (x64) - 14.44.35211)", LR"(Microsoft Corporation)",
     LR"(Microsoft)", 2, true},
    {LR"(VLC media player)", LR"(VideoLAN)", LR"(VideoLAN)", -1, true},
    {LR"(VLC media player)", LR"(VideoLAN)", LR"(vlc)", -1, false},
    {LR"(Visual Studio Code)", LR"(Microsoft Corporation)", LR"(Code)", -1, false},
    {LR"(Visual Studio Code)", LR"(Microsoft Corporation)", LR"(Microsoft VS Code)", -1, false},
    {LR"(JetBrains Toolbox)", LR"(JetBrains s.r.o.)", LR"(JetBrains)", 2, true},
    {LR"(JetBrains Toolbox)", LR"(JetBrains s.r.o.)", LR"(Toolbox)", 2, false},
    {LR"(Python 3.12.1 (64-bit))", LR"(Python Software Foundation)", LR"(Python312)", 2, false},
    {LR"(Avel Test App)", LR"(Avel)", LR"(Avel_Test_App)", 0, false},
    {LR"(Contoso Fixture 1.0)", LR"(Contoso Ltd)", LR"(Contoso Fixture)", 0, false},
    {LR"(Contoso Fixture 1.0)", LR"(Contoso Ltd)", LR"(Fixture)", 2, false},
    {LR"(Contoso Fixture 1.0)", LR"(Contoso Ltd)", LR"(ContosoFixtureData)", -1, false},
    {LR"(Contoso Fixture 1.0)", LR"(Contoso Ltd)", LR"(Contoso)", 2, true},
    {LR"(Total Commander 64-bit (Remove or Repair))", LR"(Ghisler Software GmbH)", LR"(totalcmd)", -1, false},
    {LR"(Discord)", LR"(Discord Inc.)", LR"(Discord)", 0, false},
    {LR"(Discord)", LR"(Discord Inc.)", LR"(DiscordPTB)", -1, false},
    {LR"(Spotify)", LR"(Spotify AB)", LR"(Spotify)", 0, true},
    {LR"(Google Chrome)", LR"(Google LLC)", LR"(Chrome)", 2, false},
    {LR"(Google Chrome)", LR"(Google LLC)", LR"(Google)", 2, true},
    {LR"(NVIDIA Graphics Driver 555.85)", LR"(NVIDIA Corporation)", LR"(NVIDIA Corporation)", -1, false},
    {LR"(Mozilla Maintenance Service)", LR"(Mozilla Corporation)", LR"(Maintenance Service)", 0, false},
    {LR"(Contoso Studio Pro)", LR"(Contoso Studio Pro Inc)", LR"(Studio)", -1, false},
};

TEST(ProductNameTest, MatchesLikeBcu) {
    for (const auto& item : kMatches) {
        const ProductNames names = make_product_names(item.display_name, item.publisher);
        const std::string label = platform::to_utf8(item.display_name) + " / " + platform::to_utf8(item.item);
        EXPECT_EQ(match_product_name(names, item.item), item.match) << label;
        EXPECT_EQ(item_name_equals_company_name(names, item.item), item.equals_company) << label;
    }
}

struct TrimCase {
    std::wstring_view input;
    std::wstring_view expected;
};

constexpr TrimCase kPublishers[] = {
    {LR"(Microsoft Corporation)", LR"(Microsoft)"},
    {LR"(Adobe Systems Incorporated)", LR"(Adobe Systems)"},
    {LR"(Discord Inc.)", LR"(Discord)"},
    {LR"(Oracle(R) Corp.)", LR"(Oracle)"},
    {LR"(Viacorp)", LR"(Via)"},
    {LR"(Contoso Ltd)", LR"(Contoso Ltd)"},
    {LR"(Spotify AB)", LR"(Spotify AB)"},
    {LR"(Igor Pavlov)", LR"(Igor Pavlov)"},
    {LR"(Notepad++ Team)", LR"(Notepad++ Team)"},
    {LR"(ACME Limited, Inc.)", LR"(ACME)"},
    {LR"()", LR"()"},
};

TEST(ProductNameTest, TrimsPublishersLikeBcu) {
    for (const auto& item : kPublishers) {
        EXPECT_EQ(inventory::trim_publisher(item.input), item.expected) << platform::to_utf8(item.input);
    }
}

constexpr TrimCase kExtensions[] = {
    {LR"(7-Zip 26.00)", LR"(7-Zip 26)"}, {LR"(Notepad++)", LR"(Notepad++)"}, {LR"(.hidden)", LR"()"},
    {LR"(App.v2.data)", LR"(App.v2)"},   {LR"(Folder.)", LR"(Folder)"},
};

TEST(ProductNameTest, NameWithoutExtensionLikeDotNet) {
    for (const auto& item : kExtensions) {
        EXPECT_EQ(name_without_extension(item.input), item.expected) << platform::to_utf8(item.input);
    }
}

TEST(ProductNameTest, NameConfidenceCombinesTheEvidence) {
    // %APPDATA%\Notepad++: a perfect match at the top level, but the publisher "Notepad++ Team"
    // contains the folder name, and the folder sits directly in a known folder.
    const auto notepad = make_product_names(L"Notepad++ (64-bit x64)", L"Notepad++ Team");
    const auto records = name_confidence(notepad, L"Notepad++", L"Roaming", 0, true);
    EXPECT_EQ(score(records), 2 + 2 - 2 - 1);
    EXPECT_EQ(level(records), Confidence::Questionable);

    // Program Files\Contoso\Fixture: a weaker match one level down, inside the publisher's folder.
    const auto contoso = make_product_names(L"Contoso Fixture 1.0", L"Contoso Ltd");
    const auto nested = name_confidence(contoso, L"Fixture", L"Contoso", 1, false);
    EXPECT_EQ(score(nested), -2 + 0 + 4);
    EXPECT_EQ(level(nested), Confidence::Good);

    EXPECT_TRUE(name_confidence(contoso, L"Unrelated Vendor", L"Programs", 0, true).empty());
}

TEST(ConfidenceTest, PointsAndLevels) {
    EXPECT_EQ(level({}), Confidence::Unknown);
    const std::vector<Record> uninstaller{{Evidence::UninstallerKey}};
    EXPECT_EQ(level(uninstaller), Confidence::VeryGood);
    const std::vector<Record> still_used{{Evidence::ExplicitConnection}, {Evidence::DirectoryStillUsed}};
    EXPECT_EQ(score(still_used), -3);
    EXPECT_EQ(level(still_used), Confidence::Bad);
    const std::vector<Record> one{{Evidence::Depth, 1}, {Evidence::ProductNamePerfectMatch}, {Evidence::DirectlyInsideKnownFolder}};
    EXPECT_EQ(score(one), 1);
    EXPECT_EQ(level(one), Confidence::Questionable);
    const std::vector<Record> deep{{Evidence::Depth, 2}};
    EXPECT_EQ(score(deep), -2);
}

TEST(ConfidenceTest, AddRecordCountsEachPieceOfEvidenceOnce) {
    std::vector<Record> records;
    add_record(records, {Evidence::ExplicitConnection});
    add_record(records, {Evidence::ExplicitConnection, 0, ReferenceKind::None, L"again"});
    add_record(records, {Evidence::PathReference, 0, ReferenceKind::ShortcutTarget});
    add_record(records, {Evidence::PathReference, 0, ReferenceKind::RunValue});
    add_record(records, {Evidence::Depth, 0});
    add_record(records, {Evidence::Depth, 1});
    EXPECT_EQ(records.size(), 5u);
}

}  // namespace
}  // namespace avel::leftovers
