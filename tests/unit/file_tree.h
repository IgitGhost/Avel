// Temporary folder trees for file tests (see tree_dump.h for comparing them).
#pragma once

#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <sddl.h>

#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "core/platform/text.h"
#include "tree_dump.h"

namespace avel::testing {

class TempFolder {
public:
    TempFolder() {
        GUID guid{};
        ::CoCreateGuid(&guid);
        path_ = (std::filesystem::temp_directory_path() / std::format(L"avel-test-{:08x}", guid.Data1)).wstring();
        std::filesystem::create_directories(path_);
    }
    ~TempFolder() {
        // Links are removed as links (std::filesystem::remove_all doesn't follow them).
        std::error_code ignored;
        std::filesystem::remove_all(std::filesystem::path(long_name(path_)), ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::wstring& path() const { return path_; }
    std::wstring operator/(std::wstring_view relative) const { return path_ + L"\\" + std::wstring(relative); }

private:
    std::wstring path_;
};

inline void make_folder(const std::wstring& path) {
    std::vector<std::wstring> parts;
    for (std::wstring current = path; !current.empty() && ::GetFileAttributesW(long_name(current).c_str()) ==
                                                              INVALID_FILE_ATTRIBUTES;) {
        parts.push_back(current);
        const auto slash = current.find_last_of(L'\\');
        if (slash == std::wstring::npos) {
            break;
        }
        current.resize(slash);
    }
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        ::CreateDirectoryW(long_name(*it).c_str(), nullptr);
    }
}

inline void write_file(const std::wstring& path, std::string_view content) {
    make_folder(path.substr(0, path.find_last_of(L'\\')));
    wil::unique_hfile file(::CreateFileW(long_name(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(file) << ::GetLastError();
    DWORD written = 0;
    ASSERT_TRUE(::WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
}

// A directory junction made with mklink, which any user may create.
inline bool make_junction(const std::wstring& link, const std::wstring& target) {
    std::wstring command = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &process)) {
        return false;
    }
    ::WaitForSingleObject(process.hProcess, 10000);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return (::GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

inline void set_sddl(const std::wstring& path, const std::wstring& sddl) {
    wil::unique_hlocal_security_descriptor descriptor;
    ASSERT_TRUE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(), SDDL_REVISION_1, wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(descriptor), nullptr));
    ASSERT_TRUE(::SetFileSecurityW(long_name(path).c_str(), DACL_SECURITY_INFORMATION, descriptor.get()));
}

inline std::wstring current_user_sid_text() {
    wil::unique_handle token;
    ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put());
    std::vector<std::byte> buffer(256);
    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &size);
    wil::unique_hlocal_string text;
    ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    return text.get();
}

}  // namespace avel::testing
