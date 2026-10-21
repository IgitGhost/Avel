#include "core/inventory/msi_codes.h"

#include <array>
#include <cwctype>

namespace avel::inventory {

namespace {

// Where each packed digit comes from among the GUID's 32 digits (in order, dashes left out).
// The mapping swaps pairs of positions, so it is its own inverse.
constexpr std::array<int, 32> kOrder = {7,  6,  5,  4,  3,  2,  1,  0,  11, 10, 9,  8,  15, 14, 13, 12,
                                        17, 16, 19, 18, 21, 20, 23, 22, 25, 24, 27, 26, 29, 28, 31, 30};

std::optional<std::wstring> reorder(std::wstring_view digits) {
    if (digits.size() != 32) {
        return std::nullopt;
    }
    std::wstring result(32, L'0');
    for (std::size_t i = 0; i < 32; ++i) {
        const wchar_t c = digits[static_cast<std::size_t>(kOrder[i])];
        if (!std::iswxdigit(c)) {
            return std::nullopt;
        }
        result[i] = static_cast<wchar_t>(std::towupper(c));
    }
    return result;
}

}  // namespace

std::optional<std::wstring> pack_guid(std::wstring_view guid) {
    if (guid.size() != 38 || guid.front() != L'{' || guid.back() != L'}' || guid[9] != L'-' ||
        guid[14] != L'-' || guid[19] != L'-' || guid[24] != L'-') {
        return std::nullopt;
    }
    std::wstring digits;
    for (const wchar_t c : guid.substr(1, 36)) {
        if (c != L'-') {
            digits += c;
        }
    }
    return reorder(digits);
}

std::optional<std::wstring> unpack_guid(std::wstring_view packed) {
    const auto digits = reorder(packed);
    if (!digits) {
        return std::nullopt;
    }
    return L"{" + digits->substr(0, 8) + L"-" + digits->substr(8, 4) + L"-" + digits->substr(12, 4) + L"-" +
           digits->substr(16, 4) + L"-" + digits->substr(20) + L"}";
}

}  // namespace avel::inventory
