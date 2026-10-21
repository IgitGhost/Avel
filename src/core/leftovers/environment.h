#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/known_folders.h"
#include "core/platform/user_hives.h"
#include "core/platform/user_profiles.h"
#include "core/platform/volume_folders.h"

namespace avel::leftovers {

// One user whose profile and hive the scan can look into.
struct UserScope {
    platform::UserProfile profile;
    platform::UserFolders folders;
    HKEY hive = nullptr;  // Borrowed: their HKEY_CURRENT_USER equivalent; null if it couldn't be opened
};

// Something the scan could not look at. The GUI turns the code into translated text.
enum class ScanDiagnosticCode : std::uint8_t {
    NotElevated,            // Without administrator rights other users' hives and some keys are out of reach
    UserHiveUnavailable,    // A user's registry couldn't be opened; their keys weren't scanned
    DeepSearchUnavailable,  // A volume's folders couldn't be read for the deep search (it needs administrator
                            // rights)
};

struct ScanDiagnostic {
    ScanDiagnosticCode code;
    std::wstring subject;  // e.g. the user's SID
    std::int32_t error = 0;
};

std::string_view to_string(ScanDiagnosticCode code);

// A scheduled task and the program files its actions run.
struct ScheduledTask {
    std::wstring path;                  // e.g. \Contoso\Updater
    std::vector<std::wstring> actions;  // Command lines of its Exec actions
};

// Where the scan looks. The live environment points at this machine; tests point the roots at a
// scratch registry key and a temporary folder tree. Handles are borrowed from whoever built it.
struct ScanEnvironment {
    HKEY machine = HKEY_LOCAL_MACHINE;
    platform::MachineFolders machine_folders;
    std::vector<UserScope> users;
    std::wstring current_user_sid;
    std::vector<ScanDiagnostic> diagnostics;  // Problems met while opening it
    // The scheduled tasks to consider; when empty, the scan reads them from the Task Scheduler
    // (tests set them, since they can't register tasks without changing the system).
    std::optional<std::vector<ScheduledTask>> tasks;
    // The folders of the fixed NTFS volumes, for the deep search; when empty, the scan reads
    // them from the volumes' master file tables (tests set them).
    std::optional<std::vector<platform::VolumeFolders>> volumes;
};

// This machine: its folders, and every user profile with their hive (signed-out users' hives are
// loaded privately, read-only, for as long as this object lives).
class LiveEnvironment {
public:
    LiveEnvironment();
    const ScanEnvironment& environment() const { return environment_; }

private:
    std::vector<platform::UserHive> hives_;
    ScanEnvironment environment_;
};

}  // namespace avel::leftovers
