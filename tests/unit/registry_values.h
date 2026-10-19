// Helpers for building registry values in tests.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "core/platform/registry.h"

namespace avel::testing {

inline platform::RegistryValue string_value(std::wstring name, const std::wstring& text,
                                            DWORD type = REG_SZ) {
    platform::RegistryValue value;
    value.name = std::move(name);
    value.type = type;
    value.data.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), text.c_str(), value.data.size());
    return value;
}

inline platform::RegistryValue dword_value(std::wstring name, std::uint32_t number) {
    platform::RegistryValue value;
    value.name = std::move(name);
    value.type = REG_DWORD;
    value.data.resize(sizeof(number));
    std::memcpy(value.data.data(), &number, sizeof(number));
    return value;
}

}  // namespace avel::testing
