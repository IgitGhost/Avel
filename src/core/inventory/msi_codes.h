// Windows Installer's "packed" GUIDs: the registry spells a product, upgrade or component code
// without braces and dashes, with each group's characters reversed in a fixed way, e.g.
//   {23170F69-40C1-2702-2600-000001000000}  <->  96F071321C0420726200000010000000
// (the first three groups reversed whole, every byte of the last two with its nibbles swapped).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace avel::inventory {

// "{8-4-4-4-12}" in either case -> 32 upper-case hex digits; nullopt for anything else.
std::optional<std::wstring> pack_guid(std::wstring_view guid);

// 32 hex digits -> "{8-4-4-4-12}" in upper case; nullopt for anything else.
std::optional<std::wstring> unpack_guid(std::wstring_view packed);

}  // namespace avel::inventory
