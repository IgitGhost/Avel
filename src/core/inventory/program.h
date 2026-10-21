#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace avel::inventory {

// Where an entry was discovered.
enum class Source : std::uint8_t {
    Registry,  // An Uninstall key (what Settings > Apps and Control Panel read)
    Msi,       // Windows Installer database only (no matching Uninstall key)
    Appx,      // Packaged app (Microsoft Store / MSIX)
};

enum class Scope : std::uint8_t {
    Machine,  // Installed for all users
    User,     // Installed for one user (see Program::user_sid)
};

enum class Architecture : std::uint8_t { Unknown, X86, X64, Arm, Arm64, Neutral };

// The technology that installed the program; it decides how it can be uninstalled.
enum class InstallerType : std::uint8_t {
    Unknown,
    Msi,
    Nsis,
    InnoSetup,
    InstallShield,
    WixBurn,
    Squirrel,
    PowerShell,
    SdbInst,
    Steam,
    Appx,
};

// Which registry view held an Uninstall key.
enum class RegistryView : std::uint8_t {
    Machine64,  // HKLM, 64-bit view
    Machine32,  // HKLM\SOFTWARE\WOW6432Node
    User,       // HKEY_USERS\<sid>\Software
    User32,     // HKEY_USERS\<sid>\Software\WOW6432Node (rare)
};

struct RegistryLocation {
    RegistryView view = RegistryView::Machine64;
    std::wstring key_name;  // The Uninstall subkey name, e.g. "{GUID}" or "7-Zip"
    std::wstring path;      // Full path for display, e.g. HKLM\SOFTWARE\...\Uninstall\7-Zip
};

struct Program {
    // Stable identity, e.g. "reg:HKLM64:7-Zip", "msi:{GUID}", "appx:<PackageFullName>".
    std::wstring id;
    Source source = Source::Registry;
    Scope scope = Scope::Machine;
    std::wstring user_sid;   // Owner of a per-user install
    std::wstring user_name;  // "DOMAIN\user" when it can be resolved
    Architecture architecture = Architecture::Unknown;
    InstallerType installer_type = InstallerType::Unknown;

    std::wstring display_name;
    std::wstring display_version;
    std::wstring publisher;
    std::wstring install_location;  // No surrounding quotes, no trailing separator
    std::wstring install_source;
    std::optional<std::chrono::sys_days> install_date;
    std::uint64_t estimated_size_bytes = 0;  // 0 = unknown
    std::wstring uninstall_string;
    std::wstring quiet_uninstall_string;
    std::wstring modify_path;
    std::wstring display_icon;  // "path,index" (registry) or an image file (packaged apps)
    std::wstring about_url;
    std::wstring comments;

    std::wstring msi_product_code;  // "{XXXXXXXX-...}", upper case
    std::wstring parent_key_name;   // Set on updates: the program they belong to
    std::wstring package_full_name;
    std::wstring package_family_name;
    // For a sparse package (one that gives a classic app package identity, e.g. for its Windows 11
    // context-menu handler): the app's folder it was registered with. Empty otherwise.
    std::wstring external_location;

    bool system_component = false;  // Hidden from Settings > Apps by its installer
    bool is_update = false;         // A patch or hotfix of another program
    bool is_framework = false;      // Packaged framework or resource package
    bool no_remove = false;         // The installer forbids uninstalling (also: system-signed packages)
    bool no_modify = false;
    bool no_repair = false;
    bool windows_installer = false;  // The Uninstall key has WindowsInstaller=1 (an MSI product's entry)
    // The Uninstall key claims an MSI product, but Windows Installer doesn't report that product as
    // installed for the machine or the current user (it may belong to another user, or be a
    // leftover). Settings hides such entries, and msiexec can't uninstall them for this user.
    bool msi_product_unregistered = false;
    // The file the uninstall command runs (or the script or DLL its host runs) is gone, so the
    // program can't uninstall itself; see platform::missing_command_file. Not set for MSI products,
    // which msiexec removes.
    bool uninstaller_missing = false;
    // An MSI product whose package Windows Installer can't find: neither its cached copy (the
    // LocalPackage under C:\Windows\Installer) nor the file it was installed from is there, so
    // msiexec can't remove it.
    bool msi_package_missing = false;

    std::optional<RegistryLocation> registry;  // The Uninstall key, if any
};

// The entry can't be uninstalled the normal way: its uninstaller or its MSI package is gone, or
// Windows Installer no longer knows its product. Avel Force removes such programs.
bool is_broken(const Program& program);

// Whether Settings > Apps would list the entry. Avel's default view shows exactly these.
bool is_listed_in_settings(const Program& program);

// Stable machine-readable names (used in JSON output and logs, never shown in the GUI).
std::string_view to_string(Source source);
std::string_view to_string(Scope scope);
std::string_view to_string(Architecture architecture);
std::string_view to_string(InstallerType type);
std::string_view to_string(RegistryView view);

}  // namespace avel::inventory
