#include "core/platform/known_folders.h"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <string_view>

#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::platform {

namespace {

constexpr wchar_t kUserShellFolders[] = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)";
// FOLDERID_LocalAppDataLow, named by its GUID in User Shell Folders when it is redirected.
constexpr wchar_t kLocalLowValue[] = L"{A520A1A4-1780-4FF6-BD18-167343C5AF16}";

void add_unique(std::vector<std::wstring>& list, const std::wstring& path) {
    if (path.empty() || std::any_of(list.begin(), list.end(), [&](const auto& item) { return iequals(item, path); })) {
        return;
    }
    list.push_back(path);
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    wil::unique_cotaskmem_string path;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path))) {
        return {};
    }
    return normalize_path(path.get());
}

}  // namespace

UserFolders user_folders(HKEY user_hive, const UserProfile& profile) {
    UserFolders folders;
    folders.profile = normalize_path(profile.profile_path);

    std::vector<RegistryValue> values;
    RegistryKey shell_folders;
    if (user_hive && RegistryKey::open(user_hive, kUserShellFolders, KEY_READ, shell_folders) == ERROR_SUCCESS) {
        values = shell_folders.values();
    }
    const auto folder = [&](std::wstring_view name, std::wstring_view fallback) {
        if (const auto* value = find_value(values, name)) {
            const std::wstring path = normalize_path(expand_environment_for_user(value->as_string().value_or(L""), profile));
            if (!path.empty()) {
                return path;
            }
        }
        return folders.profile.empty() ? std::wstring() : normalize_path(folders.profile + L"\\" + std::wstring(fallback));
    };

    folders.roaming_app_data = folder(L"AppData", LR"(AppData\Roaming)");
    folders.local_app_data = folder(L"Local AppData", LR"(AppData\Local)");
    folders.local_low_app_data = folder(kLocalLowValue, LR"(AppData\LocalLow)");
    folders.desktop = folder(L"Desktop", L"Desktop");
    folders.programs = folder(L"Programs", LR"(AppData\Roaming\Microsoft\Windows\Start Menu\Programs)");

    add_unique(folders.all, folders.profile);
    if (!folders.profile.empty()) {
        add_unique(folders.all, normalize_path(folders.profile + LR"(\AppData)"));
    }
    for (const auto* path : {&folders.roaming_app_data, &folders.local_app_data, &folders.local_low_app_data,
                             &folders.desktop, &folders.programs}) {
        add_unique(folders.all, *path);
    }
    for (const auto& value : values) {
        if (const auto text = value.as_string()) {
            add_unique(folders.all, normalize_path(expand_environment_for_user(*text, profile)));
        }
    }
    return folders;
}

MachineFolders machine_folders() {
    // The caller may already have COM (in either apartment); only balance what this call starts.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize = wil::scope_exit([&] {
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });

    MachineFolders folders;
    folders.windows = known_folder(FOLDERID_Windows);
    folders.program_files = known_folder(FOLDERID_ProgramFilesX64);
    if (folders.program_files.empty()) {
        folders.program_files = known_folder(FOLDERID_ProgramFiles);
    }
    folders.program_files_x86 = known_folder(FOLDERID_ProgramFilesX86);
    folders.program_data = known_folder(FOLDERID_ProgramData);
    folders.common_programs = known_folder(FOLDERID_CommonPrograms);
    folders.common_desktop = known_folder(FOLDERID_PublicDesktop);
    folders.users = known_folder(FOLDERID_UserProfiles);

    for (const auto* path : {&folders.windows, &folders.program_files, &folders.program_files_x86,
                             &folders.program_data, &folders.common_programs, &folders.common_desktop,
                             &folders.users}) {
        add_unique(folders.all, *path);
    }

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
    const auto free_ids = wil::scope_exit([&] { ::CoTaskMemFree(ids); });
    for (UINT i = 0; i < count; ++i) {
        wil::com_ptr_nothrow<IKnownFolder> folder;
        KF_CATEGORY category{};
        if (FAILED(manager->GetFolder(ids[i], folder.put())) || FAILED(folder->GetCategory(&category))) {
            continue;
        }
        if (category != KF_CATEGORY_FIXED && category != KF_CATEGORY_COMMON) {
            continue;  // Per-user folders come from each user's hive; virtual ones have no path
        }
        wil::unique_cotaskmem_string path;
        if (SUCCEEDED(folder->GetPath(KF_FLAG_DONT_VERIFY, &path)) && path) {
            add_unique(folders.all, normalize_path(path.get()));
        }
    }
    return folders;
}

}  // namespace avel::platform
