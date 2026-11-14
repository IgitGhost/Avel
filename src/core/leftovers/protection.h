// What Avel never offers as a leftover, however well it matches.
//
// Two layers check every item:
//   - HardFloor: the last line of defence, built from the live system and not replaceable. It
//     guards the Windows folder, drive roots, Program Files, ProgramData and the user profiles
//     (and every ancestor of them), system areas such as WindowsApps, Package Cache and packaged
//     apps' data, OneDrive folders, Avel's own data, the SYSTEM registry, the core registry keys
//     (Run, Uninstall, App Paths, ...), the Task Scheduler's TaskCache, what Windows keeps for
//     registered packages (PackagedCom, AppModel, ...), and Windows' own scheduled tasks. Value
//     edits are allowed only in a narrow list of keys: the machine's and each user's Environment
//     and both SharedDLLs keys (see value_edits.h). State changes (ADR 0007) have their own rules:
//     startup approvals only in Explorer's three StartupApproved keys and only as Task Manager
//     writes them; never Windows' own tasks; never a driver's or a Windows service's start type.
//     No package item (removal, registration, provisioning) touches a package signed as part of
//     Windows, a framework or resource package, or one on the critical list (ADR 0008). A package
//     that gives a desktop program its identity (an external location) goes only as that
//     program's leftover, removed for one user; never registered, provisioned or removed for
//     everyone on its own.
//   - ProtectedPolicy: known folders of every user and of the machine (and their ancestors), plus
//     the folder and key names Bulk Crap Uninstaller never treats as a program's own
//     (UninstallTools/UninstallToolsGlobalConfig.cs, Junk/Finders/Registry/SoftwareRegKeyScanner.cs;
//     Copyright (c) 2017 Marcin Szeniak, Apache License 2.0).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/leftovers/leftover.h"

namespace avel::leftovers {

enum class ProtectionReason : std::uint8_t {
    None,
    InvalidPath,  // Not an absolute path
    DriveRoot,
    WindowsFolder,        // The Windows folder or anything in it
    ProgramFiles,         // A Program Files folder or one of its ancestors
    ProgramData,          // ProgramData or one of its ancestors
    UserProfiles,         // The profiles folder, a profile, or one of their ancestors
    SystemArea,           // WindowsApps, Package Cache, System Volume Information, ...
    CloudFolder,          // A OneDrive folder or anything in it
    AvelData,             // Avel's own files and keys
    KnownFolder,          // A known folder or one of its ancestors
    BlacklistedName,      // A shared folder or key such as "Microsoft" or "Common Files"
    CoreRegistryKey,      // A key Windows relies on, or one of its ancestors
    SystemRegistry,       // HKLM\SYSTEM and the other system hives
    WindowsTask,          // One of Windows' own scheduled tasks (under "\Microsoft\")
    PackageRegistration,  // What Windows keeps for a registered package (PackagedCom, ...): it goes
                          // with the package (PackageLocation), never as raw keys or values
    NotEditable,     // A value edit outside the environment and SharedDLLs, or of a variable Windows needs;
                     // a startup approval outside StartupApproved, or not as Task Manager writes it
    WindowsService,  // A start type change of a driver, of a service whose image is Windows' own, or
                     // to a start type other than automatic, manual or disabled
    WindowsPackage,  // A package Windows needs or that isn't an app of its own: signed as part of
                     // Windows (SignatureKind System), a framework or resource package, or on the
                     // list of critical packages (the Store, App Installer, the shell's hosts...)
    // A package that gives a desktop program its identity (an external location), changed as an
    // app of its own: registration, provisioning or removal for every user. Only a leftover's
    // removal for one user (PackageLocation without all_users) may take it, once its program is
    // uninstalled.
    DesktopProgramPackage,
};

std::string_view to_string(ProtectionReason reason);

// What the floor needs to know about a service before its start type may change.
struct ServiceFacts {
    enum class Status : std::uint8_t {
        Found,
        Missing,     // No such service: the executor reports it as missing
        Unreadable,  // Its configuration couldn't be read: refused
    };
    Status status = Status::Unreadable;
    bool driver = false;
    // Its image (a svchost service's ServiceDll) is Windows' own: validly signed through one of
    // Microsoft's product roots, and not a hardware vendor's (see platform/signature.h)
    bool windows = false;
};

// The live service's facts, from the Service Control Manager, its key and its image's signature.
ServiceFacts live_service_facts(const std::wstring& name);

// What the floor needs to know about a package on this PC before any package item changes.
struct PackageFacts {
    bool system = false;  // SignatureKind System: the public stand-in for NonRemovable (ADR 0008)
    bool framework = false;
    bool resource = false;
    // It has an external location: it gives a desktop program its identity (a sparse package)
    bool external_location = false;
};

// The facts of the package with this full name, from PackageManager; nullopt when it isn't on this
// PC (the executor then reports the item missing).
std::optional<PackageFacts> live_package_facts(const std::wstring& full_name);

class HardFloor {
public:
    struct Data {
        std::wstring windows;
        // Folders that may neither be removed nor have an ancestor removed.
        std::vector<std::pair<std::wstring, ProtectionReason>> keep;
        // Folders nothing inside of which may be removed.
        std::vector<std::pair<std::wstring, ProtectionReason>> subtrees;
        // How services are looked up for ServiceStartLocation; live_service_facts when empty.
        std::function<ServiceFacts(const std::wstring& name)> services;
        // How packages are looked up for every package kind; live_package_facts when empty.
        std::function<std::optional<PackageFacts>(const std::wstring& full_name)> packages;
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
