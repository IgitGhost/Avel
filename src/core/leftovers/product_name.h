// Matching folder and key names to a program's name.
//
// Ported from Bulk Crap Uninstaller (https://github.com/BCUninstaller/Bulk-Crap-Uninstaller),
// Copyright (c) 2017 Marcin Szeniak, Apache License 2.0:
//   UninstallTools/Junk/Confidence/ConfidenceGenerators.cs
//     (MatchStringToProductName, ItemNameEqualsCompanyName, GenerateConfidence)
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/leftovers/confidence.h"

namespace avel::leftovers {

// A program's names as the matching uses them.
struct ProductNames {
    std::wstring name;       // Display name without version or noise (inventory::strip_version_number)
    std::wstring publisher;  // Publisher without company suffixes (inventory::trim_publisher)
};

ProductNames make_product_names(std::wstring_view display_name, std::wstring_view publisher);

// How well `item` (a folder or key name) matches the program's name: -1 = no match, 0 or 1 =
// perfect, higher = weaker. Names of 4 characters or fewer never match.
int match_product_name(const ProductNames& names, std::wstring_view item);

// `item` looks like the publisher's whole folder rather than the program's, e.g. "Adobe" for
// Adobe AIR.
bool item_name_equals_company_name(const ProductNames& names, std::wstring_view item);

// The evidence a name match gives for an item found `level` levels below a search root (0 = a
// direct child). `parent_name` is the name of the folder or key that contains the item.
// Empty when the name doesn't match.
std::vector<Record> name_confidence(const ProductNames& names, std::wstring_view item, std::wstring_view parent_name,
                                    int level, bool parent_is_known_folder);

// The name without its last ".extension", as .NET's Path.GetFileNameWithoutExtension does; BCU
// matches folder names this way, so "7-Zip 26.00" is compared as "7-Zip 26".
std::wstring_view name_without_extension(std::wstring_view name);

}  // namespace avel::leftovers
