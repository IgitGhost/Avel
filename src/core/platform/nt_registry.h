// The registry through ntdll's native API, for what the Win32 functions can't do exactly or fast
// enough: values byte for byte as stored, and key names with their times in one pass over a tree.
// Unlike RegistryKey, these fail on any error instead of returning what they could read, so a
// partial result never passes for a complete one.
//
// They take keys opened with RegOpenKeyExW or RegCreateKeyExW, not predefined handles such as
// HKEY_LOCAL_MACHINE, which only the Win32 functions understand.
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

// A value exactly as stored: RegEnumValueW and RegQueryValueExW may add a terminator to a string
// stored without one and count it, depending on what follows in the caller's buffer, and
// RegSetValueExW stretches a string by a terminator when one happens to follow it in memory.
struct RawValue {
    std::wstring name;  // Empty for the default value; may hold unpaired surrogates
    DWORD type = REG_NONE;
    std::vector<std::byte> data;
};

// Appends every value of `key` to `values` (on an error, the ones read before it).
LSTATUS read_raw_values(HKEY key, std::vector<RawValue>& values);

// Reads one value; `value.name` is set to `name` whatever happens. ERROR_FILE_NOT_FOUND if the key
// has no such value.
LSTATUS query_raw_value(HKEY key, std::wstring_view name, RawValue& value);

// Creates or overwrites `value.name` with exactly `value.data`.
LSTATUS set_raw_value(HKEY key, const RawValue& value);

// The key is a registry symbolic link (a SymbolicLinkValue of type REG_LINK). Open keys with
// REG_OPTION_OPEN_LINK to see the link rather than its target.
bool is_registry_link(HKEY key);

// The key's name in the kernel's namespace (\REGISTRY\MACHINE\SOFTWARE\...), whichever path
// opened it: two paths to a key that the 32- and 64-bit views share give the same name.
// Empty if it can't be read.
std::wstring kernel_name(HKEY key);

struct KeyInfo {
    FILETIME last_write{};
    DWORD subkeys = 0;
    DWORD values = 0;
    DWORD max_name_length = 0;        // Longest subkey name, in characters
    DWORD max_value_name_length = 0;  // Longest value name, in characters
    DWORD max_value_data_size = 0;    // Largest value, in bytes
};

// The key's counts and last write time, as RegQueryInfoKeyW reports them, in one call.
LSTATUS query_key_info(HKEY key, KeyInfo& info);

struct SubkeyInfo {
    std::wstring name;
    FILETIME last_write{};
};

// Appends every subkey of `key` to `subkeys`, each with its last write time, in a single call per
// subkey (on an error, the ones read before it).
LSTATUS enum_subkeys(HKEY key, std::vector<SubkeyInfo>& subkeys);

}  // namespace avel::platform
