#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

// Who may own and use a folder that holds Avel's backups.
struct FolderPolicy {
    std::wstring sddl;                // The DACL a new folder gets
    std::vector<std::wstring> owners;  // Acceptable owners: SIDs or SDDL aliases ("BA", "SY")
    bool hidden = false;               // Hidden + system attributes on a new folder
};

// SYSTEM and Administrators only, owned by either.
FolderPolicy admin_only_folder_policy(bool hidden = false);

enum class FolderStatus : std::uint8_t {
    Ok,
    NotFolder,     // A file is in the way
    ReparsePoint,  // The folder is a junction or link
    Redirected,    // It resolves elsewhere (a parent is a link)
    ForeignOwner,  // Someone else created it
    Unavailable,   // It can't be created or opened
};

std::string_view to_string(FolderStatus status);

// Creates `path` with the policy's DACL if it doesn't exist (its parents normally), then checks it.
FolderStatus ensure_secure_folder(const std::wstring& path, const FolderPolicy& policy);

// Checks an existing folder: a real folder, at the path it claims, owned as the policy says.
FolderStatus check_secure_folder(const std::wstring& path, const FolderPolicy& policy);

}  // namespace avel::platform
