// Explorer's StartupApproved lists: whether each Run value and Startup folder item starts.
//
// Task Manager's Startup apps page and Settings > Apps > Startup don't touch the entries
// themselves. They keep a 12-byte REG_BINARY per entry, named after it, in
// ...\Explorer\StartupApproved\Run (the 64-bit Run key, and a user's), Run32 (the machine's
// WOW6432Node Run key) or StartupFolder (by the item's file name), under the machine's SOFTWARE
// and each user's Software. The first byte's low bit set means disabled (Task Manager writes 03,
// 01 is seen too); clear means enabled (02, 06). A value that is absent means enabled. Task
// Manager disables with 03 00 00 00 and the FILETIME of the change, and enables with 02 and 11
// zero bytes; Avel writes exactly that, so Task Manager agrees with it.
//
// Pure: nothing here reads or writes the registry.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"

namespace avel::leftovers {

enum class ApprovalList : std::uint8_t {
    Run,            // HKLM\SOFTWARE\...\Run and a user's Software\...\Run
    Run32,          // HKLM\SOFTWARE\WOW6432Node\...\Run
    StartupFolder,  // The common and the user's Startup folders, by file name
};

inline constexpr std::size_t kApprovalSize = 12;

// The list's key: the machine's for an empty `sid`, otherwise that user's.
platform::RegistryAddress approval_key(ApprovalList list, const std::wstring& sid);

// Which list `key` is, if it is exactly one of them (in the machine's hive or a user's, not a
// user's classes).
std::optional<ApprovalList> approval_list(const platform::RegistryAddress& key);

// The entry is turned off: a REG_BINARY whose first byte has its low bit set.
bool approval_disabled(const platform::RawValue& value);

// What Task Manager writes to turn an entry off at `when`, and on.
platform::RawValue disabled_approval(std::wstring name, FILETIME when);
platform::RawValue enabled_approval(std::wstring name);

// A value Avel may write: 12 bytes of REG_BINARY starting with 02 or 03.
bool is_approval_value(const platform::RawValue& value);

}  // namespace avel::leftovers
