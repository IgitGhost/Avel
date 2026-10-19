#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

struct UserProfile {
    std::wstring sid;
    std::wstring account_name;  // "DOMAIN\user", or the profile folder name if the SID can't be resolved
    std::wstring profile_path;  // e.g. C:\Users\name
    bool hive_loaded = false;   // HKEY_USERS\<sid> exists: the user is signed in or has processes running
};

// Real user accounts from the ProfileList: local and domain users (S-1-5-21-*) and Microsoft
// Entra ID users (S-1-12-1-*). Service accounts are skipped.
std::vector<UserProfile> enumerate_user_profiles();

// String SID of the user the current process runs as; empty on failure.
std::wstring current_user_sid();

// Expands %VARIABLES% using this process's environment.
std::wstring expand_environment(std::wstring_view text);

// Expands %VARIABLES% as they would be for `profile`'s owner: the per-user folders
// (%USERPROFILE%, %APPDATA%, %LOCALAPPDATA%, %HOMEDRIVE%%HOMEPATH%) point into their profile,
// everything else comes from this process's environment. Folder redirection is not considered.
std::wstring expand_environment_for_user(std::wstring_view text, const UserProfile& profile);

}  // namespace avel::platform
