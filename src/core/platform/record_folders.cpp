#include "core/platform/record_folders.h"

#include <objbase.h>
#include <shlobj.h>

#include <format>

#include "core/platform/file_ops.h"

namespace avel::platform {

std::int64_t unix_seconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_unix(std::int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

std::string iso_time(std::chrono::system_clock::time_point time) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(time));
}

std::wstring new_record_id(std::chrono::system_clock::time_point now) {
    GUID guid{};
    ::CoCreateGuid(&guid);
    return std::format(L"{:%Y%m%d-%H%M%S}-{:08x}", std::chrono::floor<std::chrono::seconds>(now), guid.Data1);
}

bool is_safe_record_id(std::wstring_view id) {
    return !id.empty() && id.find_first_of(L"\\/:") == std::wstring_view::npos && id != L"." && id != L"..";
}

wil::unique_hfile take_folder_lock(const std::wstring& folder) {
    return wil::unique_hfile(::CreateFileW(extended(folder + L"\\lock").c_str(), GENERIC_WRITE, 0, nullptr,
                                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                                           nullptr));
}

std::vector<std::wstring> record_folders(const std::wstring& root) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(::FindFirstFileExW(extended(root + L"\\*").c_str(), FindExInfoBasic, &data,
                                              FindExSearchLimitToDirectories, nullptr, 0));
    if (!find) {
        return names;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && name != L"." && name != L"..") {
            names.emplace_back(name);
        }
    } while (::FindNextFileW(find.get(), &data));
    return names;
}

std::wstring program_data_folder(std::wstring_view name) {
    wil::unique_cotaskmem_string program_data;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data))) {
        return {};
    }
    return std::wstring(program_data.get()) + LR"(\Avel\)" + std::wstring(name);
}

}  // namespace avel::platform
