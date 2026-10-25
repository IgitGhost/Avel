// Scans this machine's programs for leftovers (read-only) and checks every result against an
// oracle built independently of HardFloor and ProtectedPolicy: every known folder Windows
// reports (all categories), every user profile, the Windows folder, drive roots and the registry
// keys Windows relies on. "Zero hits in protected areas" is a Phase 3 acceptance criterion.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::leftovers {
namespace {

std::vector<std::wstring> all_known_folders() {
    std::vector<std::wstring> folders;
    wil::com_ptr_nothrow<IKnownFolderManager> manager;
    if (FAILED(::CoCreateInstance(CLSID_KnownFolderManager, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(manager.put())))) {
        return folders;
    }
    KNOWNFOLDERID* ids = nullptr;
    UINT count = 0;
    if (FAILED(manager->GetFolderIds(&ids, &count))) {
        return folders;
    }
    for (UINT i = 0; i < count; ++i) {
        wil::com_ptr_nothrow<IKnownFolder> folder;
        wil::unique_cotaskmem_string path;
        if (SUCCEEDED(manager->GetFolder(ids[i], folder.put())) &&
            SUCCEEDED(folder->GetPath(KF_FLAG_DONT_VERIFY, &path)) && path && platform::is_absolute_path(path.get())) {
            folders.emplace_back(path.get());
        }
    }
    ::CoTaskMemFree(ids);
    return folders;
}

std::vector<std::wstring> profile_folders() {
    std::vector<std::wstring> folders;
    platform::RegistryKey list;
    if (platform::RegistryKey::open(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList)",
                                    KEY_READ, list) != ERROR_SUCCESS) {
        return folders;
    }
    for (const auto& sid : list.subkey_names()) {
        platform::RegistryKey profile;
        if (platform::RegistryKey::open(list.get(), sid, KEY_READ, profile) == ERROR_SUCCESS) {
            const auto values = profile.values();
            if (const auto* path = platform::find_value(values, L"ProfileImagePath")) {
                folders.push_back(platform::expand_environment(path->as_string().value_or(L"")));
            }
        }
    }
    return folders;
}

// Returns why `location` must never be reported, or an empty string.
std::string violation(const Location& location, const std::wstring& windows, const std::vector<std::wstring>& keep) {
    if (const auto* file = std::get_if<FileLocation>(&location)) {
        if (!platform::is_absolute_path(file->path) || platform::is_root_path(file->path)) {
            return "not a removable path";
        }
        if (platform::path_is_under(file->path, windows)) {
            return "inside the Windows folder";
        }
        for (const auto& folder : keep) {
            if (platform::path_is_under(folder, file->path)) {
                return "is or contains " + platform::to_utf8(folder);
            }
        }
        return {};
    }
    const platform::RegistryAddress* key = nullptr;
    if (const auto* item = std::get_if<RegistryKeyLocation>(&location)) {
        key = &item->key;
    }
    if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        if (value->key.hive == platform::Hive::Machine && platform::istarts_with(value->key.path, L"SYSTEM\\")) {
            return "a SYSTEM value";
        }
        return {};
    }
    if (!key) {
        return {};
    }
    if (key->path.find(L'\\') == std::wstring::npos) {
        return "a top-level key";
    }
    if (key->hive == platform::Hive::Machine && platform::istarts_with(key->path, L"SYSTEM\\")) {
        return "a SYSTEM key";
    }
    for (const auto core : {LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)",
                            LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
                            LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths)", LR"(SOFTWARE\Classes\CLSID)",
                            LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)"}) {
        const std::wstring_view core_path = core;
        if (platform::istarts_with(core_path, key->path) &&
            (core_path.size() == key->path.size() || core_path[key->path.size()] == L'\\')) {
            return "is or contains " + platform::to_utf8(core_path);
        }
    }
    return {};
}

TEST(LeftoverSmokeTest, NothingReportedForThisMachineTouchesProtectedAreas) {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize = wil::scope_exit([&] {
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });

    const auto inventory = inventory::scan_inventory();
    std::vector<ScanTarget> targets;
    for (const auto& program : inventory.programs) {
        targets.push_back(make_scan_target(program));
    }
    const LiveEnvironment environment;
    const auto result = scan_leftovers(environment.environment(), targets, inventory.programs);

    wchar_t windows[MAX_PATH] = {};
    ASSERT_GT(::GetWindowsDirectoryW(windows, MAX_PATH), 0u);
    std::vector<std::wstring> keep = all_known_folders();
    const auto profiles = profile_folders();
    keep.insert(keep.end(), profiles.begin(), profiles.end());
    ASSERT_GT(keep.size(), 50u);

    std::size_t violations = 0;
    for (const auto& item : result.leftovers) {
        const std::string why = violation(item.location, windows, keep);
        if (!why.empty()) {
            ++violations;
            ADD_FAILURE() << "protected area reported (" << why << ") by " << to_string(item.found_by);
        }
    }
    EXPECT_EQ(violations, 0u);
    std::cout << "  " << targets.size() << " programs, " << result.leftovers.size() << " leftovers, "
              << result.protected_count << " protected matches dropped, " << result.elapsed.count() << " ms\n";
    EXPECT_LT(result.elapsed, std::chrono::seconds(20));
}

}  // namespace
}  // namespace avel::leftovers
