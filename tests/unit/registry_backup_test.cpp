// Registry backup round trips in the volatile scratch key: dump a tree with an independent dumper,
// capture it, pass the snapshot through its JSON text, delete the tree, restore it and dump it
// again. The dumps must be identical.
#include <gtest/gtest.h>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <wil/resource.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "core/changes/registry_backup.h"
#include "core/changes/serialization.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"
#include "registry_dump.h"
#include "scratch_registry.h"

namespace avel::changes {
namespace {

std::string dump(HKEY parent, const std::wstring& name) {
    return testing::dump_key(parent, name);
}

void set_raw(HKEY key, const std::wstring& name, DWORD type, const std::vector<std::byte>& data) {
    ASSERT_EQ(::RegSetValueExW(key, name.c_str(), 0, type,
                               data.empty() ? nullptr : reinterpret_cast<const BYTE*>(data.data()),
                               static_cast<DWORD>(data.size())),
              ERROR_SUCCESS);
}

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> data;
    for (const int value : values) {
        data.push_back(static_cast<std::byte>(value));
    }
    return data;
}

std::vector<std::byte> wide(std::wstring_view text, bool terminator) {
    std::vector<std::byte> data((text.size() + (terminator ? 1 : 0)) * sizeof(wchar_t));
    std::memcpy(data.data(), text.data(), text.size() * sizeof(wchar_t));
    return data;
}

std::wstring current_user_sid() {
    wil::unique_handle token;
    ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put());
    std::vector<std::byte> buffer(256);
    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &size);
    wil::unique_hlocal_string text;
    ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text);
    return text.get();
}

void set_dacl(HKEY key, const std::wstring& sddl) {
    wil::unique_hlocal_security_descriptor descriptor;
    ASSERT_TRUE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                       wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(descriptor), nullptr));
    ASSERT_EQ(::RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor.get()), ERROR_SUCCESS);
}

class RegistryBackupTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tree = scratch_.create(L"Tree");
        // Every value type, at awkward lengths.
        set_raw(tree.get(), L"none-empty", REG_NONE, {});
        set_raw(tree.get(), L"none-odd", REG_NONE, bytes({1, 2, 3}));
        set_raw(tree.get(), L"sz", REG_SZ, wide(L"Contoso Fixture", true));
        set_raw(tree.get(), L"sz-unterminated", REG_SZ, wide(L"no terminator", false));
        set_raw(tree.get(), L"sz-odd", REG_SZ, bytes({'A', 0, 'B', 0, 'C'}));
        set_raw(tree.get(), L"expand", REG_EXPAND_SZ, wide(L"%ProgramFiles%\\Contoso", true));
        set_raw(tree.get(), L"multi", REG_MULTI_SZ, wide(std::wstring(L"one\0two\0\0", 9), false));
        set_raw(tree.get(), L"binary-empty", REG_BINARY, {});
        std::vector<std::byte> big(5000);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::byte>(i * 7 + 3);
        }
        set_raw(tree.get(), L"binary-big", REG_BINARY, big);
        set_raw(tree.get(), L"dword", REG_DWORD, bytes({0x78, 0x56, 0x34, 0x12}));
        set_raw(tree.get(), L"dword-big-endian", REG_DWORD_BIG_ENDIAN, bytes({0x12, 0x34, 0x56, 0x78}));
        set_raw(tree.get(), L"qword", REG_QWORD, bytes({1, 2, 3, 4, 5, 6, 7, 8}));
        set_raw(tree.get(), L"resource-list", REG_RESOURCE_LIST, bytes({9, 9}));
        set_raw(tree.get(), L"custom-type", 0x12345, bytes({0xff}));
        set_raw(tree.get(), std::wstring(L"surrogate-\xD800-name"), REG_SZ, wide(L"x", true));
        set_raw(tree.get(), L"", REG_SZ, wide(L"", true));  // An empty default value
        set_raw(tree.get(), L"Name with \"quotes\" and \\", REG_SZ, wide(L"a\"b\\c", true));

        // A key without a default value, a key with a class, and nesting.
        scratch_.create(LR"(Tree\NoDefault\Deeper\Deepest)");
        HKEY classed = nullptr;
        wchar_t class_name[] = L"ContosoClass";
        ASSERT_EQ(::RegCreateKeyExW(tree.get(), L"WithClass", 0, class_name, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                                    nullptr, &classed, nullptr),
                  ERROR_SUCCESS);
        set_raw(classed, L"inside", REG_DWORD, bytes({1, 0, 0, 0}));
        ::RegCloseKey(classed);
        const auto surrogate = scratch_.create(std::wstring(L"Tree\\Key \xDC00 surrogate"));
        set_raw(surrogate.get(), L"v", REG_DWORD, bytes({2, 0, 0, 0}));

        // A key with its own protected DACL.
        const auto secured = scratch_.create(LR"(Tree\Secured)");
        set_raw(secured.get(), L"secret", REG_SZ, wide(L"s", true));
        set_dacl(secured.get(), L"D:P(A;;KA;;;" + current_user_sid() + L")(A;;KA;;;SY)(A;;KR;;;BA)");

        // A key whose inherited DACL is marked auto-inherited ("D:AI(...;ID;...)"), as SetSecurityInfo
        // leaves it; Windows Installer's keys look like this.
        const auto inherited = scratch_.create(LR"(Tree\AutoInherited)");
        PACL dacl = nullptr;
        wil::unique_hlocal_security_descriptor current;
        ASSERT_EQ(::GetSecurityInfo(inherited.get(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                    &dacl, nullptr, wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(current)),
                  ERROR_SUCCESS);
        ASSERT_EQ(::SetSecurityInfo(inherited.get(), SE_REGISTRY_KEY,
                                    DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                    nullptr, dacl, nullptr),
                  ERROR_SUCCESS);
        ASSERT_NE(dump(root(), LR"(Tree\AutoInherited)").find("D:AI("), std::string::npos);
    }

    HKEY root() const { return scratch_.get(); }

    testing::ScratchKey scratch_;
};

TEST_F(RegistryBackupTest, RoundTripRestoresTheTreeExactly) {
    const std::string before = dump(root(), L"Tree");
    ASSERT_NE(before, "<missing>");

    KeySnapshot snapshot;
    ASSERT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_SUCCESS);
    // Through JSON text, as the ChangeSet stores it.
    const std::string text = to_json(snapshot).dump();
    KeySnapshot loaded;
    ASSERT_TRUE(from_json(Json::parse(text), loaded));

    ASSERT_EQ(delete_key(root(), L"Tree"), ERROR_SUCCESS);
    EXPECT_EQ(dump(root(), L"Tree"), "<missing>");

    ASSERT_EQ(restore_key(root(), loaded), ERROR_SUCCESS);
    EXPECT_EQ(dump(root(), L"Tree"), before);
}

TEST_F(RegistryBackupTest, NeverRestoresOverAnExistingKey) {
    KeySnapshot snapshot;
    ASSERT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_SUCCESS);
    scratch_.set_string(L"Tree", L"added-after", L"reinstalled");
    const std::string before = dump(root(), L"Tree");
    EXPECT_EQ(restore_key(root(), snapshot), ERROR_ALREADY_EXISTS);
    EXPECT_EQ(dump(root(), L"Tree"), before);
}

TEST_F(RegistryBackupTest, ATreeThatCantBeDeletedFailsCaptureAndStaysIntact) {
    const auto locked = scratch_.create(LR"(Tree\NoDefault\Locked)");
    // Readable but not deletable: no DELETE right for anyone.
    set_dacl(locked.get(), L"D:P(A;;KR;;;" + current_user_sid() + L")(A;;KR;;;SY)");
    const std::string before = dump(root(), L"Tree");

    KeySnapshot snapshot;
    EXPECT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_ACCESS_DENIED);
    EXPECT_EQ(dump(root(), L"Tree"), before);

    // Unlock it (as its owner) so the scratch key can be cleaned up.
    set_dacl(locked.get(), L"D:(A;;KA;;;" + current_user_sid() + L")");
}

TEST_F(RegistryBackupTest, MergeFillsInWhatAPartialDeletionRemoved) {
    const std::string before = dump(root(), L"Tree");
    KeySnapshot snapshot;
    ASSERT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_SUCCESS);
    // A deletion that stopped part way: one subtree and some values gone.
    ASSERT_EQ(delete_key(scratch_.create(L"Tree").get(), L"NoDefault"), ERROR_SUCCESS);
    const auto tree = scratch_.create(L"Tree");
    ::RegDeleteValueW(tree.get(), L"sz");
    ::RegDeleteValueW(tree.get(), L"");
    ASSERT_NE(dump(root(), L"Tree"), before);
    ASSERT_EQ(merge_key(root(), snapshot), ERROR_SUCCESS);
    EXPECT_EQ(dump(root(), L"Tree"), before);
    // Merging never overwrites.
    scratch_.set_string(L"Tree", L"sz", L"changed since");
    ASSERT_EQ(merge_key(root(), snapshot), ERROR_SUCCESS);
    EXPECT_NE(dump(root(), L"Tree"), before);
}

TEST_F(RegistryBackupTest, ReadOnlyCaptureSkipsNamedSubkeys) {
    const auto locked = scratch_.create(LR"(Tree\Security)");
    set_dacl(locked.get(), L"D:P(A;;KR;;;" + current_user_sid() + L")");  // Readable, not deletable
    KeySnapshot snapshot;
    EXPECT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_ACCESS_DENIED);
    ASSERT_EQ(capture_key_readonly(root(), L"Tree", {L"security"}, snapshot), ERROR_SUCCESS);
    EXPECT_FALSE(std::any_of(snapshot.subkeys.begin(), snapshot.subkeys.end(),
                             [](const KeySnapshot& key) { return key.name == L"Security"; }));
    EXPECT_TRUE(std::any_of(snapshot.subkeys.begin(), snapshot.subkeys.end(),
                            [](const KeySnapshot& key) { return key.name == L"Secured"; }));
    set_dacl(locked.get(), L"D:(A;;KA;;;" + current_user_sid() + L")");
}

TEST_F(RegistryBackupTest, MissingKeyIsReportedAsNotFound) {
    KeySnapshot snapshot;
    EXPECT_EQ(capture_key(root(), L"Nope", snapshot), ERROR_FILE_NOT_FOUND);
}

TEST_F(RegistryBackupTest, SingleValuesRoundTrip) {
    const auto tree = scratch_.create(L"Tree");
    for (const std::wstring name : {std::wstring(L"binary-big"), std::wstring(L"surrogate-\xD800-name"),
                                    std::wstring(L""), std::wstring(L"none-empty")}) {
        const std::string before = dump(root(), L"Tree");
        ValueSnapshot value;
        ASSERT_EQ(capture_value(tree.get(), name, value), ERROR_SUCCESS);
        ValueSnapshot loaded;
        ASSERT_TRUE(from_json(Json::parse(to_json(value).dump()), loaded));
        ASSERT_EQ(::RegDeleteValueW(tree.get(), name.c_str()), ERROR_SUCCESS);
        EXPECT_NE(dump(root(), L"Tree"), before);
        EXPECT_EQ(restore_value(tree.get(), loaded), ERROR_SUCCESS);
        EXPECT_EQ(dump(root(), L"Tree"), before);
        EXPECT_EQ(restore_value(tree.get(), loaded), ERROR_ALREADY_EXISTS);
    }
    ValueSnapshot missing;
    EXPECT_EQ(capture_value(tree.get(), L"no-such-value", missing), ERROR_FILE_NOT_FOUND);
}

TEST_F(RegistryBackupTest, RegistryLinksAreRefused) {
    HKEY link = nullptr;
    const auto parent = scratch_.create(LR"(Tree\Links)");
    if (::RegCreateKeyExW(parent.get(), L"Link", 0, nullptr, REG_OPTION_VOLATILE | REG_OPTION_CREATE_LINK,
                          KEY_ALL_ACCESS, nullptr, &link, nullptr) != ERROR_SUCCESS) {
        GTEST_SKIP() << "this account can't create registry links";
    }
    const std::wstring target = L"\\Registry\\User\\" + current_user_sid() + L"\\" + scratch_.path() + L"\\Tree";
    ::RegSetValueExW(link, L"SymbolicLinkValue", 0, REG_LINK, reinterpret_cast<const BYTE*>(target.c_str()),
                     static_cast<DWORD>(target.size() * sizeof(wchar_t)));
    ::RegCloseKey(link);
    KeySnapshot snapshot;
    EXPECT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_CANT_ACCESS_FILE);
    // Clean up the link itself, not its target.
    HKEY opened = nullptr;
    if (::RegOpenKeyExW(parent.get(), L"Link", REG_OPTION_OPEN_LINK, DELETE, &opened) == ERROR_SUCCESS) {
        using NtDeleteKeyFunction = LONG(NTAPI*)(HANDLE);
        const auto nt_delete_key = reinterpret_cast<NtDeleteKeyFunction>(
            ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtDeleteKey"));
        nt_delete_key(opened);
        ::RegCloseKey(opened);
    }
}

TEST_F(RegistryBackupTest, RegTextIsReadable) {
    KeySnapshot snapshot;
    ASSERT_EQ(capture_key(root(), L"Tree", snapshot), ERROR_SUCCESS);
    const std::wstring text = to_reg_text(L"HKEY_CURRENT_USER\\Software\\Tree", snapshot);
    EXPECT_EQ(text.rfind(L"\xFEFFWindows Registry Editor Version 5.00\r\n", 0), 0u);
    EXPECT_NE(text.find(L"[HKEY_CURRENT_USER\\Software\\Tree]\r\n"), std::wstring::npos);
    EXPECT_NE(text.find(L"\"sz\"=\"Contoso Fixture\"\r\n"), std::wstring::npos);
    EXPECT_NE(text.find(L"\"dword\"=dword:12345678\r\n"), std::wstring::npos);
    EXPECT_NE(text.find(L"\"qword\"=hex(b):01,02,03,04,05,06,07,08\r\n"), std::wstring::npos);
    EXPECT_NE(text.find(L"@=\"\"\r\n"), std::wstring::npos);
    EXPECT_NE(text.find(L"\"Name with \\\"quotes\\\" and \\\\\"=\"a\\\"b\\\\c\"\r\n"), std::wstring::npos);
    EXPECT_NE(text.find(L"[HKEY_CURRENT_USER\\Software\\Tree\\NoDefault\\Deeper]\r\n"), std::wstring::npos);
}

// Read-only: App Paths is one key that both registry views share, so its WOW6432Node path names
// the same key, while Run is a separate key per view.
TEST(RegistryKeyTest, KernelNameSeesThroughSharedKeys) {
    const auto name_of = [](const wchar_t* path) {
        platform::RegistryKey key;
        EXPECT_EQ(platform::RegistryKey::open(HKEY_LOCAL_MACHINE, path, KEY_READ | KEY_WOW64_64KEY, key), ERROR_SUCCESS)
            << platform::to_utf8(path);
        return platform::to_lower(key.kernel_name());
    };
    const auto app_paths = name_of(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths)");
    EXPECT_EQ(app_paths, L"\\registry\\machine\\software\\microsoft\\windows\\currentversion\\app paths");
    EXPECT_EQ(name_of(LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths)"), app_paths);
    EXPECT_NE(name_of(LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run)"),
              name_of(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)"));
}

}  // namespace
}  // namespace avel::changes
