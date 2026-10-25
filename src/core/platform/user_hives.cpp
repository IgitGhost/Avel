#include "core/platform/user_hives.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace avel::platform {

namespace {

LSTATUS open_hive(const std::wstring& loaded_name, bool loaded, const std::filesystem::path& hive_file,
                  REGSAM access, UserHive& result) {
    result = {};
    if (loaded) {
        HKEY key = nullptr;
        const LSTATUS status = ::RegOpenKeyExW(HKEY_USERS, loaded_name.c_str(), 0, access, &key);
        if (status == ERROR_SUCCESS) {
            result.key.reset(key);
        }
        return status;
    }
    std::error_code ignored;
    if (hive_file.empty() || !std::filesystem::exists(hive_file, ignored)) {
        return ERROR_FILE_NOT_FOUND;
    }
    HKEY loaded_key = nullptr;
    const LSTATUS status = ::RegLoadAppKeyW(hive_file.c_str(), &loaded_key, access, REG_PROCESS_APPKEY, 0);
    if (status == ERROR_SUCCESS) {
        result.key.reset(loaded_key);
        result.loaded_privately = true;
    }
    return status;
}

}  // namespace

LSTATUS open_user_hive(const UserProfile& profile, REGSAM access, UserHive& result) {
    const std::filesystem::path hive_file = profile.profile_path.empty()
                                                ? std::filesystem::path()
                                                : std::filesystem::path(profile.profile_path) / L"NTUSER.DAT";
    return open_hive(profile.sid, profile.hive_loaded, hive_file, access, result);
}

LSTATUS open_user_classes_hive(const UserProfile& profile, REGSAM access, UserHive& result) {
    const std::filesystem::path hive_file =
        profile.profile_path.empty() ? std::filesystem::path()
                                     : std::filesystem::path(expand_environment_for_user(
                                           LR"(%LOCALAPPDATA%\Microsoft\Windows\UsrClass.dat)", profile));
    return open_hive(profile.sid + L"_Classes", profile.hive_loaded, hive_file, access, result);
}

}  // namespace avel::platform
