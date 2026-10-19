// File system operations for removal and undo. None of them follows a reparse point: a junction or
// symbolic link is always handled as the link itself, never as what it points to.
#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace avel::platform {

// Opens a file or folder for `access` without following a reparse point at its last component.
// Sharing allows others to read, write and delete, so it doesn't disturb running programs.
wil::unique_hfile open_item(const std::wstring& path, DWORD access);

// What the handle really refers to, as a normalized DOS path (no \\?\ prefix); nullopt if Windows
// can't tell. Differs from the path it was opened with when a parent folder is a link.
std::optional<std::wstring> final_path(HANDLE item);

// Renames the item behind `item` to `destination` on the same volume. Never replaces an existing
// item. Returns a Win32 error code.
DWORD rename_item(HANDLE item, const std::wstring& destination);

// Deletes a file, or a folder and everything in it. Links are deleted themselves, never followed;
// read-only attributes are cleared first. Returns ERROR_SUCCESS if nothing is left.
DWORD delete_tree(const std::wstring& path);

// The root of the volume holding `path` ("C:\"), and whether it's a fixed NTFS volume, the only
// kind Avel quarantines on.
std::optional<std::wstring> volume_root(const std::wstring& path);
bool is_fixed_ntfs(const std::wstring& volume_root);

// A path with a \\?\ prefix when it's too long for the plain Win32 APIs.
std::wstring extended(const std::wstring& path);

struct ItemInfo {
    DWORD attributes = 0;
    FILETIME created{};
    FILETIME accessed{};
    FILETIME written{};
    std::uint64_t size = 0;
};
std::optional<ItemInfo> item_info(const std::wstring& path);

// Owner, group and DACL as SDDL; empty if unreadable. And back.
std::wstring security_sddl(const std::wstring& path);
bool apply_security_sddl(const std::wstring& path, const std::wstring& sddl);

}  // namespace avel::platform
