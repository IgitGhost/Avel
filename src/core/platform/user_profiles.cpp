#include "core/platform/user_profiles.h"

#include <windows.h>
#include <sddl.h>

#include <wil/resource.h>

#include <array>
#include <utility>

#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::platform {

namespace {

constexpr wchar_t kProfileListPath[] = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList)";

bool is_user_sid(std::wstring_view sid) {
    return istarts_with(sid, L"S-1-5-21-") || istarts_with(sid, L"S-1-12-1-");
}

// Users with a profile on this machine are normally answered from the local account database or
// LSA's logon cache, without contacting a domain controller.
std::wstring lookup_account_name(const std::wstring& sid_string) {
    wil::unique_hlocal_ptr<void> sid;
    if (!::ConvertStringSidToSidW(sid_string.c_str(), wil::out_param(sid))) {
        return {};
    }
    std::array<wchar_t, 256> name{};
    std::array<wchar_t, 256> domain{};
    DWORD name_length = static_cast<DWORD>(name.size());
    DWORD domain_length = static_cast<DWORD>(domain.size());
    SID_NAME_USE use{};
    if (!::LookupAccountSidW(nullptr, sid.get(), name.data(), &name_length, domain.data(), &domain_length,
                             &use)) {
        return {};
    }
    std::wstring result(domain.data(), domain_length);
    if (!result.empty()) {
        result += L'\\';
    }
    result.append(name.data(), name_length);
    return result;
}

// Replaces every case-insensitive occurrence of `token` (e.g. "%APPDATA%") with `replacement`.
void replace_token(std::wstring& text, std::wstring_view token, std::wstring_view replacement) {
    size_t position = 0;
    while (position + token.size() <= text.size()) {
        if (iequals(std::wstring_view(text).substr(position, token.size()), token)) {
            text.replace(position, token.size(), replacement);
            position += replacement.size();
        } else {
            ++position;
        }
    }
}

}  // namespace

std::vector<UserProfile> enumerate_user_profiles() {
    std::vector<UserProfile> profiles;
    RegistryKey profile_list;
    if (RegistryKey::open(HKEY_LOCAL_MACHINE, kProfileListPath, KEY_READ, profile_list) != ERROR_SUCCESS) {
        return profiles;
    }
    for (const auto& sid : profile_list.subkey_names()) {
        if (!is_user_sid(sid)) {
            continue;
        }
        UserProfile profile;
        profile.sid = sid;
        RegistryKey profile_key;
        if (RegistryKey::open(profile_list.get(), sid, KEY_READ, profile_key) == ERROR_SUCCESS) {
            const auto values = profile_key.values();
            if (const auto* path = find_value(values, L"ProfileImagePath")) {
                profile.profile_path = expand_environment(path->as_string().value_or(L""));
            }
        }
        RegistryKey hive;
        profile.hive_loaded = RegistryKey::open(HKEY_USERS, sid, KEY_READ, hive) == ERROR_SUCCESS;
        profile.account_name = lookup_account_name(sid);
        if (profile.account_name.empty()) {
            // Deleted or unreachable account: the profile folder name is the best hint left.
            const auto separator = profile.profile_path.find_last_of(L'\\');
            profile.account_name = separator == std::wstring::npos
                                       ? profile.profile_path
                                       : profile.profile_path.substr(separator + 1);
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::wstring current_user_sid() {
    wil::unique_handle token;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put())) {
        return {};
    }
    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (!::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
        return {};
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wil::unique_hlocal_string sid_string;
    if (!::ConvertSidToStringSidW(user->User.Sid, wil::out_param(sid_string))) {
        return {};
    }
    return sid_string.get();
}

std::wstring expand_environment(std::wstring_view text) {
    if (text.find(L'%') == std::wstring_view::npos) {
        return std::wstring(text);
    }
    const std::wstring input(text);
    const DWORD size = ::ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (size == 0) {
        return input;
    }
    std::wstring result(size, L'\0');
    const DWORD written = ::ExpandEnvironmentStringsW(input.c_str(), result.data(), size);
    if (written == 0 || written > size) {
        return input;
    }
    result.resize(written - 1);  // drop the terminator
    return result;
}

std::wstring expand_environment_for_user(std::wstring_view text, const UserProfile& profile) {
    std::wstring result(text);
    if (result.find(L'%') == std::wstring::npos || profile.profile_path.empty()) {
        return expand_environment(result);
    }
    const std::wstring& home = profile.profile_path;
    const std::wstring home_path = home.size() > 2 && home[1] == L':' ? home.substr(2) : home;
    replace_token(result, L"%USERPROFILE%", home);
    replace_token(result, L"%APPDATA%", home + LR"(\AppData\Roaming)");
    replace_token(result, L"%LOCALAPPDATA%", home + LR"(\AppData\Local)");
    replace_token(result, L"%HOMEPATH%", home_path);
    return expand_environment(result);
}

}  // namespace avel::platform
