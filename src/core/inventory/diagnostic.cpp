#include "core/inventory/diagnostic.h"

namespace avel::inventory {

std::string_view to_string(DiagnosticCode code) {
    switch (code) {
        case DiagnosticCode::UninstallKeyUnreadable:
            return "uninstall-key-unreadable";
        case DiagnosticCode::UserHiveLoadFailed:
            return "user-hive-load-failed";
        case DiagnosticCode::MsiAllUsersDenied:
            return "msi-all-users-denied";
        case DiagnosticCode::MsiEnumerationFailed:
            return "msi-enumeration-failed";
        case DiagnosticCode::AppxEnumerationFailed:
            return "appx-enumeration-failed";
    }
    return "unknown";
}

}  // namespace avel::inventory
