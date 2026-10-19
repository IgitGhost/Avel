#include "core/platform/user_hives.h"

#include <filesystem>
#include <system_error>

namespace avel::platform {

LSTATUS open_user_hive(const UserProfile& profile, REGSAM access, UserHive& result) {
    result = {};
    if (profile.hive_loaded) {
        HKEY key = nullptr;
        const LSTATUS status = ::RegOpenKeyExW(HKEY_USERS, profile.sid.c_str(), 0, access, &key);
        if (status == ERROR_SUCCESS) {
            result.key.reset(key);
        }
        return status;
    }
    const std::filesystem::path hive_file = std::filesystem::path(profile.profile_path) / L"NTUSER.DAT";
    std::error_code ignored;
    if (profile.profile_path.empty() || !std::filesystem::exists(hive_file, ignored)) {
        return ERROR_FILE_NOT_FOUND;
    }
    HKEY loaded = nullptr;
    const LSTATUS status = ::RegLoadAppKeyW(hive_file.c_str(), &loaded, access, REG_PROCESS_APPKEY, 0);
    if (status == ERROR_SUCCESS) {
        result.key.reset(loaded);
        result.loaded_privately = true;
    }
    return status;
}

}  // namespace avel::platform
