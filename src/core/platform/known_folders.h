#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/platform/user_profiles.h"

namespace avel::platform {

// A user's shell folders, read from their own hive ("User Shell Folders"), so folder
// redirection (for example to OneDrive) is honored for every user, not just the current one.
// All paths are normalized; folders the hive doesn't name fall back to the Windows defaults.
struct UserFolders {
    std::wstring profile;             // C:\Users\name
    std::wstring roaming_app_data;    // %APPDATA%
    std::wstring local_app_data;      // %LOCALAPPDATA%
    std::wstring local_low_app_data;  // AppData\LocalLow
    std::wstring programs;            // Start Menu\Programs
    std::wstring desktop;
    std::vector<std::wstring> all;    // every shell folder the user has, plus the profile and AppData roots
};

// `user_hive` is the user's HKEY_CURRENT_USER equivalent (see user_hives.h).
UserFolders user_folders(HKEY user_hive, const UserProfile& profile);

// Machine-wide folders. All paths are normalized.
struct MachineFolders {
    std::wstring windows;
    std::wstring program_files;      // the native (64-bit) one
    std::wstring program_files_x86;
    std::wstring program_data;
    std::wstring common_programs;    // the all-users Start Menu\Programs
    std::wstring common_desktop;     // the Public desktop
    std::wstring users;              // C:\Users
    std::vector<std::wstring> all;   // every fixed and common known folder
};

// Needs COM; initializes it on the calling thread if it isn't yet.
MachineFolders machine_folders();

}  // namespace avel::platform
