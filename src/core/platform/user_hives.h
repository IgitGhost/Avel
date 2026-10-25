#pragma once

#include <windows.h>

#include <wil/resource.h>

#include "core/platform/user_profiles.h"

namespace avel::platform {

// A user's registry hive, the equivalent of their HKEY_CURRENT_USER.
struct UserHive {
    wil::unique_hkey key;
    bool loaded_privately = false;  // true when it is a private load of their NTUSER.DAT
};

// Opens `profile`'s hive: HKEY_USERS\<sid> while the user is signed in, otherwise a private load
// of their NTUSER.DAT with RegLoadAppKey, which keeps it invisible to other processes and
// unloads it when the last handle closes, so the user can still sign in meanwhile.
// Returns ERROR_SUCCESS, ERROR_FILE_NOT_FOUND when the profile has no hive file, or the error
// from opening or loading it.
LSTATUS open_user_hive(const UserProfile& profile, REGSAM access, UserHive& result);

// Opens `profile`'s classes hive, their HKEY_CURRENT_USER\Software\Classes: HKEY_USERS\<sid>_Classes
// while the user is signed in, otherwise a private load of their
// %LOCALAPPDATA%\Microsoft\Windows\UsrClass.dat, like open_user_hive. A hive file can be loaded
// privately only once per process at a time: a second load fails with ERROR_SHARING_VIOLATION.
LSTATUS open_user_classes_hive(const UserProfile& profile, REGSAM access, UserHive& result);

}  // namespace avel::platform
