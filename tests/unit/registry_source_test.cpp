// Reads a real registry key: a fixture created under HKCU\Software\Avel.Tests\<random>.
// Every key is volatile, so even if a test crashes before cleaning up, Windows discards the
// fixture at sign-out. Nothing outside that key is touched.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/inventory/registry_source.h"
#include "core/platform/registry.h"

namespace avel::inventory {
namespace {

class RegistryFixture : public ::testing::Test {
protected:
    void SetUp() override {
        GUID guid{};
        ASSERT_EQ(::CoCreateGuid(&guid), S_OK);
        wchar_t text[40] = {};
        ::StringFromGUID2(guid, text, 40);
        root_path_ = std::wstring(LR"(Software\Avel.Tests\)") + text;
        uninstall_ = create(HKEY_CURRENT_USER, root_path_);
    }

    void TearDown() override {
        uninstall_.reset();
        ::RegDeleteTreeW(HKEY_CURRENT_USER, root_path_.c_str());
        ::RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\Avel.Tests)");  // Only succeeds once it's empty
    }

    static wil::unique_hkey create(HKEY parent, const std::wstring& path) {
        HKEY key = nullptr;
        EXPECT_EQ(::RegCreateKeyExW(parent, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                                    nullptr, &key, nullptr),
                  ERROR_SUCCESS);
        return wil::unique_hkey(key);
    }

    void add_entry(const std::wstring& name,
                   std::initializer_list<std::pair<std::wstring, std::wstring>> strings,
                   std::initializer_list<std::pair<std::wstring, DWORD>> dwords = {}) {
        const auto entry = create(uninstall_.get(), name);
        for (const auto& [value_name, text] : strings) {
            ::RegSetValueExW(entry.get(), value_name.c_str(), 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(text.c_str()),
                             static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        }
        for (const auto& [value_name, number] : dwords) {
            ::RegSetValueExW(entry.get(), value_name.c_str(), 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&number), sizeof(number));
        }
    }

    RegistryScanResult scan() {
        UninstallKeyContext context;
        context.location.view = RegistryView::User;
        context.location.path = L"HKCU\\" + root_path_;
        context.id_prefix = L"reg:test:";
        context.scope = Scope::User;
        RegistryScanResult result;
        scan_uninstall_key(uninstall_.get(), context, result);
        return result;
    }

    std::wstring root_path_;
    wil::unique_hkey uninstall_;
};

TEST_F(RegistryFixture, ReadsEntriesFromARealKey) {
    add_entry(L"Contoso",
              {{L"DisplayName", L"Contoso App"}, {L"UninstallString", LR"("C:\Contoso\uninst.exe")"}},
              {{L"EstimatedSize", 1234}});
    add_entry(L"{11111111-2222-3333-4444-555555555555}", {{L"DisplayName", L"Runtime"}},
              {{L"SystemComponent", 1}, {L"WindowsInstaller", 1}});
    add_entry(L"Empty", {});
    add_entry(L"BadDate", {{L"DisplayName", L"Bad date"}, {L"InstallDate", L"20245511"}});

    const auto result = scan();
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.programs.size(), 3u);  // "Empty" carries no information

    // An InstallDate that doesn't parse means no date, as in Settings (no last-write fallback).
    const auto bad_date = std::find_if(result.programs.begin(), result.programs.end(),
                                       [](const Program& p) { return p.display_name == L"Bad date"; });
    ASSERT_NE(bad_date, result.programs.end());
    EXPECT_FALSE(bad_date->install_date.has_value());

    const auto contoso = std::find_if(result.programs.begin(), result.programs.end(),
                                      [](const Program& p) { return p.display_name == L"Contoso App"; });
    ASSERT_NE(contoso, result.programs.end());
    EXPECT_EQ(contoso->id, L"reg:test:Contoso");
    EXPECT_EQ(contoso->estimated_size_bytes, 1234u * 1024u);
    EXPECT_EQ(contoso->scope, Scope::User);
    // No InstallDate value: the key's last-write date (today) stands in, as in Settings.
    EXPECT_TRUE(contoso->install_date.has_value());
    ASSERT_TRUE(contoso->registry.has_value());
    EXPECT_EQ(contoso->registry->path, L"HKCU\\" + root_path_ + L"\\Contoso");

    const auto runtime = std::find_if(result.programs.begin(), result.programs.end(),
                                      [](const Program& p) { return p.display_name == L"Runtime"; });
    ASSERT_NE(runtime, result.programs.end());
    EXPECT_TRUE(runtime->system_component);
    EXPECT_EQ(runtime->msi_product_code, L"{11111111-2222-3333-4444-555555555555}");
}

TEST_F(RegistryFixture, ReadsUnicodeNames) {
    add_entry(L"Avel", {{L"DisplayName", L"Ավել — Веник"}, {L"Publisher", L"Հեղինակ"}});
    const auto result = scan();
    ASSERT_EQ(result.programs.size(), 1u);
    EXPECT_EQ(result.programs[0].display_name, L"Ավել — Веник");
    EXPECT_EQ(result.programs[0].publisher, L"Հեղինակ");
}

TEST(RegistryValueTest, StringsWithoutTerminatorAndOddSizes) {
    platform::RegistryValue value;
    value.type = REG_SZ;
    const wchar_t text[] = {L'a', L'b', L'c'};  // no terminator
    value.data.resize(sizeof(text) + 1);        // and an odd trailing byte
    std::memcpy(value.data.data(), text, sizeof(text));
    EXPECT_EQ(value.as_string(), L"abc");

    value.type = REG_DWORD;
    EXPECT_FALSE(value.as_string().has_value());
}

TEST(RegistryValueTest, IntegersFromDwordQwordAndText) {
    platform::RegistryValue value;
    value.type = REG_QWORD;
    const std::uint64_t big = 1ull << 40;
    value.data.resize(sizeof(big));
    std::memcpy(value.data.data(), &big, sizeof(big));
    EXPECT_EQ(value.as_integer(), big);

    value.type = REG_SZ;
    const std::wstring digits = L"42";
    value.data.resize((digits.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), digits.c_str(), value.data.size());
    EXPECT_EQ(value.as_integer(), 42u);

    const std::wstring junk = L"4x2";
    value.data.resize((junk.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), junk.c_str(), value.data.size());
    EXPECT_FALSE(value.as_integer().has_value());
}

}  // namespace
}  // namespace avel::inventory
