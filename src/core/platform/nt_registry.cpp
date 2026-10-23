#include "core/platform/nt_registry.h"

#include <algorithm>
#include <cstring>

namespace avel::platform {

namespace {

// The native API's structures (winternl.h declares few of them), with lengths in bytes.
struct KeyBasicInformation {
    LARGE_INTEGER last_write_time;
    ULONG title_index;
    ULONG name_length;
    WCHAR name[1];
};

struct KeyFullInformation {
    LARGE_INTEGER last_write_time;
    ULONG title_index;
    ULONG class_offset;
    ULONG class_length;
    ULONG subkeys;
    ULONG max_name_length;
    ULONG max_class_length;
    ULONG values;
    ULONG max_value_name_length;
    ULONG max_value_data_length;
    WCHAR class_name[1];
};

struct KeyValueFullInformation {
    ULONG title_index;
    ULONG type;
    ULONG data_offset;
    ULONG data_length;
    ULONG name_length;
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
constexpr int kKeyBasicInformation = 0;
constexpr int kKeyFullInformation = 2;
constexpr int kKeyNameInformation = 3;
constexpr int kKeyValueFullInformation = 1;
constexpr int kKeyValuePartialInformation = 2;

using NtEnumerateKeyFunction = LONG(NTAPI*)(HANDLE, ULONG, int, PVOID, ULONG, PULONG);
using NtQueryKeyFunction = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using NtEnumerateValueKeyFunction = LONG(NTAPI*)(HANDLE, ULONG, int, PVOID, ULONG, PULONG);
using NtQueryValueKeyFunction = LONG(NTAPI*)(HANDLE, CountedString*, int, PVOID, ULONG, PULONG);
using NtSetValueKeyFunction = LONG(NTAPI*)(HANDLE, CountedString*, ULONG, ULONG, PVOID, ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(LONG);

template <typename Function>
Function ntdll(const char* name) {
    return reinterpret_cast<Function>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), name));
}

LSTATUS win32_error(LONG status) {
    static const auto convert = ntdll<RtlNtStatusToDosErrorFunction>("RtlNtStatusToDosError");
    return static_cast<LSTATUS>(convert(status));
}

bool too_small(LONG status) { return status == kStatusBufferOverflow || status == kStatusBufferTooSmall; }

// Room for one variable-length result: on the stack for the usual sizes (a key name has at most
// 255 characters), on the heap when a result needs more. A trace reads every key in the registry.
class ResultBuffer {
public:
    std::byte* data() { return heap_.empty() ? local_ : heap_.data(); }
    ULONG size() const {
        return heap_.empty() ? static_cast<ULONG>(sizeof(local_)) : static_cast<ULONG>(heap_.size());
    }
    void grow(ULONG needed) { heap_.resize(std::max<std::size_t>(needed, std::size_t{size()} * 2)); }

    template <typename Info>
    const Info* as() {
        return reinterpret_cast<const Info*>(data());
    }

private:
    alignas(8) std::byte local_[1024];
    std::vector<std::byte> heap_;
};

// A counted view of `text` for the native API, which takes the buffer as non-const.
CountedString counted(std::wstring& text) {
    const auto bytes = static_cast<USHORT>(text.size() * sizeof(wchar_t));
    return {bytes, bytes, text.data()};
}

FILETIME to_filetime(const LARGE_INTEGER& time) { return {time.LowPart, static_cast<DWORD>(time.HighPart)}; }

}  // namespace

LSTATUS read_raw_values(HKEY key, std::vector<RawValue>& values) {
    static const auto enumerate = ntdll<NtEnumerateValueKeyFunction>("NtEnumerateValueKey");
    ResultBuffer buffer;
    for (ULONG index = 0;;) {
        ULONG needed = 0;
        const LONG status =
            enumerate(key, index, kKeyValueFullInformation, buffer.data(), buffer.size(), &needed);
        if (status == kStatusNoMoreEntries) {
            return ERROR_SUCCESS;
        }
        if (too_small(status)) {
            buffer.grow(needed);
            continue;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* info = buffer.as<KeyValueFullInformation>();
        const auto* data = reinterpret_cast<const std::byte*>(info) + info->data_offset;
        values.push_back({std::wstring(info->name, info->name_length / sizeof(wchar_t)), info->type,
                          std::vector<std::byte>(data, data + info->data_length)});
        ++index;
    }
}

LSTATUS query_raw_value(HKEY key, std::wstring_view name, RawValue& value) {
    static const auto query = ntdll<NtQueryValueKeyFunction>("NtQueryValueKey");
    value = {std::wstring(name), REG_NONE, {}};
    CountedString counted_name = counted(value.name);
    ResultBuffer buffer;
    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG needed = 0;
        const LONG status =
            query(key, &counted_name, kKeyValuePartialInformation, buffer.data(), buffer.size(), &needed);
        if (too_small(status)) {
            buffer.grow(needed);
            continue;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* info = buffer.as<KeyValuePartialInformation>();
        const auto* data = reinterpret_cast<const std::byte*>(info->data);
        value.type = info->type;
        value.data.assign(data, data + info->data_length);
        return ERROR_SUCCESS;
    }
    return ERROR_MORE_DATA;
}

LSTATUS set_raw_value(HKEY key, const RawValue& value) {
    static const auto set = ntdll<NtSetValueKeyFunction>("NtSetValueKey");
    std::wstring name = value.name;
    CountedString counted_name = counted(name);
    const LONG status = set(key, &counted_name, 0, value.type, const_cast<std::byte*>(value.data.data()),
                            static_cast<ULONG>(value.data.size()));
    return status == kStatusSuccess ? ERROR_SUCCESS : win32_error(status);
}

bool is_registry_link(HKEY key) {
    DWORD type = REG_NONE;
    return ::RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS &&
           type == REG_LINK;
}

std::wstring kernel_name(HKEY key) {
    // KeyNameInformation fills a KEY_NAME_INFORMATION: a byte length, then the name.
    static const auto query = ntdll<NtQueryKeyFunction>("NtQueryKey");
    if (!query || !key) {
        return {};
    }
    ResultBuffer buffer;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG needed = 0;
        const LONG status = query(key, kKeyNameInformation, buffer.data(), buffer.size(), &needed);
        if (status >= 0) {
            ULONG length = 0;
            std::memcpy(&length, buffer.data(), sizeof(length));
            return std::wstring(reinterpret_cast<const wchar_t*>(buffer.data() + sizeof(ULONG)),
                                length / sizeof(wchar_t));
        }
        if (needed <= buffer.size()) {
            return {};
        }
        buffer.grow(needed);
    }
    return {};
}

LSTATUS query_key_info(HKEY key, KeyInfo& info) {
    static const auto query = ntdll<NtQueryKeyFunction>("NtQueryKey");
    ResultBuffer buffer;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG needed = 0;
        const LONG status = query(key, kKeyFullInformation, buffer.data(), buffer.size(), &needed);
        if (too_small(status)) {
            buffer.grow(needed);  // A long class name
            continue;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* full = buffer.as<KeyFullInformation>();
        info.last_write = to_filetime(full->last_write_time);
        info.subkeys = full->subkeys;
        info.values = full->values;
        info.max_name_length = full->max_name_length / sizeof(wchar_t);
        info.max_value_name_length = full->max_value_name_length / sizeof(wchar_t);
        info.max_value_data_size = full->max_value_data_length;
        return ERROR_SUCCESS;
    }
    return ERROR_MORE_DATA;
}

LSTATUS enum_subkeys(HKEY key, std::vector<SubkeyInfo>& subkeys) {
    static const auto enumerate = ntdll<NtEnumerateKeyFunction>("NtEnumerateKey");
    ResultBuffer buffer;
    for (ULONG index = 0;;) {
        ULONG needed = 0;
        const LONG status =
            enumerate(key, index, kKeyBasicInformation, buffer.data(), buffer.size(), &needed);
        if (status == kStatusNoMoreEntries) {
            return ERROR_SUCCESS;
        }
        if (too_small(status)) {
            buffer.grow(needed);
            continue;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* info = buffer.as<KeyBasicInformation>();
        subkeys.push_back({std::wstring(info->name, info->name_length / sizeof(wchar_t)),
                           to_filetime(info->last_write_time)});
        ++index;
    }
}

}  // namespace avel::platform
