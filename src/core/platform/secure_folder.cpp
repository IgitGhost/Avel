#include "core/platform/secure_folder.h"

#include <windows.h>
#include <sddl.h>

#include <wil/resource.h>

#include "core/platform/file_ops.h"
#include "core/platform/paths.h"

namespace avel::platform {

FolderPolicy admin_only_folder_policy(bool hidden) {
    return {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", {L"BA", L"SY"}, hidden};
}

std::string_view to_string(FolderStatus status) {
    switch (status) {
        case FolderStatus::Ok:
            return "ok";
        case FolderStatus::NotFolder:
            return "not-folder";
        case FolderStatus::ReparsePoint:
            return "reparse-point";
        case FolderStatus::Redirected:
            return "redirected";
        case FolderStatus::ForeignOwner:
            return "foreign-owner";
        case FolderStatus::Unavailable:
            return "unavailable";
    }
    return "unknown";
}

FolderStatus check_secure_folder(const std::wstring& path, const FolderPolicy& policy) {
    const auto folder = open_item(path, READ_CONTROL | FILE_READ_ATTRIBUTES);
    if (!folder) {
        return FolderStatus::Unavailable;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(folder.get(), &information)) {
        return FolderStatus::Unavailable;
    }
    if (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return FolderStatus::ReparsePoint;
    }
    if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return FolderStatus::NotFolder;
    }
    const auto real = final_path(folder.get());
    if (!real || !paths_equal(*real, long_path(path))) {
        return FolderStatus::Redirected;
    }

    DWORD size = 0;
    ::GetKernelObjectSecurity(folder.get(), OWNER_SECURITY_INFORMATION, nullptr, 0, &size);
    std::vector<std::byte> security(size);
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    if (size == 0 ||
        !::GetKernelObjectSecurity(folder.get(), OWNER_SECURITY_INFORMATION, security.data(), size, &size) ||
        !::GetSecurityDescriptorOwner(security.data(), &owner, &defaulted) || owner == nullptr) {
        return FolderStatus::Unavailable;
    }
    for (const auto& allowed : policy.owners) {
        // Two statements: out_param_ptr fills `sid` only at the end of the full expression.
        wil::unique_hlocal sid;
        const BOOL converted = ::ConvertStringSidToSidW(allowed.c_str(), wil::out_param_ptr<PSID*>(sid));
        if (converted && ::EqualSid(owner, sid.get())) {
            return FolderStatus::Ok;
        }
    }
    return FolderStatus::ForeignOwner;
}

FolderStatus ensure_secure_folder(const std::wstring& path, const FolderPolicy& policy) {
    const std::wstring normalized = normalize_path(path);
    if (::GetFileAttributesW(extended(normalized).c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Parents are ordinary folders; only the last one gets the policy.
        std::vector<std::wstring> missing;
        for (std::wstring parent = parent_path(normalized); !parent.empty() && !is_root_path(parent) &&
                                                            ::GetFileAttributesW(extended(parent).c_str()) ==
                                                                INVALID_FILE_ATTRIBUTES;
             parent = parent_path(parent)) {
            missing.push_back(parent);
        }
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            ::CreateDirectoryW(extended(*it).c_str(), nullptr);
        }
        wil::unique_hlocal_security_descriptor descriptor;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                policy.sddl.c_str(), SDDL_REVISION_1, wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(descriptor),
                nullptr)) {
            return FolderStatus::Unavailable;
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
        if (!::CreateDirectoryW(extended(normalized).c_str(), &attributes) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS) {
            return FolderStatus::Unavailable;
        }
        if (policy.hidden) {
            ::SetFileAttributesW(extended(normalized).c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        }
    }
    return check_secure_folder(normalized, policy);
}

}  // namespace avel::platform
