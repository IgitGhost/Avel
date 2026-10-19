// What Avel never offers as a leftover, however well it matches.
//
// Two layers check every item:
//   - HardFloor: the last line of defence, built from the live system and not replaceable. It
//     guards the Windows folder, drive roots, Program Files, ProgramData and the user profiles
//     (and every ancestor of them), system areas such as WindowsApps, Package Cache and packaged
//     apps' data, OneDrive folders, Avel's own data, the SYSTEM registry and the core registry
//     keys (Run, Uninstall, App Paths, ...), and Windows' own scheduled tasks.
//   - ProtectedPolicy: known folders of every user and of the machine (and their ancestors), plus
//     the folder and key names Bulk Crap Uninstaller never treats as a program's own
//     (UninstallTools/UninstallToolsGlobalConfig.cs, Junk/Finders/Registry/SoftwareRegKeyScanner.cs;
//     Copyright (c) 2017 Marcin Szeniak, Apache License 2.0).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/leftovers/leftover.h"

namespace avel::leftovers {

enum class ProtectionReason : std::uint8_t {
    None,
    InvalidPath,       // Not an absolute path
    DriveRoot,
    WindowsFolder,     // The Windows folder or anything in it
    ProgramFiles,      // A Program Files folder or one of its ancestors
    ProgramData,       // ProgramData or one of its ancestors
    UserProfiles,      // The profiles folder, a profile, or one of their ancestors
    SystemArea,        // WindowsApps, Package Cache, System Volume Information, ...
    CloudFolder,       // A OneDrive folder or anything in it
    AvelData,          // Avel's own files and keys
    KnownFolder,       // A known folder or one of its ancestors
    BlacklistedName,   // A shared folder or key such as "Microsoft" or "Common Files"
    CoreRegistryKey,   // A key Windows relies on, or one of its ancestors
    SystemRegistry,    // HKLM\SYSTEM and the other system hives
    WindowsTask,       // One of Windows' own scheduled tasks (under "\Microsoft\")
};

std::string_view to_string(ProtectionReason reason);

class HardFloor {
public:
    struct Data {
        std::wstring windows;
        // Folders that may neither be removed nor have an ancestor removed.
        std::vector<std::pair<std::wstring, ProtectionReason>> keep;
        // Folders nothing inside of which may be removed.
        std::vector<std::pair<std::wstring, ProtectionReason>> subtrees;
    };

    explicit HardFloor(Data data);

    // Built once from this machine: its folders, every user profile and their OneDrive folders.
    static const HardFloor& live();

    ProtectionReason check(const Location& location) const;

private:
    Data data_;
};

class ProtectedPolicy {
public:
    // Known folders of the machine and of every user in the scan.
    explicit ProtectedPolicy(std::vector<std::wstring> known_folders);

    ProtectionReason check(const Location& location) const;

private:
    std::vector<std::wstring> known_folders_;
};

// Folders and keys that hold many programs' data rather than one program's. A folder name also
// counts without its last ".extension" ("Microsoft.NET").
bool is_blacklisted_folder_name(std::wstring_view name);
bool is_blacklisted_key_name(std::wstring_view name);
// Folder names too generic to tell which program they belong to.
bool is_questionable_folder_name(std::wstring_view name);

}  // namespace avel::leftovers
