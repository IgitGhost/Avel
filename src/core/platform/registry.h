#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

struct RegistryValue {
    std::wstring name;  // Empty for the key's default value
    DWORD type = REG_NONE;
    std::vector<std::byte> data;

    // REG_SZ, REG_EXPAND_SZ (not expanded) or the first string of a REG_MULTI_SZ.
    // Tolerates missing terminators and odd byte counts, both common in real-world data.
    std::optional<std::wstring> as_string() const;

    // REG_DWORD or REG_QWORD, or a REG_SZ holding a decimal number (some installers do that).
    std::optional<std::uint64_t> as_integer() const;
};

// Finds a value by name, ignoring case like the registry itself does.
const RegistryValue* find_value(const std::vector<RegistryValue>& values, std::wstring_view name);

class RegistryKey {
public:
    RegistryKey() = default;
    explicit RegistryKey(wil::unique_hkey key) : key_(std::move(key)) {}

    // Opens parent\path. Returns ERROR_SUCCESS, ERROR_FILE_NOT_FOUND or another Win32 error code.
    static LSTATUS open(HKEY parent, std::wstring_view path, REGSAM access, RegistryKey& result);

    HKEY get() const { return key_.get(); }
    explicit operator bool() const { return static_cast<bool>(key_); }

    std::vector<std::wstring> subkey_names() const;
    // Reads every value in one pass, sizing buffers up front.
    std::vector<RegistryValue> values() const;
    std::optional<FILETIME> last_write_time() const;

private:
    wil::unique_hkey key_;
};

}  // namespace avel::platform
