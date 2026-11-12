#include <gtest/gtest.h>

#include <windows.h>

#include <cwctype>
#include <string>

#include "core/platform/version_info.h"

namespace avel::platform {
namespace {

std::wstring system_file(const wchar_t* name) {
    wchar_t folder[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(folder, MAX_PATH);
    return std::wstring(folder, length) + L"\\" + name;
}

TEST(VersionInfoTest, ReadsProductCompanyAndVersion) {
    const FileProduct product = file_product(system_file(L"kernel32.dll"));
    EXPECT_FALSE(product.product_name.empty());
    EXPECT_NE(product.company_name.find(L"Microsoft"), std::wstring::npos);
    ASSERT_FALSE(product.version.empty());
    EXPECT_TRUE(std::iswdigit(product.version.front()));
}

TEST(VersionInfoTest, MissingFileGivesNothing) {
    const FileProduct product = file_product(system_file(L"avel-no-such-file.dll"));
    EXPECT_TRUE(product.product_name.empty());
    EXPECT_TRUE(product.company_name.empty());
    EXPECT_TRUE(product.version.empty());
}

}  // namespace
}  // namespace avel::platform
