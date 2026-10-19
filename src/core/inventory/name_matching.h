// Fuzzy name comparison used to decide whether two entries describe the same program.
//
// Ported from Bulk Crap Uninstaller (https://github.com/BCUninstaller/Bulk-Crap-Uninstaller),
// Copyright (c) 2017 Marcin Szeniak, Apache License 2.0:
//   KlocTools/Tools/Sift4.cs                 (SimplestDistance)
//   KlocTools/Tools/StringTools.cs           (StripStringFromVersionNumber)
//   KlocTools/Extensions/StringExtensions.cs (ExtendedTrimEndAny)
//   UninstallTools/Factory/ApplicationEntryTools.cs (CompareStrings)
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace avel::inventory {

// Sift4 "simplest" edit distance: a fast approximation of Levenshtein distance.
// `max_offset` is how far ahead it looks for matching characters after a mismatch.
int sift4_distance(std::wstring_view s1, std::wstring_view s2, int max_offset);

// Strips trailing version numbers and noise from a program name:
// "7-Zip 23.01 (x64)" -> "7-Zip", "Foo Helper v2" -> "Foo".
std::wstring strip_version_number(std::wstring_view name);

// true = similar, false = different, nullopt = too short to judge (under 5 characters, or 4
// with `relaxed`). `relaxed` also treats one string being a prefix of the other as similar,
// which suits version strings.
std::optional<bool> compare_names(std::wstring_view a, std::wstring_view b, bool relaxed = false);

}  // namespace avel::inventory
