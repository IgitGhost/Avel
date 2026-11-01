// Explorer's StartupApproved values, as Task Manager reads and writes them, and the three keys
// they live in. Pure: no registry.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <vector>

#include "core/leftovers/startup_approved.h"

namespace avel::leftovers {
namespace {

using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

RawValue binary(std::initializer_list<int> values, DWORD type = REG_BINARY) {
    RawValue value{L"Contoso", type, {}};
    for (const int byte : values) {
        value.data.push_back(static_cast<std::byte>(byte));
    }
    return value;
}

TEST(StartupApprovedTest, TheFirstBytesLowBitMeansDisabled) {
    // As seen on real machines: 02 and 06 enabled, 03 (Task Manager) and 01 disabled.
    EXPECT_FALSE(approval_disabled(binary({0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    EXPECT_FALSE(approval_disabled(binary({0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    EXPECT_TRUE(approval_disabled(binary({0x03, 0, 0, 0, 0x7f, 0x6e, 0x5d, 0x4c, 0x3b, 0x2a, 0xdb, 0x01})));
    EXPECT_TRUE(approval_disabled(binary({0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    // Not an approval at all: enabled, as Explorer treats a missing one.
    EXPECT_FALSE(approval_disabled(binary({})));
    EXPECT_FALSE(approval_disabled(binary({0x03, 0, 0, 0}, REG_DWORD)));
}

TEST(StartupApprovedTest, WritesWhatTaskManagerWrites) {
    const FILETIME when{0x4c5d6e7f, 0x01db2a3b};
    const RawValue disabled = disabled_approval(L"Contoso", when);
    EXPECT_EQ(disabled.name, L"Contoso");
    EXPECT_EQ(disabled.type, static_cast<DWORD>(REG_BINARY));
    EXPECT_EQ(disabled.data, binary({0x03, 0, 0, 0, 0x7f, 0x6e, 0x5d, 0x4c, 0x3b, 0x2a, 0xdb, 0x01}).data);
    EXPECT_TRUE(approval_disabled(disabled));
    EXPECT_TRUE(is_approval_value(disabled));

    const RawValue enabled = enabled_approval(L"Contoso");
    EXPECT_EQ(enabled.data, binary({0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}).data);
    EXPECT_FALSE(approval_disabled(enabled));
    EXPECT_TRUE(is_approval_value(enabled));

    // Avel writes nothing else: not 01 or 06, not another length or type.
    EXPECT_FALSE(is_approval_value(binary({0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    EXPECT_FALSE(is_approval_value(binary({0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
    EXPECT_FALSE(is_approval_value(binary({0x02, 0, 0, 0})));
    EXPECT_FALSE(is_approval_value(binary({0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, REG_NONE)));
}

TEST(StartupApprovedTest, KnowsItsThreeKeysInEveryHive) {
    constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
    for (const auto list : {ApprovalList::Run, ApprovalList::Run32, ApprovalList::StartupFolder}) {
        const RegistryAddress machine = approval_key(list, L"");
        const RegistryAddress user = approval_key(list, kSid);
        EXPECT_EQ(machine.hive, Hive::Machine);
        EXPECT_TRUE(machine.path.starts_with(
            LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)"));
        EXPECT_EQ(user.hive, Hive::User);
        EXPECT_EQ(user.sid, kSid);
        EXPECT_TRUE(
            user.path.starts_with(LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)"));
        EXPECT_EQ(approval_list(machine), list);
        EXPECT_EQ(approval_list(user), list);
    }
    EXPECT_EQ(approval_key(ApprovalList::Run32, L"").path,
              LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run32)");
    EXPECT_EQ(approval_list({Hive::Machine, L"",
                             LR"(software\microsoft\windows\currentversion\explorer\)"
                             LR"(startupapproved\startupfolder)"}),
              ApprovalList::StartupFolder);
    // Anything else isn't one: the Run keys themselves, other lists, deeper or shallower keys, the
    // WOW6432Node view, a user's classes, a machine key with a SID.
    for (const auto& key : {
             RegistryAddress{Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)"},
             RegistryAddress{Hive::Machine, L"",
                             LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved)"},
             RegistryAddress{
                 Hive::Machine, L"",
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\RunOnce)"},
             RegistryAddress{Hive::Machine, L"",
                             LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run\x)"},
             RegistryAddress{
                 Hive::Machine, L"",
                 LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
             RegistryAddress{Hive::User, std::wstring(kSid) + L"_Classes",
                             LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
             RegistryAddress{Hive::User, L"",
                             LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
             RegistryAddress{Hive::Machine, kSid,
                             LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
         }) {
        EXPECT_FALSE(approval_list(key)) << platform::to_utf8(platform::display_path(key));
    }
}

}  // namespace
}  // namespace avel::leftovers
