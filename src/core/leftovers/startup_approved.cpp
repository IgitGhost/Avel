#include "core/leftovers/startup_approved.h"

#include <array>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "core/platform/text.h"

namespace avel::leftovers {

namespace {

constexpr std::wstring_view kApproved = LR"(Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)";

constexpr std::array kLists = {
    std::pair{ApprovalList::Run, std::wstring_view(L"Run")},
    std::pair{ApprovalList::Run32, std::wstring_view(L"Run32")},
    std::pair{ApprovalList::StartupFolder, std::wstring_view(L"StartupFolder")},
};

}  // namespace

platform::RegistryAddress approval_key(ApprovalList list, const std::wstring& sid) {
    std::wstring path = (sid.empty() ? L"SOFTWARE\\" : L"Software\\") + std::wstring(kApproved);
    for (const auto& [candidate, name] : kLists) {
        if (candidate == list) {
            path += name;
        }
    }
    return {sid.empty() ? platform::Hive::Machine : platform::Hive::User, sid, std::move(path)};
}

std::optional<ApprovalList> approval_list(const platform::RegistryAddress& key) {
    if (key.hive == platform::Hive::Machine ? !key.sid.empty()
                                            : key.sid.empty() || platform::iends_with(key.sid, L"_Classes")) {
        return std::nullopt;
    }
    for (const auto& [list, name] : kLists) {
        // Spelled SOFTWARE in the machine's hive and Software in a user's; registry paths ignore case.
        if (platform::iequals(key.path, L"Software\\" + std::wstring(kApproved) + std::wstring(name))) {
            return list;
        }
    }
    return std::nullopt;
}

bool approval_disabled(const platform::RawValue& value) {
    return value.type == REG_BINARY && !value.data.empty() &&
           (std::to_integer<unsigned>(value.data[0]) & 1) != 0;
}

platform::RawValue disabled_approval(std::wstring name, FILETIME when) {
    platform::RawValue value{std::move(name), REG_BINARY, std::vector<std::byte>(kApprovalSize)};
    value.data[0] = std::byte{0x03};
    std::memcpy(value.data.data() + 4, &when, sizeof(when));
    return value;
}

platform::RawValue enabled_approval(std::wstring name) {
    platform::RawValue value{std::move(name), REG_BINARY, std::vector<std::byte>(kApprovalSize)};
    value.data[0] = std::byte{0x02};
    return value;
}

bool is_approval_value(const platform::RawValue& value) {
    return value.type == REG_BINARY && value.data.size() == kApprovalSize &&
           (value.data[0] == std::byte{0x02} || value.data[0] == std::byte{0x03});
}

}  // namespace avel::leftovers
