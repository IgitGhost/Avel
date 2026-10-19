#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/leftovers/merge.h"

namespace avel::leftovers {
namespace {

using platform::Hive;

Leftover file_item(std::wstring path, bool directory, std::vector<Record> records, std::wstring owner,
                   Scanner scanner = Scanner::CommonDrive) {
    Leftover item;
    item.location = FileLocation{std::move(path), directory};
    item.records = std::move(records);
    item.owner_ids = {std::move(owner)};
    item.found_by = scanner;
    item.category = Category::Files;
    return item;
}

Leftover key_item(std::wstring path, std::wstring owner) {
    Leftover item;
    item.location = RegistryKeyLocation{{Hive::Machine, L"", std::move(path)}};
    item.records = {{Evidence::ProductNamePerfectMatch}};
    item.owner_ids = {std::move(owner)};
    item.category = Category::Registry;
    return item;
}

TEST(MergeTest, DuplicatesShareTheirEvidenceOnce) {
    std::vector<Leftover> items;
    items.push_back(file_item(LR"(C:\Program Files\Contoso\)", true, {{Evidence::ExplicitConnection}}, L"reg:A",
                              Scanner::InstallLocation));
    items.push_back(file_item(LR"(c:\program files\contoso)", true,
                              {{Evidence::ProductNamePerfectMatch}, {Evidence::ExplicitConnection}}, L"reg:A"));
    items.push_back(file_item(LR"(C:\Program Files\Contoso)", true, {{Evidence::Depth, 0}}, L"reg:B"));
    merge_duplicates(items);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].found_by, Scanner::InstallLocation);
    EXPECT_EQ(items[0].records.size(), 3u);
    EXPECT_EQ(score(items[0].records), 4 + 2 + 2);
    EXPECT_EQ(items[0].owner_ids, (std::vector<std::wstring>{L"reg:A", L"reg:B"}));
}

TEST(MergeTest, ItemsPointAtTheirNearestContainer) {
    std::vector<Leftover> items;
    items.push_back(file_item(LR"(C:\Data\Contoso\Cache\x.tmp)", false, {}, L"reg:A"));
    items.push_back(file_item(LR"(C:\Data\Contoso)", true, {}, L"reg:A"));
    items.push_back(file_item(LR"(C:\Data\Contoso\Cache)", true, {}, L"reg:A"));
    items.push_back(file_item(LR"(C:\Data\Contoso2)", true, {}, L"reg:A"));
    items.push_back(key_item(LR"(SOFTWARE\Contoso\App)", L"reg:A"));
    items.push_back(key_item(LR"(SOFTWARE\Contoso)", L"reg:A"));
    link_containment(items);

    ASSERT_EQ(items.size(), 6u);
    const auto path = [&](size_t i) { return std::get<FileLocation>(items[i].location).path; };
    // Files first (by category), each sorted by location.
    EXPECT_EQ(path(0), LR"(C:\Data\Contoso)");
    EXPECT_EQ(path(1), LR"(C:\Data\Contoso\Cache)");
    EXPECT_EQ(path(2), LR"(C:\Data\Contoso\Cache\x.tmp)");
    EXPECT_EQ(path(3), LR"(C:\Data\Contoso2)");
    EXPECT_FALSE(items[0].inside.has_value());
    EXPECT_EQ(items[1].inside, 0u);
    EXPECT_EQ(items[2].inside, 1u);
    EXPECT_FALSE(items[3].inside.has_value());
    EXPECT_EQ(std::get<RegistryKeyLocation>(items[4].location).key.path, LR"(SOFTWARE\Contoso)");
    EXPECT_EQ(items[5].inside, 4u);
}

TEST(MergeTest, ContainmentAndIdentity) {
    const Location key{RegistryKeyLocation{{Hive::User, L"S-1", LR"(Software\Contoso)"}}};
    const Location value{RegistryValueLocation{{Hive::User, L"S-1", LR"(Software\Contoso\App)"}, L"Path"}};
    const Location other_user{RegistryValueLocation{{Hive::User, L"S-2", LR"(Software\Contoso\App)"}, L"Path"}};
    EXPECT_TRUE(contains(key, value));
    EXPECT_FALSE(contains(key, other_user));
    EXPECT_FALSE(contains(key, key));
    EXPECT_EQ(identity(key), LR"(key:hku\s-1\software\contoso)");
    EXPECT_EQ(owner_sid(value), L"S-1");
    // A file is not a folder: nothing is inside it.
    EXPECT_FALSE(contains(FileLocation{LR"(C:\a)", false}, FileLocation{LR"(C:\a\b)", false}));
}

}  // namespace
}  // namespace avel::leftovers
