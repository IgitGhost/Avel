#include "core/platform/file_ops.h"

#include <sddl.h>

#include <cstring>
#include <cwchar>
#include <vector>

#include "core/platform/elevation.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::platform {

namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr SECURITY_INFORMATION kSecurityParts =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

bool same_sid(PSID a, PSID b) {
    return (a == nullptr) == (b == nullptr) && (a == nullptr || ::EqualSid(a, b));
}

std::vector<std::byte> read_security(HANDLE item) {
    DWORD size = 0;
    ::GetKernelObjectSecurity(item, kSecurityParts, nullptr, 0, &size);
    std::vector<std::byte> security(size);
    if (size == 0 || !::GetKernelObjectSecurity(item, kSecurityParts, security.data(), size, &size)) {
        return {};
    }
    return security;
}

}  // namespace

std::wstring extended(const std::wstring& path) {
    if (path.size() < 248 || path.starts_with(LR"(\\?\)")) {
        return path;
    }
    if (path.starts_with(LR"(\\)")) {
        return LR"(\\?\UNC\)" + path.substr(2);
    }
    return LR"(\\?\)" + path;
}

wil::unique_hfile open_item(const std::wstring& path, DWORD access) {
    return wil::unique_hfile(::CreateFileW(extended(path).c_str(), access, kShareAll, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

std::optional<std::wstring> final_path(HANDLE item) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD length = ::GetFinalPathNameByHandleW(item, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            if (buffer.starts_with(LR"(\\?\UNC\)")) {
                buffer = LR"(\\)" + buffer.substr(8);
            } else if (buffer.starts_with(LR"(\\?\)")) {
                buffer.erase(0, 4);
            }
            return normalize_path(buffer);
        }
        buffer.resize(length + 1);
    }
    return std::nullopt;
}

DWORD rename_item(HANDLE item, const std::wstring& destination) {
    // An NT path ("\??\C:\...") works whether or not Windows translates the name, and has no
    // length limit.
    const std::wstring name = LR"(\??\)" + destination;
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
    if (!::SetFileInformationByHandle(item, FileRenameInfo, info, static_cast<DWORD>(buffer.size()))) {
        return ::GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD delete_tree(const std::wstring& path) {
    const std::wstring long_name = extended(path);
    DWORD attributes = ::GetFileAttributesW(long_name.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        attributes &= ~FILE_ATTRIBUTE_READONLY;
        ::SetFileAttributesW(long_name.c_str(), attributes == 0 ? FILE_ATTRIBUTE_NORMAL : attributes);
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return ::DeleteFileW(long_name.c_str()) ? ERROR_SUCCESS : ::GetLastError();  // A file, or a link to one
    }
    DWORD result = ERROR_SUCCESS;
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {  // A junction is removed below, never entered
        WIN32_FIND_DATAW data{};
        wil::unique_hfind find(::FindFirstFileExW(extended(path + L"\\*").c_str(), FindExInfoBasic, &data,
                                                  FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (find) {
            do {
                const std::wstring_view name = data.cFileName;
                if (name != L"." && name != L"..") {
                    if (const DWORD error = delete_tree(path + L"\\" + data.cFileName); error != ERROR_SUCCESS) {
                        result = error;
                    }
                }
            } while (::FindNextFileW(find.get(), &data));
        }
    }
    if (!::RemoveDirectoryW(long_name.c_str())) {
        return result != ERROR_SUCCESS ? result : ::GetLastError();
    }
    return ERROR_SUCCESS;
}

std::optional<std::wstring> volume_root(const std::wstring& path) {
    wchar_t buffer[MAX_PATH + 1] = {};
    if (!::GetVolumePathNameW(extended(path).c_str(), buffer, MAX_PATH + 1)) {
        return std::nullopt;
    }
    std::wstring root = buffer;
    if (root.starts_with(LR"(\\?\)") && !root.starts_with(LR"(\\?\Volume)")) {
        root.erase(0, 4);
    }
    return root;
}

bool is_fixed_ntfs(const std::wstring& volume_root) {
    if (::GetDriveTypeW(volume_root.c_str()) != DRIVE_FIXED) {
        return false;
    }
    wchar_t file_system[MAX_PATH + 1] = {};
    return ::GetVolumeInformationW(volume_root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, file_system,
                                   MAX_PATH + 1) &&
           iequals(file_system, L"NTFS");
}

std::vector<std::wstring> fixed_ntfs_volumes() {
    std::vector<std::wstring> roots;
    wchar_t buffer[512] = {};
    const DWORD length = ::GetLogicalDriveStringsW(511, buffer);
    for (const wchar_t* root = buffer; root < buffer + length && *root; root += std::wcslen(root) + 1) {
        if (is_fixed_ntfs(root)) {
            roots.emplace_back(root);
        }
    }
    return roots;
}

std::optional<ItemInfo> item_info(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(extended(path).c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    return ItemInfo{data.dwFileAttributes, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime,
                    (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
}

std::wstring security_sddl(const std::wstring& path) {
    const auto item = open_item(path, READ_CONTROL);
    const auto security = item ? read_security(item.get()) : std::vector<std::byte>();
    wil::unique_hlocal_string sddl;
    if (security.empty() || !::ConvertSecurityDescriptorToStringSecurityDescriptorW(
                                const_cast<std::byte*>(security.data()), SDDL_REVISION_1, kSecurityParts, &sddl,
                                nullptr)) {
        return {};
    }
    return sddl.get();
}

bool apply_security_sddl(const std::wstring& path, const std::wstring& sddl) {
    wil::unique_hlocal_security_descriptor wanted;
    if (sddl.empty() || !::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                            sddl.c_str(), SDDL_REVISION_1, wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(wanted), nullptr)) {
        return false;
    }
    if (security_sddl(path) == sddl) {
        return true;
    }
    // The DACL exactly as recorded: SetKernelObjectSecurity doesn't recompute inheritance, so
    // inherited ACEs come back marked inherited, as they were.
    const auto item = open_item(path, READ_CONTROL | WRITE_DAC);
    if (!item || !::SetKernelObjectSecurity(item.get(), DACL_SECURITY_INFORMATION, wanted.get())) {
        return false;
    }
    PSID wanted_owner = nullptr;
    PSID wanted_group = nullptr;
    BOOL defaulted = FALSE;
    ::GetSecurityDescriptorOwner(wanted.get(), &wanted_owner, &defaulted);
    ::GetSecurityDescriptorGroup(wanted.get(), &wanted_group, &defaulted);
    const auto current = read_security(item.get());
    PSID current_owner = nullptr;
    PSID current_group = nullptr;
    if (!current.empty()) {
        ::GetSecurityDescriptorOwner(const_cast<std::byte*>(current.data()), &current_owner, &defaulted);
        ::GetSecurityDescriptorGroup(const_cast<std::byte*>(current.data()), &current_group, &defaulted);
    }
    SECURITY_INFORMATION parts = 0;
    if (!same_sid(wanted_owner, current_owner)) {
        enable_privilege(SE_RESTORE_NAME);  // Needed to make someone else the owner
        parts |= OWNER_SECURITY_INFORMATION;
    }
    if (!same_sid(wanted_group, current_group)) {
        parts |= GROUP_SECURITY_INFORMATION;
    }
    if (parts != 0) {
        const auto owner_handle = open_item(path, WRITE_OWNER);
        return owner_handle && ::SetKernelObjectSecurity(owner_handle.get(), parts, wanted.get());
    }
    return true;
}

}  // namespace avel::platform
