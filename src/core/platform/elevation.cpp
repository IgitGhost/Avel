#include "core/platform/elevation.h"

#include <windows.h>

#include <wil/resource.h>

#include <string>

namespace avel::platform {

bool is_process_elevated() {
    wil::unique_handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put())) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!::GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

bool enable_privilege(std::wstring_view name) {
    wil::unique_handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put())) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::LookupPrivilegeValueW(nullptr, std::wstring(name).c_str(), &privileges.Privileges[0].Luid)) {
        return false;
    }
    // Succeeds even when the token lacks the privilege; that case is reported as ERROR_NOT_ALL_ASSIGNED.
    return ::AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr) &&
           ::GetLastError() == ERROR_SUCCESS;
}

}  // namespace avel::platform
