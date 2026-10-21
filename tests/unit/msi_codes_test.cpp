#include <gtest/gtest.h>

#include "core/inventory/msi_codes.h"

namespace avel::inventory {
namespace {

TEST(MsiCodesTest, PacksAndUnpacksLikeWindowsInstaller) {
    // 7-Zip 26.00 (x64), as HKLM\SOFTWARE\Classes\Installer\Products spells it.
    EXPECT_EQ(pack_guid(L"{23170F69-40C1-2702-2600-000001000000}"), L"96F071321C0420726200000010000000");
    EXPECT_EQ(unpack_guid(L"96F071321C0420726200000010000000"), L"{23170F69-40C1-2702-2600-000001000000}");
    EXPECT_EQ(pack_guid(L"{605108c1-153e-43d8-8a67-7ce326b00eca}"), L"1C801506E3518D34A876C73E620BE0AC");
    EXPECT_EQ(unpack_guid(L"1c801506e3518d34a876c73e620be0ac"), L"{605108C1-153E-43D8-8A67-7CE326B00ECA}");
}

TEST(MsiCodesTest, RejectsAnythingElse) {
    EXPECT_FALSE(pack_guid(L""));
    EXPECT_FALSE(pack_guid(L"23170F69-40C1-2702-2600-000001000000"));
    EXPECT_FALSE(pack_guid(L"{23170F69-40C1-2702-2600-00000100000G}"));
    EXPECT_FALSE(pack_guid(L"{23170F6940C1-2702-2600-0000010000000}"));
    EXPECT_FALSE(unpack_guid(L"96F071321C042072620000001000000"));
    EXPECT_FALSE(unpack_guid(L"96F071321C04207262000000100000ZZ"));
}

}  // namespace
}  // namespace avel::inventory
