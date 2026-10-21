// Internal to the leftover scan: the snapshots of folders, keys, shortcuts and references taken
// once per scan, and the scanners that match each target against them. Front ends use scan.h.
//
// The scanners follow Bulk Crap Uninstaller's junk finders (https://github.com/BCUninstaller/Bulk-Crap-Uninstaller),
// Copyright (c) 2017 Marcin Szeniak, Apache License 2.0:
//   UninstallTools/Junk/Finders/Drive/InstallLocationScanner.cs
//   UninstallTools/Junk/Finders/Drive/CommonDriveJunkScanner.cs
//   UninstallTools/Junk/Finders/Misc/ShortcutJunk.cs
//   UninstallTools/Junk/Finders/Registry/SoftwareRegKeyScanner.cs
//   UninstallTools/Junk/Finders/Registry/UninstallerKeySearcher.cs
//   UninstallTools/Junk/Finders/JunkCreatorBase.cs
//   UninstallTools/Junk/Confidence/ConfidenceGenerators.cs (TestForSimilarNames)
// BCU scans the current user only, walks the disk and registry again for every program, and
// compares paths as plain string prefixes; Avel scans every user, takes each snapshot once, and
// compares paths by component.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/leftover.h"
#include "core/leftovers/scan.h"
#include "core/leftovers/scan_target.h"
#include "core/platform/registry.h"

namespace avel::leftovers::detail {

// A folder up to three levels below one of the drive scan's roots.
struct FolderNode {
    std::wstring path;
    std::wstring name;
    std::wstring parent_name;
    int level = 0;                    // 0 = directly in a root
    bool known_folder = false;        // A known folder itself: never offered, but searched
    bool parent_known_folder = false;
    const UserScope* user = nullptr;  // Whose profile it is in; null for machine-wide roots
};

// A key up to three levels below one of the registry scan's roots, with the values that may
// name a program's folder.
struct KeyNode {
    platform::RegistryAddress address;
    std::wstring name;
    std::wstring parent_name;
    int level = 0;
    std::size_t root = 0;             // Index into Snapshots::key_roots
    std::wstring relative_path;       // Below its root, for finding the same key under the others
    const UserScope* user = nullptr;
    std::vector<std::wstring> folder_values;          // Values naming a folder (InstallDir, ...)
    std::vector<std::wstring> file_values;            // Values naming a program file (ExePath, ...)
    std::vector<std::wstring> file_or_folder_values;  // Either (Path, AppPath, ...)
    std::wstring default_value;
};

struct KeyRoot {
    platform::RegistryAddress address;
    const UserScope* user = nullptr;
};

struct Shortcut {
    std::wstring file;
    std::wstring target;
    const UserScope* user = nullptr;
};

// A registry entry that refers to files: a Run value, an App Paths key, a SharedDLLs value, ...
struct Reference {
    Location location;
    ReferenceKind kind = ReferenceKind::None;
    bool run_once = false;
    std::vector<std::wstring> paths;
    const UserScope* user = nullptr;
};

// A service and the files it runs (ImagePath, ServiceDll).
struct ServiceEntry {
    std::wstring name;
    std::vector<std::wstring> paths;
    bool driver = false;
};

// A COM class and its server files.
struct ComClass {
    platform::RegistryAddress key;  // ...\Classes\CLSID\{GUID}
    std::wstring clsid;
    std::vector<std::wstring> paths;
    const UserScope* user = nullptr;
};

// A registration that names a COM class: a shell extension handler key or an "Approved" value.
struct ClassReference {
    Location location;
    std::wstring clsid;
    const UserScope* user = nullptr;
};

// A scheduled task and the files its actions run.
struct TaskEntry {
    std::wstring path;
    std::vector<std::wstring> paths;
};

struct FirewallRule {
    std::wstring id;
    std::wstring program;
};

// A Windows Installer key that lists products as its values, one of them a target's: an upgrade
// code (...\UpgradeCodes\<packed code>) or a component (UserData\<sid>\Components\<packed code>).
struct MsiClient {
    platform::RegistryAddress key;
    std::wstring parent;       // display_path of its UpgradeCodes or Components key, lowercase
    std::wstring product;      // The packed product code: the value's name
    std::wstring data;         // A component's key path: "C:\Program Files\App\app.exe", "02:\SOFTWARE\..."
    bool only_client = false;  // The product is the key's only value
};

// The folders of the fixed volumes that the deep search may offer, grouped by name.
struct DeepFolders {
    struct Folder {
        std::uint32_t volume = 0;  // Index into volumes
        std::uint32_t index = 0;   // Index into its folders()
    };
    struct Name {
        std::wstring name;  // As the first folder so named spells it
        std::vector<Folder> folders;
    };
    std::vector<platform::VolumeFolders> volumes;
    std::vector<Name> names;
};

struct Snapshots {
    std::vector<std::wstring> known_folders;  // Normalized, lowercase and sorted
    std::vector<std::wstring> folder_roots;   // Where the folders below come from, normalized
    std::vector<FolderNode> folders;
    std::vector<KeyRoot> key_roots;
    std::vector<KeyNode> keys;
    std::unordered_map<std::wstring, std::size_t> key_index;  // key_index_name(root, relative path) -> keys
    std::vector<Shortcut> shortcuts;
    std::vector<Reference> references;
    std::vector<ServiceEntry> services;
    std::vector<TaskEntry> tasks;
    std::vector<ComClass> com_classes;
    std::vector<ClassReference> class_references;
    std::vector<FirewallRule> firewall_rules;
    std::vector<MsiClient> msi_clients;  // Sorted by product; only for products still registered
    DeepFolders deep;                    // Empty unless ScanOptions::deep
    std::vector<ScanTiming> timings;
};

// Needs COM on the calling thread (shortcuts are resolved through IShellLink).
Snapshots take_snapshots(const ScanEnvironment& environment);

std::wstring key_index_name(std::size_t root, std::wstring_view relative_path);

// Parts of take_snapshots (system_snapshots.cpp).
void take_services(const ScanEnvironment& environment, Snapshots& snapshots);  // Also firewall rules
void take_tasks(const ScanEnvironment& environment, Snapshots& snapshots);
void take_com_classes(const ScanEnvironment& environment, Snapshots& snapshots);
// The upgrade codes and components of the targets' MSI products (msi_scanner.cpp). Components
// keys hold tens of thousands of subkeys, so they are read once for all targets, and only for
// products whose registration is still there.
void take_msi_clients(const ScanEnvironment& environment, std::span<const ScanTarget> targets,
                      Snapshots& snapshots);

// The folders of every fixed NTFS volume (deep_scanner.cpp). A volume that can't be read is
// reported in `diagnostics`.
void take_deep_folders(const ScanEnvironment& environment, Snapshots& snapshots,
                       std::vector<ScanDiagnostic>& diagnostics);

// Many MSI products record no install folder. Their components' key paths say where their files
// are: the deepest folder holding most of them (below the first protected folder), if it is named
// after the product and isn't the publisher's. Empty when there is no such folder.
std::wstring msi_install_folder(const ScanEnvironment& environment, const Snapshots& snapshots,
                                const ScanTarget& target,
                                const std::function<bool(const std::wstring& folder)>& is_protected);

struct ScanContext {
    const ScanEnvironment& environment;
    const Snapshots& snapshots;
    const ScanTarget& target;
    std::span<const inventory::Program> installed;  // What is still installed, targets excluded
    std::vector<std::wstring> other_locations;      // Their install folders
    std::vector<std::wstring> other_names;          // Their names without version or noise

    // The scan looks at a per-user program's owner only, and at everyone for a machine-wide one.
    bool in_scope(const UserScope* user) const;
    // Belongs to a user other than the one running Avel, for a machine-wide program.
    bool other_user(const UserScope* user) const;
    // Another installed program lives in or below `folder`.
    bool still_used(std::wstring_view folder) const;
};

ScanContext make_context(const ScanEnvironment& environment, const Snapshots& snapshots, const ScanTarget& target,
                         std::span<const inventory::Program> installed);

void scan_install_location(const ScanContext& context, std::vector<Leftover>& found);
void scan_common_drive(const ScanContext& context, std::vector<Leftover>& found);
void scan_shortcuts(const ScanContext& context, std::vector<Leftover>& found);
void scan_software_keys(const ScanContext& context, std::vector<Leftover>& found);
void scan_uninstaller_key(const ScanContext& context, std::vector<Leftover>& found);
void scan_references(const ScanContext& context, std::vector<Leftover>& found);
void scan_related_packages(const ScanContext& context, std::vector<Leftover>& found);
void scan_services(const ScanContext& context, std::vector<Leftover>& found);
void scan_tasks(const ScanContext& context, std::vector<Leftover>& found);
void scan_com_classes(const ScanContext& context, std::vector<Leftover>& found);
void scan_firewall_rules(const ScanContext& context, std::vector<Leftover>& found);
void scan_msi_registration(const ScanContext& context, std::vector<Leftover>& found);
void scan_deep(const ScanContext& context, std::vector<Leftover>& found);

// BCU TestForSimilarNames: among items found side by side (in one folder or key), penalizes
// those whose name another program's name contains, and all but the closest of several similar
// names. `names[i]` is the name of `found[indices[i]]`.
void test_for_similar_names(const ScanContext& context, std::vector<Leftover>& found,
                            const std::vector<std::size_t>& indices, const std::vector<std::wstring>& names);

// Opens a key of the environment read-only, in the 64-bit view (paths spell out WOW6432Node).
LSTATUS open_key(const ScanEnvironment& environment, const platform::RegistryAddress& address,
                 platform::RegistryKey& key);

}  // namespace avel::leftovers::detail
