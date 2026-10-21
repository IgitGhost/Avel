// Ported in part from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See scanners.h for the original source files.
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <set>

#include "core/leftovers/protection.h"
#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;
constexpr int kMaxLevel = 2;  // BCU evaluates folders and keys up to three levels below a root

// SoftwareRegKeyScanner's value names that may hold the program's folder or files.
constexpr std::array<std::wstring_view, 9> kFolderValueNames = {
    L"InstallDir", L"Install_Dir", L"Install Directory",        L"InstDir", L"ApplicationPath",
    L"Install folder", L"Last Stable Install Path", L"TARGETDIR", L"JavaHome"};
constexpr std::array<std::wstring_view, 5> kFileValueNames = {L"exe64", L"exe32", L"Executable", L"PathToExe",
                                                              L"ExePath"};
constexpr std::array<std::wstring_view, 5> kFileOrFolderValueNames = {L"Path", L"Path64", L"pth", L"PlayerPath",
                                                                      L"AppPath"};

template <size_t N>
bool in_list(const std::array<std::wstring_view, N>& list, std::wstring_view name) {
    return std::any_of(list.begin(), list.end(), [&](auto item) { return platform::iequals(item, name); });
}

// `known_folders` holds normalized, lowercase paths, sorted.
bool is_known(const Snapshots& snapshots, std::wstring_view path) {
    return std::binary_search(snapshots.known_folders.begin(), snapshots.known_folders.end(),
                              platform::to_lower(platform::normalize_path(path)));
}

// Calls `visit(name, attributes)` for every entry of `folder` (without "." and "..").
void list_folder(const std::wstring& folder, bool directories_only,
                 const std::function<void(const std::wstring&, DWORD)>& visit) {
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = (folder.size() > 248 ? L"\\\\?\\" + folder : folder) + L"\\*";
    wil::unique_hfind find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                              directories_only ? FindExSearchLimitToDirectories : FindExSearchNameMatch,
                                              nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        return;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if (directories_only && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        visit(data.cFileName, data.dwFileAttributes);
    } while (::FindNextFileW(find.get(), &data));
}

// BCU IsSystemDirectory, plus Avel's rule never to enter reparse points (junctions, symlinks).
bool skip_folder(const std::wstring& path, const std::wstring& name, DWORD attributes, const std::wstring& windows) {
    return (attributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
           is_blacklisted_folder_name(name) || (!windows.empty() && platform::path_is_under(path, windows));
}

void walk_folder(Snapshots& snapshots, const std::wstring& folder, int level, const UserScope* user,
                 const std::wstring& windows) {
    const bool parent_known = is_known(snapshots, folder);
    const std::wstring parent_name(platform::last_path_component(folder));
    list_folder(folder, true, [&](const std::wstring& name, DWORD attributes) {
        const std::wstring path = folder + L"\\" + name;
        if (skip_folder(path, name, attributes, windows)) {
            return;
        }
        snapshots.folders.push_back({path, name, parent_name, level, is_known(snapshots, path), parent_known, user});
        if (level < kMaxLevel) {
            walk_folder(snapshots, path, level + 1, user, windows);
        }
    });
}

void take_folders(const ScanEnvironment& environment, Snapshots& snapshots) {
    std::vector<std::pair<std::wstring, const UserScope*>> roots;
    const auto add_root = [&](const std::wstring& path, const UserScope* user) {
        if (path.empty() || std::any_of(roots.begin(), roots.end(),
                                        [&](const auto& root) { return platform::paths_equal(root.first, path); })) {
            return;
        }
        roots.emplace_back(platform::normalize_path(path), user);
    };
    const auto& machine = environment.machine_folders;
    for (const auto* path : {&machine.program_files, &machine.program_files_x86, &machine.program_data,
                             &machine.common_programs}) {
        add_root(*path, nullptr);
    }
    for (const auto& user : environment.users) {
        const auto& folders = user.folders;
        for (const auto* path : {&folders.roaming_app_data, &folders.local_app_data, &folders.local_low_app_data,
                                 &folders.programs}) {
            add_root(*path, &user);
        }
        // Programs that wrote to Program Files without rights were redirected here.
        if (!folders.local_app_data.empty()) {
            const std::wstring store = folders.local_app_data + LR"(\VirtualStore)";
            list_folder(store, true, [&](const std::wstring& name, DWORD attributes) {
                if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    add_root(store + L"\\" + name, &user);
                }
            });
        }
    }
    for (const auto& [root, user] : roots) {
        if (::GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) {
            walk_folder(snapshots, root, 0, user, machine.windows);
        }
    }
}

HKEY hive_root(const ScanEnvironment& environment, const platform::RegistryAddress& address) {
    if (address.hive == platform::Hive::Machine) {
        return environment.machine;
    }
    for (const auto& user : environment.users) {
        if (platform::iequals(user.profile.sid, address.sid)) {
            return user.hive;
        }
    }
    return nullptr;
}

std::wstring expand_for(const UserScope* user, std::wstring_view text) {
    return user ? platform::expand_environment_for_user(text, user->profile) : platform::expand_environment(text);
}

// .NET (and so BCU) expands REG_EXPAND_SZ values when reading them; so does this, for the key's owner.
void collect_values(const platform::RegistryKey& key, KeyNode& node) {
    for (const auto& value : key.values()) {
        const auto raw = value.as_string();
        if (!raw || raw->empty()) {
            continue;
        }
        std::wstring text = value.type == REG_EXPAND_SZ ? expand_for(node.user, *raw) : *raw;
        if (value.name.empty()) {
            node.default_value = std::move(text);
        } else if (in_list(kFolderValueNames, value.name)) {
            node.folder_values.push_back(std::move(text));
        } else if (in_list(kFileValueNames, value.name)) {
            node.file_values.push_back(std::move(text));
        } else if (in_list(kFileOrFolderValueNames, value.name)) {
            node.file_or_folder_values.push_back(std::move(text));
        }
    }
}

void walk_key(Snapshots& snapshots, const platform::RegistryKey& key, const platform::RegistryAddress& address,
              const std::wstring& relative_path, int level, std::size_t root, const UserScope* user) {
    const std::wstring parent_name(platform::last_path_component(address.path));
    for (const auto& name : key.subkey_names()) {
        if (is_blacklisted_key_name(name)) {
            continue;
        }
        platform::RegistryKey subkey;
        if (platform::RegistryKey::open(key.get(), name, kRead, subkey) != ERROR_SUCCESS) {
            continue;
        }
        KeyNode node;
        node.address = {address.hive, address.sid, address.path + L"\\" + name};
        node.name = name;
        node.parent_name = parent_name;
        node.level = level;
        node.root = root;
        node.relative_path = relative_path.empty() ? name : relative_path + L"\\" + name;
        node.user = user;
        collect_values(subkey, node);
        snapshots.key_index.emplace(key_index_name(root, node.relative_path), snapshots.keys.size());
        snapshots.keys.push_back(node);
        if (level < kMaxLevel) {
            walk_key(snapshots, subkey, node.address, node.relative_path, level + 1, root, user);
        }
    }
}

void take_keys(const ScanEnvironment& environment, Snapshots& snapshots) {
    constexpr std::array<std::wstring_view, 4> kRoots = {
        L"SOFTWARE", LR"(SOFTWARE\WOW6432Node)", LR"(SOFTWARE\Classes\VirtualStore\MACHINE\SOFTWARE)",
        LR"(SOFTWARE\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node)"};
    for (const auto root : kRoots) {
        snapshots.key_roots.push_back({{platform::Hive::Machine, L"", std::wstring(root)}, nullptr});
    }
    for (const auto& user : environment.users) {
        if (!user.hive) {
            continue;
        }
        for (const auto root : kRoots) {
            // A user's hive spells it "Software"; the case is only cosmetic.
            std::wstring path = L"Software" + std::wstring(root.substr(8));
            snapshots.key_roots.push_back({{platform::Hive::User, user.profile.sid, std::move(path)}, &user});
        }
    }
    for (std::size_t i = 0; i < snapshots.key_roots.size(); ++i) {
        const auto& root = snapshots.key_roots[i];
        platform::RegistryKey key;
        if (open_key(environment, root.address, key) == ERROR_SUCCESS) {
            walk_key(snapshots, key, root.address, L"", 0, i, root.user);
        }
    }
}

std::wstring resolve_shortcut(const std::wstring& file) {
    wil::com_ptr_nothrow<IShellLinkW> link;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) {
        return {};
    }
    const auto persist = link.try_query<IPersistFile>();
    if (!persist || FAILED(persist->Load(file.c_str(), STGM_READ))) {
        return {};
    }
    wchar_t target[MAX_PATH] = {};
    // The raw path keeps %VARIABLES% so they can be expanded for the shortcut's owner. Advertised
    // (Windows Installer) shortcuts have no path and are skipped, as in BCU.
    if (link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH) != S_OK) {
        return {};
    }
    return target;
}

void collect_shortcuts(Snapshots& snapshots, const std::wstring& folder, bool recursive, const UserScope* user,
                       const std::wstring& windows) {
    if (folder.empty()) {
        return;
    }
    list_folder(folder, false, [&](const std::wstring& name, DWORD attributes) {
        const std::wstring path = folder + L"\\" + name;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                collect_shortcuts(snapshots, path, true, user, windows);
            }
            return;
        }
        if (!platform::iends_with(name, L".lnk")) {
            return;
        }
        const std::wstring raw = resolve_shortcut(path);
        if (raw.empty()) {
            return;
        }
        const std::wstring target = platform::normalize_path(
            user ? platform::expand_environment_for_user(raw, user->profile) : platform::expand_environment(raw));
        if (target.empty() || (!windows.empty() && platform::path_is_under(target, windows))) {
            return;
        }
        snapshots.shortcuts.push_back({path, target, user});
    });
}

void take_shortcuts(const ScanEnvironment& environment, Snapshots& snapshots) {
    const auto& windows = environment.machine_folders.windows;
    collect_shortcuts(snapshots, environment.machine_folders.common_programs, true, nullptr, windows);
    collect_shortcuts(snapshots, environment.machine_folders.common_desktop, false, nullptr, windows);
    for (const auto& user : environment.users) {
        collect_shortcuts(snapshots, user.folders.programs, true, &user, windows);
        collect_shortcuts(snapshots, user.folders.desktop, false, &user, windows);
    }
}

struct ReferenceSource {
    platform::RegistryAddress key;
    const UserScope* user;
};

// The machine's key and each user's copy, in both views: "SOFTWARE\<path>" and
// "SOFTWARE\WOW6432Node\<path>".
std::vector<ReferenceSource> software_keys(const ScanEnvironment& environment, std::wstring_view path,
                                           bool include_users) {
    std::vector<ReferenceSource> keys;
    for (const auto prefix : {LR"(SOFTWARE\)", LR"(SOFTWARE\WOW6432Node\)"}) {
        keys.push_back({{platform::Hive::Machine, L"", prefix + std::wstring(path)}, nullptr});
    }
    if (include_users) {
        for (const auto& user : environment.users) {
            for (const auto prefix : {LR"(Software\)", LR"(Software\WOW6432Node\)"}) {
                keys.push_back({{platform::Hive::User, user.profile.sid, prefix + std::wstring(path)}, &user});
            }
        }
    }
    return keys;
}

platform::Expander expander_for(const UserScope* user) {
    if (!user) {
        return {};
    }
    return [user](std::wstring_view text) { return platform::expand_environment_for_user(text, user->profile); };
}

void take_references(const ScanEnvironment& environment, Snapshots& snapshots) {
    // Some of these keys are shared by the 32- and 64-bit views (App Paths is), so the
    // WOW6432Node path is another name for the same key: each key is read once.
    std::set<std::wstring> seen;
    const auto first_time = [&](const platform::RegistryKey& key) {
        const std::wstring name = key.kernel_name();
        return name.empty() || seen.insert(platform::to_lower(name)).second;
    };
    // Run and RunOnce: values whose data is a command line.
    for (const bool run_once : {false, true}) {
        const auto path = run_once ? LR"(Microsoft\Windows\CurrentVersion\RunOnce)" : LR"(Microsoft\Windows\CurrentVersion\Run)";
        for (const auto& source : software_keys(environment, path, true)) {
            platform::RegistryKey key;
            if (open_key(environment, source.key, key) != ERROR_SUCCESS || !first_time(key)) {
                continue;
            }
            for (const auto& value : key.values()) {
                if (const auto text = value.as_string()) {
                    auto paths = platform::referenced_paths(*text, expander_for(source.user));
                    snapshots.references.push_back({RegistryValueLocation{source.key, value.name},
                                                    ReferenceKind::RunValue, run_once, std::move(paths), source.user});
                }
            }
        }
    }
    // App Paths: a key per program file; its default value is the file, "Path" its folder.
    for (const auto& source : software_keys(environment, LR"(Microsoft\Windows\CurrentVersion\App Paths)", true)) {
        platform::RegistryKey key;
        if (open_key(environment, source.key, key) != ERROR_SUCCESS || !first_time(key)) {
            continue;
        }
        for (const auto& name : key.subkey_names()) {
            platform::RegistryKey app;
            if (platform::RegistryKey::open(key.get(), name, kRead, app) != ERROR_SUCCESS) {
                continue;
            }
            std::vector<std::wstring> paths;
            const auto values = app.values();
            for (const auto value_name : {L"", L"Path"}) {
                if (const auto* value = platform::find_value(values, value_name)) {
                    if (const auto text = value->as_string(); text && !text->empty()) {
                        paths.push_back(platform::normalize_path(expand_for(source.user, *text)));
                    }
                }
            }
            platform::RegistryAddress address = source.key;
            address.path += L"\\" + name;
            snapshots.references.push_back(
                {RegistryKeyLocation{address}, ReferenceKind::AppPath, false, std::move(paths), source.user});
        }
    }
    // SharedDLLs and Windows Installer's Folders list: the value names are the paths.
    const std::pair<std::wstring_view, ReferenceKind> name_lists[] = {
        {LR"(Microsoft\Windows\CurrentVersion\SharedDLLs)", ReferenceKind::SharedDll},
        {LR"(Microsoft\Windows\CurrentVersion\Installer\Folders)", ReferenceKind::InstallerFolder}};
    for (const auto& [path, kind] : name_lists) {
        for (const auto& source : software_keys(environment, path, false)) {
            platform::RegistryKey key;
            if (open_key(environment, source.key, key) != ERROR_SUCCESS || !first_time(key)) {
                continue;
            }
            for (const auto& value : key.values()) {
                if (platform::is_absolute_path(value.name)) {
                    snapshots.references.push_back({RegistryValueLocation{source.key, value.name}, kind, false,
                                                    {platform::normalize_path(value.name)}, nullptr});
                }
            }
        }
    }
    // Explorer's MuiCache: value names are "<program file>.FriendlyAppName" and similar. It lives
    // in the user's classes hive, reachable through their hive only while they are signed in.
    for (const auto& user : environment.users) {
        if (!user.hive) {
            continue;
        }
        const platform::RegistryAddress address{
            platform::Hive::User, user.profile.sid,
            LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache)"};
        platform::RegistryKey key;
        if (open_key(environment, address, key) != ERROR_SUCCESS) {
            continue;
        }
        for (const auto& value : key.values()) {
            const size_t dot = value.name.find_last_of(L'.');
            if (dot == std::wstring::npos) {
                continue;
            }
            const std::wstring file = value.name.substr(0, dot);
            if (platform::is_absolute_path(file)) {
                snapshots.references.push_back({RegistryValueLocation{address, value.name}, ReferenceKind::MuiCache,
                                                false, {platform::normalize_path(file)}, &user});
            }
        }
    }
}

}  // namespace

std::wstring key_index_name(std::size_t root, std::wstring_view relative_path) {
    return std::to_wstring(root) + L"|" + platform::to_lower(relative_path);
}

LSTATUS open_key(const ScanEnvironment& environment, const platform::RegistryAddress& address,
                 platform::RegistryKey& key) {
    const HKEY root = hive_root(environment, address);
    if (!root) {
        return ERROR_FILE_NOT_FOUND;
    }
    return platform::RegistryKey::open(root, address.path, kRead, key);
}

Snapshots take_snapshots(const ScanEnvironment& environment) {
    Snapshots snapshots;
    const auto add_known = [&](const std::vector<std::wstring>& folders) {
        for (const auto& folder : folders) {
            snapshots.known_folders.push_back(platform::to_lower(platform::normalize_path(folder)));
        }
    };
    add_known(environment.machine_folders.all);
    for (const auto& user : environment.users) {
        add_known(user.folders.all);
    }
    std::sort(snapshots.known_folders.begin(), snapshots.known_folders.end());
    const auto timed = [&](std::string_view stage, void (*take)(const ScanEnvironment&, Snapshots&)) {
        const auto started = std::chrono::steady_clock::now();
        take(environment, snapshots);
        snapshots.timings.push_back({stage, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - started)});
    };
    // COM classes are the largest snapshot (tens of thousands of keys) and only read the
    // registry, so they are read on their own thread while the rest proceeds. Its time overlaps
    // the other stages'.
    auto com = std::async(std::launch::async, [&environment] {
        Snapshots part;
        const auto started = std::chrono::steady_clock::now();
        take_com_classes(environment, part);
        part.timings.push_back({"com", std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - started)});
        return part;
    });
    timed("folders", take_folders);
    timed("keys", take_keys);
    timed("shortcuts", take_shortcuts);
    timed("references", take_references);
    timed("services", take_services);
    timed("tasks", take_tasks);
    Snapshots part = com.get();
    snapshots.com_classes = std::move(part.com_classes);
    snapshots.class_references = std::move(part.class_references);
    snapshots.timings.push_back(part.timings.front());
    return snapshots;
}

}  // namespace avel::leftovers::detail
