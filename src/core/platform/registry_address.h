#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/platform/text.h"

namespace avel::platform {

enum class Hive : std::uint8_t {
    Machine,  // HKEY_LOCAL_MACHINE
    User,     // a user's hive: HKEY_USERS\<sid>, or their NTUSER.DAT loaded privately
};

// Where a registry key lives, independent of how it was opened. `path` is the physical path
// below the hive root with WOW6432Node spelled out (Avel is a 64-bit process and always opens
// keys with KEY_WOW64_64KEY), so the 32-bit view needs no separate flag.
struct RegistryAddress {
    Hive hive = Hive::Machine;
    std::wstring sid;   // The user's SID for Hive::User; empty for Hive::Machine
    std::wstring path;  // e.g. SOFTWARE\WOW6432Node\Vendor\App
};

inline bool same_address(const RegistryAddress& a, const RegistryAddress& b) {
    return a.hive == b.hive && iequals(a.sid, b.sid) && iequals(a.path, b.path);
}

// "HKLM\SOFTWARE\Vendor" or "HKU\S-1-5-21-...\Software\Vendor", for display and identity.
inline std::wstring display_path(const RegistryAddress& address) {
    std::wstring root = address.hive == Hive::Machine ? std::wstring(L"HKLM") : L"HKU\\" + address.sid;
    return address.path.empty() ? root : root + L"\\" + address.path;
}

}  // namespace avel::platform
