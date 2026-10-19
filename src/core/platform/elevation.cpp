#include "core/platform/elevation.h"

#include <windows.h>

#include <wil/resource.h>

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

}  // namespace avel::platform
