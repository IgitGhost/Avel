#include "core/platform/registry.h"

#include <cstring>

#include "core/platform/text.h"

namespace avel::platform {

std::optional<std::wstring> RegistryValue::as_string() const {
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
        return std::nullopt;
    }
    std::wstring text(data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), data.data(), text.size() * sizeof(wchar_t));
    // Stop at the first terminator; for REG_MULTI_SZ that yields the first string.
    if (const auto nul = text.find(L'\0'); nul != std::wstring::npos) {
        text.resize(nul);
    }
    return text;
}

std::optional<std::uint64_t> RegistryValue::as_integer() const {
    switch (type) {
        case REG_DWORD:
            if (data.size() >= sizeof(std::uint32_t)) {
                std::uint32_t value = 0;
                std::memcpy(&value, data.data(), sizeof(value));
                return value;
            }
            return std::nullopt;
        case REG_QWORD:
            if (data.size() >= sizeof(std::uint64_t)) {
                std::uint64_t value = 0;
                std::memcpy(&value, data.data(), sizeof(value));
                return value;
            }
            return std::nullopt;
        case REG_SZ:
        case REG_EXPAND_SZ: {
            const auto text = as_string();
            const std::wstring_view digits = trim(*text);
            if (digits.empty() || digits.size() > 19) {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            for (const wchar_t c : digits) {
                if (c < L'0' || c > L'9') {
                    return std::nullopt;
                }
                value = value * 10 + static_cast<std::uint64_t>(c - L'0');
            }
            return value;
        }
        default:
            return std::nullopt;
    }
}

const RegistryValue* find_value(const std::vector<RegistryValue>& values, std::wstring_view name) {
    for (const auto& value : values) {
        if (iequals(value.name, name)) {
            return &value;
        }
    }
    return nullptr;
}

LSTATUS RegistryKey::open(HKEY parent, std::wstring_view path, REGSAM access, RegistryKey& result) {
    const std::wstring path_z(path);
    HKEY key = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(parent, path_z.c_str(), 0, access, &key);
    if (status == ERROR_SUCCESS) {
        result = RegistryKey(wil::unique_hkey(key));
    }
    return status;
}

std::wstring RegistryKey::kernel_name() const {
    // NtQueryKey(KeyNameInformation) fills a KEY_NAME_INFORMATION: a byte length, then the name.
    using NtQueryKeyFunction = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryKeyFunction>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryKey"));
    constexpr int kKeyNameInformation = 3;
    if (!query || !key_) {
        return {};
    }
    std::vector<std::byte> buffer(1024);
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG needed = 0;
        const LONG status =
            query(key_.get(), kKeyNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
        if (status >= 0) {
            ULONG length = 0;
            std::memcpy(&length, buffer.data(), sizeof(length));
            return std::wstring(reinterpret_cast<const wchar_t*>(buffer.data() + sizeof(ULONG)),
                                length / sizeof(wchar_t));
        }
        if (needed <= buffer.size()) {
            return {};
        }
        buffer.resize(needed);
    }
    return {};
}

std::vector<std::wstring> RegistryKey::subkey_names() const {
    std::vector<std::wstring> names;
    DWORD count = 0;
    DWORD max_name_length = 0;
    if (::RegQueryInfoKeyW(key_.get(), nullptr, nullptr, nullptr, &count, &max_name_length, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return names;
    }
    names.reserve(count);
    std::wstring buffer(max_name_length + 1, L'\0');
    DWORD index = 0;
    while (true) {
        DWORD length = static_cast<DWORD>(buffer.size());
        const LSTATUS status =
            ::RegEnumKeyExW(key_.get(), index, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_MORE_DATA) {
            // A longer subkey appeared after RegQueryInfoKeyW; grow and retry the same index.
            buffer.resize(buffer.size() * 2);
            continue;
        }
        if (status != ERROR_SUCCESS) {
            break;  // ERROR_NO_MORE_ITEMS, or the key went away
        }
        names.emplace_back(buffer.data(), length);
        ++index;
    }
    return names;
}

std::vector<RegistryValue> RegistryKey::values() const {
    std::vector<RegistryValue> result;
    DWORD count = 0;
    DWORD max_name_length = 0;
    DWORD max_data_size = 0;
    if (::RegQueryInfoKeyW(key_.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count,
                           &max_name_length, &max_data_size, nullptr, nullptr) != ERROR_SUCCESS) {
        return result;
    }
    result.reserve(count);
    std::wstring name(max_name_length + 1, L'\0');
    std::vector<std::byte> data(max_data_size);
    DWORD index = 0;
    int retries = 0;
    while (true) {
        DWORD name_length = static_cast<DWORD>(name.size());
        DWORD data_size = static_cast<DWORD>(data.size());
        DWORD type = REG_NONE;
        const LSTATUS status =
            ::RegEnumValueW(key_.get(), index, name.data(), &name_length, nullptr, &type,
                            data.empty() ? nullptr : reinterpret_cast<BYTE*>(data.data()), &data_size);
        if (status == ERROR_MORE_DATA && retries++ < 4) {
            // The value grew after RegQueryInfoKeyW; grow both buffers and retry the same index.
            name.resize(name.size() * 2 + 64);
            data.resize(data.size() * 2 + 1024);
            continue;
        }
        if (status != ERROR_SUCCESS) {
            break;  // ERROR_NO_MORE_ITEMS, or the key went away
        }
        RegistryValue value;
        value.name.assign(name.data(), name_length);
        value.type = type;
        value.data.assign(data.begin(), data.begin() + data_size);
        result.push_back(std::move(value));
        ++index;
        retries = 0;
    }
    return result;
}

std::optional<FILETIME> RegistryKey::last_write_time() const {
    FILETIME time{};
    if (::RegQueryInfoKeyW(key_.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, &time) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return time;
}

}  // namespace avel::platform
