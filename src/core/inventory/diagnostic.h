#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace avel::inventory {

// Something a scan could not do. The GUI turns the code into translated text; `subject` names
// what was affected (a user SID, a registry path, ...) and `error` carries the Win32/HRESULT code.
enum class DiagnosticCode : std::uint8_t {
    UninstallKeyUnreadable,  // An Uninstall key exists but couldn't be opened
    UserHiveLoadFailed,      // A signed-out user's NTUSER.DAT couldn't be loaded
    MsiAllUsersDenied,  // Other users' Windows Installer products need elevation; only the current user was
                        // listed
    MsiEnumerationFailed,
    AppxEnumerationFailed,
};

struct Diagnostic {
    DiagnosticCode code;
    std::wstring subject;
    std::int32_t error = 0;
};

std::string_view to_string(DiagnosticCode code);

}  // namespace avel::inventory
