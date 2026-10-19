#include "core/changes/registry_backup.h"

#include <wil/resource.h>

#include <algorithm>
#include <cstring>
#include <format>

#include "core/platform/elevation.h"

namespace avel::changes {

namespace {

// KEY_READ includes READ_CONTROL. Opening with DELETE and KEY_SET_VALUE up front proves the tree
// can be deleted before anything is touched.
constexpr REGSAM kCaptureAccess = KEY_READ | KEY_SET_VALUE | DELETE | KEY_WOW64_64KEY;
constexpr int kMaxDepth = 512;  // The registry's own limit
constexpr SECURITY_INFORMATION kSecurityParts =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

LSTATUS read_security(HKEY key, std::vector<std::byte>& security) {
    DWORD size = 256;
    for (int attempt = 0; attempt < 4; ++attempt) {
        security.resize(size);
        const LSTATUS status = ::RegGetKeySecurity(key, kSecurityParts, security.data(), &size);
        if (status == ERROR_SUCCESS) {
            security.resize(::GetSecurityDescriptorLength(security.data()));
            return ERROR_SUCCESS;
        }
        if (status != ERROR_INSUFFICIENT_BUFFER) {
            return status;
        }
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

LSTATUS read_class(HKEY key, std::wstring& class_name) {
    std::wstring buffer(256, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD length = static_cast<DWORD>(buffer.size());
        const LSTATUS status = ::RegQueryInfoKeyW(key, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_SUCCESS) {
            class_name.assign(buffer.data(), length);
            return ERROR_SUCCESS;
        }
        if (status != ERROR_MORE_DATA) {
            return status;
        }
        buffer.resize(buffer.size() * 4);
    }
    return ERROR_MORE_DATA;
}

// Values are read through the native API: RegEnumValueW and RegQueryValueExW may add a terminator
// to a string stored without one and count it, depending on what follows in the caller's buffer.
// A backup needs the bytes exactly as stored.
struct KeyValueFullInformation {
    ULONG title_index;
    ULONG type;
    ULONG data_offset;
    ULONG data_length;
    ULONG name_length;  // In bytes
    WCHAR name[1];
};

struct KeyValuePartialInformation {
    ULONG title_index;
    ULONG type;
    ULONG data_length;
    UCHAR data[1];
};

struct CountedString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

constexpr LONG kStatusSuccess = 0;
constexpr LONG kStatusBufferOverflow = static_cast<LONG>(0x80000005);
constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001A);
constexpr LONG kStatusBufferTooSmall = static_cast<LONG>(0xC0000023);
constexpr int kKeyValueFullInformation = 1;
constexpr int kKeyValuePartialInformation = 2;

using NtEnumerateValueKeyFunction = LONG(NTAPI*)(HANDLE, ULONG, int, PVOID, ULONG, PULONG);
using NtQueryValueKeyFunction = LONG(NTAPI*)(HANDLE, CountedString*, int, PVOID, ULONG, PULONG);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(LONG);

template <typename Function>
Function ntdll(const char* name) {
    return reinterpret_cast<Function>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), name));
}

LSTATUS win32_error(LONG status) {
    static const auto convert = ntdll<RtlNtStatusToDosErrorFunction>("RtlNtStatusToDosError");
    return static_cast<LSTATUS>(convert(status));
}

// Unlike platform::RegistryKey, these fail on any error instead of returning what they could
// read: a partial backup must never pass for a complete one.
LSTATUS read_values(HKEY key, std::vector<ValueSnapshot>& values) {
    static const auto enumerate = ntdll<NtEnumerateValueKeyFunction>("NtEnumerateValueKey");
    std::vector<std::byte> buffer(1024);
    for (ULONG index = 0;;) {
        ULONG needed = 0;
        const LONG status = enumerate(key, index, kKeyValueFullInformation, buffer.data(),
                                      static_cast<ULONG>(buffer.size()), &needed);
        if (status == kStatusNoMoreEntries) {
            return ERROR_SUCCESS;
        }
        if (status == kStatusBufferOverflow || status == kStatusBufferTooSmall) {
            buffer.resize(std::max<std::size_t>(needed, buffer.size() * 2));
            continue;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* info = reinterpret_cast<const KeyValueFullInformation*>(buffer.data());
        const auto* data = buffer.data() + info->data_offset;
        values.push_back({std::wstring(info->name, info->name_length / sizeof(wchar_t)), info->type,
                          std::vector<std::byte>(data, data + info->data_length)});
        ++index;
    }
}

LSTATUS read_subkey_names(HKEY key, std::vector<std::wstring>& names) {
    std::wstring name(256, L'\0');
    for (DWORD index = 0;;) {
        DWORD length = static_cast<DWORD>(name.size());
        const LSTATUS status = ::RegEnumKeyExW(key, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            return ERROR_SUCCESS;
        }
        if (status == ERROR_MORE_DATA) {
            name.resize(name.size() * 2);
            continue;
        }
        if (status != ERROR_SUCCESS) {
            return status;
        }
        names.emplace_back(name.data(), length);
        ++index;
    }
}

bool iequals_ordinal(std::wstring_view a, std::wstring_view b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool is_link(HKEY key) {
    DWORD type = REG_NONE;
    return ::RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS &&
           type == REG_LINK;
}

LSTATUS capture(HKEY parent, std::wstring_view name, KeySnapshot& snapshot, int depth, REGSAM access = kCaptureAccess,
                const std::vector<std::wstring>* skip = nullptr) {
    if (depth > kMaxDepth) {
        return ERROR_INVALID_DATA;
    }
    HKEY raw = nullptr;
    if (const LSTATUS status = ::RegOpenKeyExW(parent, std::wstring(name).c_str(), REG_OPTION_OPEN_LINK, access, &raw);
        status != ERROR_SUCCESS) {
        return status;
    }
    const wil::unique_hkey key(raw);
    if (is_link(key.get())) {
        return ERROR_CANT_ACCESS_FILE;
    }
    snapshot.name = name;
    std::vector<std::wstring> subkeys;
    for (const LSTATUS status :
         {read_class(key.get(), snapshot.class_name), read_security(key.get(), snapshot.security),
          read_values(key.get(), snapshot.values), read_subkey_names(key.get(), subkeys)}) {
        if (status != ERROR_SUCCESS) {
            return status;
        }
    }
    if (skip) {
        std::erase_if(subkeys, [&](const std::wstring& subkey) {
            return std::any_of(skip->begin(), skip->end(), [&](const auto& name) { return iequals_ordinal(name, subkey); });
        });
    }
    snapshot.subkeys.resize(subkeys.size());
    for (std::size_t i = 0; i < subkeys.size(); ++i) {
        if (const LSTATUS status = capture(key.get(), subkeys[i], snapshot.subkeys[i], depth + 1, access);
            status != ERROR_SUCCESS) {
            return status;
        }
    }
    return ERROR_SUCCESS;
}

bool same_sid(PSID a, PSID b) {
    return (a == nullptr) == (b == nullptr) && (a == nullptr || ::EqualSid(a, b));
}

// Applies the parts of `stored` that differ from what the new key got by inheritance.
void apply_security(HKEY key, const std::vector<std::byte>& stored) {
    if (stored.empty() || !::IsValidSecurityDescriptor(const_cast<std::byte*>(stored.data()))) {
        return;
    }
    std::vector<std::byte> current;
    if (read_security(key, current) == ERROR_SUCCESS && current == stored) {
        return;
    }
    const auto descriptor = const_cast<std::byte*>(stored.data());
    ::RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor);

    PSID stored_owner = nullptr;
    PSID current_owner = nullptr;
    PSID stored_group = nullptr;
    PSID current_group = nullptr;
    BOOL defaulted = FALSE;
    ::GetSecurityDescriptorOwner(descriptor, &stored_owner, &defaulted);
    ::GetSecurityDescriptorGroup(descriptor, &stored_group, &defaulted);
    if (!current.empty()) {
        ::GetSecurityDescriptorOwner(current.data(), &current_owner, &defaulted);
        ::GetSecurityDescriptorGroup(current.data(), &current_group, &defaulted);
    }
    if (!same_sid(stored_owner, current_owner)) {
        // Setting someone else as owner takes SeRestorePrivilege; without it the key keeps the
        // owner it was created with.
        platform::enable_privilege(SE_RESTORE_NAME);
        ::RegSetKeySecurity(key, OWNER_SECURITY_INFORMATION, descriptor);
    }
    if (!same_sid(stored_group, current_group)) {
        ::RegSetKeySecurity(key, GROUP_SECURITY_INFORMATION, descriptor);
    }
}

// Through the native API too: RegSetValueExW stretches a string value by a terminator when one
// happens to follow the data in memory.
LSTATUS set_value(HKEY key, const ValueSnapshot& value) {
    using NtSetValueKeyFunction = LONG(NTAPI*)(HANDLE, CountedString*, ULONG, ULONG, PVOID, ULONG);
    static const auto set = ntdll<NtSetValueKeyFunction>("NtSetValueKey");
    std::wstring name = value.name;
    CountedString counted{static_cast<USHORT>(name.size() * sizeof(wchar_t)),
                          static_cast<USHORT>(name.size() * sizeof(wchar_t)), name.data()};
    const LONG status = set(key, &counted, 0, value.type, const_cast<std::byte*>(value.data.data()),
                            static_cast<ULONG>(value.data.size()));
    return status == kStatusSuccess ? ERROR_SUCCESS : win32_error(status);
}

LSTATUS create(HKEY parent, const KeySnapshot& snapshot, bool top, int depth) {
    if (depth > kMaxDepth) {
        return ERROR_INVALID_DATA;
    }
    std::wstring class_name = snapshot.class_name;
    HKEY raw = nullptr;
    DWORD disposition = 0;
    const auto create_key = [&](DWORD options) {
        return ::RegCreateKeyExW(parent, snapshot.name.c_str(), 0, class_name.empty() ? nullptr : class_name.data(),
                                 options, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &raw, &disposition);
    };
    LSTATUS status = create_key(REG_OPTION_NON_VOLATILE);
    if (status == ERROR_CHILD_MUST_BE_VOLATILE) {
        status = create_key(REG_OPTION_VOLATILE);
    }
    if (status != ERROR_SUCCESS) {
        return status;
    }
    const wil::unique_hkey key(raw);
    if (top && disposition == REG_OPENED_EXISTING_KEY) {
        return ERROR_ALREADY_EXISTS;
    }
    for (const auto& value : snapshot.values) {
        if (status = set_value(key.get(), value); status != ERROR_SUCCESS) {
            return status;
        }
    }
    for (const auto& subkey : snapshot.subkeys) {
        if (status = create(key.get(), subkey, false, depth + 1); status != ERROR_SUCCESS) {
            return status;
        }
    }
    apply_security(key.get(), snapshot.security);
    return ERROR_SUCCESS;
}

// regedit's escaping inside quotes.
std::wstring quoted(std::wstring_view text) {
    std::wstring result = L"\"";
    for (const wchar_t c : text) {
        if (c == L'\\' || c == L'"') {
            result.push_back(L'\\');
        }
        result.push_back(c);
    }
    return result + L"\"";
}

std::wstring value_line(const ValueSnapshot& value) {
    std::wstring line = value.name.empty() ? L"@=" : quoted(value.name) + L"=";
    if (value.type == REG_SZ && value.data.size() % 2 == 0) {
        std::wstring text(value.data.size() / 2, L'\0');
        std::memcpy(text.data(), value.data.data(), value.data.size());
        if (!text.empty() && text.back() == L'\0') {
            text.pop_back();
        }
        if (text.find(L'\0') == std::wstring::npos) {
            return line + quoted(text);
        }
    }
    if (value.type == REG_DWORD && value.data.size() == 4) {
        DWORD number = 0;
        std::memcpy(&number, value.data.data(), 4);
        return line + std::format(L"dword:{:08x}", number);
    }
    line += value.type == REG_BINARY ? L"hex:" : std::format(L"hex({:x}):", value.type);
    for (std::size_t i = 0; i < value.data.size(); ++i) {
        line += std::format(L"{}{:02x}", i ? L"," : L"", static_cast<unsigned>(value.data[i]));
    }
    return line;
}

void append_key(std::wstring& text, const std::wstring& path, const KeySnapshot& snapshot) {
    text += L"[" + path + L"]\r\n";
    for (const auto& value : snapshot.values) {
        text += value_line(value) + L"\r\n";
    }
    text += L"\r\n";
    for (const auto& subkey : snapshot.subkeys) {
        append_key(text, path + L"\\" + subkey.name, subkey);
    }
}

constexpr wchar_t kRegHeader[] = L"\xFEFFWindows Registry Editor Version 5.00\r\n\r\n";

}  // namespace

LSTATUS capture_key(HKEY parent, std::wstring_view name, KeySnapshot& snapshot) {
    snapshot = {};
    return capture(parent, name, snapshot, 0);
}

LSTATUS capture_key_readonly(HKEY parent, std::wstring_view name, const std::vector<std::wstring>& skip,
                             KeySnapshot& snapshot) {
    snapshot = {};
    return capture(parent, name, snapshot, 0, KEY_READ | KEY_WOW64_64KEY, &skip);
}

LSTATUS delete_key(HKEY parent, std::wstring_view name) {
    const std::wstring path(name);
    const LSTATUS status = ::RegDeleteTreeW(parent, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return status;
    }
    // RegDeleteTreeW leaves the key itself on some versions of Windows.
    const LSTATUS deleted = ::RegDeleteKeyExW(parent, path.c_str(), KEY_WOW64_64KEY, 0);
    return deleted == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : deleted;
}

LSTATUS restore_key(HKEY parent, const KeySnapshot& snapshot) {
    HKEY existing = nullptr;
    if (::RegOpenKeyExW(parent, snapshot.name.c_str(), REG_OPTION_OPEN_LINK, KEY_READ | KEY_WOW64_64KEY, &existing) ==
        ERROR_SUCCESS) {
        ::RegCloseKey(existing);
        return ERROR_ALREADY_EXISTS;
    }
    return create(parent, snapshot, true, 0);
}

LSTATUS merge_key(HKEY parent, const KeySnapshot& snapshot) {
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(parent, snapshot.name.c_str(), REG_OPTION_OPEN_LINK, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &raw) !=
        ERROR_SUCCESS) {
        return create(parent, snapshot, false, 0);  // Missing altogether: recreate it whole
    }
    const wil::unique_hkey key(raw);
    if (is_link(key.get())) {
        return ERROR_CANT_ACCESS_FILE;
    }
    for (const auto& value : snapshot.values) {
        if (::RegQueryValueExW(key.get(), value.name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND) {
            if (const LSTATUS status = set_value(key.get(), value); status != ERROR_SUCCESS) {
                return status;
            }
        }
    }
    for (const auto& subkey : snapshot.subkeys) {
        if (const LSTATUS status = merge_key(key.get(), subkey); status != ERROR_SUCCESS) {
            return status;
        }
    }
    return ERROR_SUCCESS;
}

LSTATUS capture_value(HKEY key, std::wstring_view name, ValueSnapshot& snapshot) {
    static const auto query = ntdll<NtQueryValueKeyFunction>("NtQueryValueKey");
    snapshot = {std::wstring(name), REG_NONE, {}};
    CountedString counted{static_cast<USHORT>(snapshot.name.size() * sizeof(wchar_t)),
                          static_cast<USHORT>(snapshot.name.size() * sizeof(wchar_t)), snapshot.name.data()};
    std::vector<std::byte> buffer(256);
    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG needed = 0;
        const LONG status = query(key, &counted, kKeyValuePartialInformation, buffer.data(),
                                  static_cast<ULONG>(buffer.size()), &needed);
        if (status == kStatusBufferOverflow || status == kStatusBufferTooSmall) {
            buffer.resize(std::max<std::size_t>(needed, buffer.size() * 2));
            continue;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* info = reinterpret_cast<const KeyValuePartialInformation*>(buffer.data());
        snapshot.type = info->type;
        snapshot.data.assign(reinterpret_cast<const std::byte*>(info->data),
                             reinterpret_cast<const std::byte*>(info->data) + info->data_length);
        return ERROR_SUCCESS;
    }
    return ERROR_MORE_DATA;
}

LSTATUS restore_value(HKEY key, const ValueSnapshot& snapshot) {
    if (::RegQueryValueExW(key, snapshot.name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        return ERROR_ALREADY_EXISTS;
    }
    return set_value(key, snapshot);
}

std::wstring to_reg_text(std::wstring_view path, const KeySnapshot& snapshot) {
    std::wstring text = kRegHeader;
    append_key(text, std::wstring(path), snapshot);
    return text;
}

std::wstring to_reg_text(std::wstring_view key_path, const ValueSnapshot& snapshot) {
    return kRegHeader + (L"[" + std::wstring(key_path) + L"]\r\n") + value_line(snapshot) + L"\r\n";
}

}  // namespace avel::changes
