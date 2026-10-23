// FNV-1a, 64-bit: a fast, stable hash for telling whether something changed without keeping its
// bytes, such as every registry value in a before-and-after snapshot. Not for anything security
// depends on: anyone who controls the input can make two inputs collide.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace avel::platform {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// Pass a previous result as `seed` to hash several pieces as one.
inline std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t seed = kFnvOffset) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * kFnvPrime;
    }
    return hash;
}

// A registry value's type, length and bytes, so values that differ only in type (REG_SZ and
// REG_EXPAND_SZ) hash differently.
inline std::uint64_t hash_raw_value(DWORD type, std::span<const std::byte> data) {
    const std::uint64_t size = data.size();
    std::uint64_t hash = fnv1a(&type, sizeof(type));
    hash = fnv1a(&size, sizeof(size), hash);
    return fnv1a(data.data(), data.size(), hash);
}

}  // namespace avel::platform
