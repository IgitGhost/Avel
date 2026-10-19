// An independent dump of a registry tree, to compare before and after a change: names (as hex, to
// keep unpaired surrogates), classes, security as SDDL, values sorted by name, subkeys sorted by
// name. Written without the code under test.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <wil/resource.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "core/changes/serialization.h"
#include "core/platform/text.h"

namespace avel::testing {

inline std::string hex_of(const std::wstring& text) {
    return changes::to_hex(reinterpret_cast<const std::byte*>(text.data()), text.size() * sizeof(wchar_t));
}

inline std::string dump_key(HKEY parent, const std::wstring& name, int depth = 0) {
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(parent, name.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &raw) != ERROR_SUCCESS) {
        return "<missing>";
    }
    const wil::unique_hkey key(raw);
    wchar_t class_name[256] = {};
    DWORD class_length = 256;
    DWORD subkeys = 0;
    DWORD values = 0;
    ::RegQueryInfoKeyW(key.get(), class_name, &class_length, nullptr, &subkeys, nullptr, nullptr, &values, nullptr,
                       nullptr, nullptr, nullptr);
    std::string text = std::format("{}key {} class={}", std::string(depth * 2, ' '), hex_of(name),
                                   hex_of(std::wstring(class_name, class_length)));

    std::vector<std::byte> security(4096);
    DWORD size = static_cast<DWORD>(security.size());
    constexpr SECURITY_INFORMATION kParts =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    if (::RegGetKeySecurity(key.get(), kParts, security.data(), &size) == ERROR_SUCCESS) {
        wil::unique_hlocal_string sddl;
        ::ConvertSecurityDescriptorToStringSecurityDescriptorW(security.data(), SDDL_REVISION_1, kParts, &sddl, nullptr);
        text += " sd=" + platform::to_utf8(sddl.get());
    }
    text += "\n";

    std::vector<std::string> lines;
    for (DWORD i = 0; i < values; ++i) {
        wchar_t value_name[512] = {};
        DWORD name_length = 512;
        DWORD type = 0;
        std::vector<std::byte> data(70000);
        DWORD data_size = static_cast<DWORD>(data.size());
        if (::RegEnumValueW(key.get(), i, value_name, &name_length, nullptr, &type,
                            reinterpret_cast<BYTE*>(data.data()), &data_size) == ERROR_SUCCESS) {
            lines.push_back(std::format("{}value {} type={} data={}\n", std::string(depth * 2 + 2, ' '),
                                        hex_of(std::wstring(value_name, name_length)), type,
                                        changes::to_hex(data.data(), data_size)));
        }
    }
    std::sort(lines.begin(), lines.end());
    std::vector<std::wstring> names;
    for (DWORD i = 0; i < subkeys; ++i) {
        wchar_t subkey[256] = {};
        DWORD length = 256;
        if (::RegEnumKeyExW(key.get(), i, subkey, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            names.emplace_back(subkey, length);
        }
    }
    std::sort(names.begin(), names.end());
    for (const auto& line : lines) {
        text += line;
    }
    for (const auto& subkey : names) {
        text += dump_key(key.get(), subkey, depth + 1);
    }
    return text;
}

}  // namespace avel::testing
