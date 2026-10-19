#include "core/leftovers/environment.h"

#include "core/platform/elevation.h"

namespace avel::leftovers {

std::string_view to_string(ScanDiagnosticCode code) {
    switch (code) {
        case ScanDiagnosticCode::NotElevated:
            return "not-elevated";
        case ScanDiagnosticCode::UserHiveUnavailable:
            return "user-hive-unavailable";
    }
    return "unknown";
}

LiveEnvironment::LiveEnvironment() {
    environment_.machine = HKEY_LOCAL_MACHINE;
    environment_.machine_folders = platform::machine_folders();
    environment_.current_user_sid = platform::current_user_sid();
    if (!platform::is_process_elevated()) {
        environment_.diagnostics.push_back({ScanDiagnosticCode::NotElevated, L"", 0});
    }
    for (auto& profile : platform::enumerate_user_profiles()) {
        platform::UserHive hive;
        const LSTATUS status = platform::open_user_hive(profile, KEY_READ, hive);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            environment_.diagnostics.push_back({ScanDiagnosticCode::UserHiveUnavailable, profile.sid, status});
        }
        UserScope scope;
        scope.hive = hive.key.get();
        scope.folders = platform::user_folders(scope.hive, profile);
        scope.profile = std::move(profile);
        environment_.users.push_back(std::move(scope));
        hives_.push_back(std::move(hive));  // Moving a unique_hkey keeps the handle value valid
    }
}

}  // namespace avel::leftovers
