#pragma once

#include <string>
#include <string_view>

namespace avel::platform {

// Converts UTF-16 (the native Windows encoding) to UTF-8.
// Unpaired surrogates, which do occur in real-world registry data, become U+FFFD instead of
// failing the conversion.
std::string to_utf8(std::wstring_view text);

// Converts UTF-8 to UTF-16. Invalid byte sequences become U+FFFD.
std::wstring to_utf16(std::string_view text);

// Case-insensitive ordinal comparisons: culture-independent, the same rules Windows applies to
// file names and registry keys.
bool iequals(std::wstring_view a, std::wstring_view b);
bool icontains(std::wstring_view text, std::wstring_view part);
bool istarts_with(std::wstring_view text, std::wstring_view prefix);
bool iends_with(std::wstring_view text, std::wstring_view suffix);

// Removes leading and trailing whitespace (and stray NULs, which registry strings sometimes carry).
std::wstring_view trim(std::wstring_view text);

// Lowercase using culture-independent rules, like .NET's ToLowerInvariant.
std::wstring to_lower(std::wstring_view text);

}  // namespace avel::platform
