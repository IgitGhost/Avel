#include "core/platform/version_info.h"

#include <windows.h>

#include <cwchar>
#include <format>
#include <vector>

#include "core/platform/text.h"

namespace avel::platform {

FileProduct file_product(const std::wstring& path) {
    FileProduct product;
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &ignored);
    if (size == 0) {
        return product;
    }
    std::vector<std::byte> data(size);
    if (!::GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, data.data())) {
        return product;
    }
    struct Translation {
        WORD language;
        WORD code_page;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(data.data(), LR"(\VarFileInfo\Translation)",
                          reinterpret_cast<void**>(&translations), &length) ||
        length < sizeof(Translation)) {
        return product;
    }
    const auto read = [&](const wchar_t* name) -> std::wstring {
        const std::wstring query = std::format(LR"(\StringFileInfo\{:04x}{:04x}\{})", translations->language,
                                               translations->code_page, name);
        wchar_t* text = nullptr;
        UINT characters = 0;
        if (!::VerQueryValueW(data.data(), query.c_str(), reinterpret_cast<void**>(&text), &characters) ||
            !text) {
            return {};
        }
        return std::wstring(trim(std::wstring_view(text, ::wcsnlen(text, characters))));
    };
    product.product_name = read(L"ProductName");
    if (product.product_name.empty()) {
        product.product_name = read(L"FileDescription");
    }
    product.company_name = read(L"CompanyName");
    return product;
}

}  // namespace avel::platform
