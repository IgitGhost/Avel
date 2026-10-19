// A scratch registry key for tests: HKCU\Software\Avel.Tests\{random GUID}. Every key under it
// is volatile, so even if a test crashes before cleaning up, Windows discards it at sign-out.
// Tests use it as a stand-in for HKLM or a user's hive; nothing outside it is touched.
#pragma once

#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <wil/resource.h>

#include <string>
#include <string_view>

namespace avel::testing {

class ScratchKey {
public:
    ScratchKey() {
        GUID guid{};
        EXPECT_EQ(::CoCreateGuid(&guid), S_OK);
        wchar_t text[40] = {};
        ::StringFromGUID2(guid, text, 40);
        path_ = std::wstring(LR"(Software\Avel.Tests\)") + text;
        root_ = create_under(HKEY_CURRENT_USER, path_);
    }

    ~ScratchKey() {
        root_.reset();
        ::RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        ::RegDeleteKeyW(HKEY_CURRENT_USER, LR"(Software\Avel.Tests)");  // Only succeeds once it's empty
    }

    ScratchKey(const ScratchKey&) = delete;
    ScratchKey& operator=(const ScratchKey&) = delete;

    HKEY get() const { return root_.get(); }
    const std::wstring& path() const { return path_; }  // Relative to HKEY_CURRENT_USER

    // Creates `subpath` (and any missing parents) below the scratch key.
    wil::unique_hkey create(std::wstring_view subpath) const { return create_under(root_.get(), std::wstring(subpath)); }

    void set_string(std::wstring_view subpath, std::wstring_view name, std::wstring_view value,
                    DWORD type = REG_SZ) const {
        const auto key = create(subpath);
        const std::wstring text(value);
        EXPECT_EQ(::RegSetValueExW(key.get(), std::wstring(name).c_str(), 0, type,
                                   reinterpret_cast<const BYTE*>(text.c_str()),
                                   static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))),
                  ERROR_SUCCESS);
    }

    void set_dword(std::wstring_view subpath, std::wstring_view name, DWORD value) const {
        const auto key = create(subpath);
        EXPECT_EQ(::RegSetValueExW(key.get(), std::wstring(name).c_str(), 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value)),
                  ERROR_SUCCESS);
    }

private:
    static wil::unique_hkey create_under(HKEY parent, const std::wstring& path) {
        HKEY key = nullptr;
        EXPECT_EQ(::RegCreateKeyExW(parent, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                                    nullptr, &key, nullptr),
                  ERROR_SUCCESS);
        return wil::unique_hkey(key);
    }

    std::wstring path_;
    wil::unique_hkey root_;
};

}  // namespace avel::testing
