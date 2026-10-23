// FNV-1a against its published test vectors, and how registry values are told apart.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstddef>
#include <string_view>
#include <vector>

#include "core/platform/hash.h"

namespace avel::platform {
namespace {

std::uint64_t hash_text(std::string_view text) { return fnv1a(text.data(), text.size()); }

TEST(HashTest, MatchesTheFnv1aTestVectors) {
    EXPECT_EQ(hash_text(""), 0xcbf29ce484222325ull);
    EXPECT_EQ(hash_text("a"), 0xaf63dc4c8601ec8cull);
    EXPECT_EQ(hash_text("foobar"), 0x85944171f73967e8ull);
}

TEST(HashTest, ASeedContinuesAHash) { EXPECT_EQ(fnv1a("bar", 3, fnv1a("foo", 3)), hash_text("foobar")); }

TEST(HashTest, RawValuesDifferByTypeAndLength) {
    const std::vector<std::byte> a = {std::byte{'a'}, std::byte{0}, std::byte{0}, std::byte{0}};  // L"a"
    const std::vector<std::byte> empty_string(2);                                                 // L""
    EXPECT_EQ(hash_raw_value(REG_SZ, a), hash_raw_value(REG_SZ, std::vector<std::byte>(a)));
    EXPECT_NE(hash_raw_value(REG_SZ, a), hash_raw_value(REG_EXPAND_SZ, a));
    EXPECT_NE(hash_raw_value(REG_SZ, empty_string), hash_raw_value(REG_SZ, {}));
    EXPECT_NE(hash_raw_value(REG_NONE, {}), hash_raw_value(REG_BINARY, {}));
    EXPECT_NE(hash_raw_value(REG_SZ, {}), kFnvOffset);
}

}  // namespace
}  // namespace avel::platform
