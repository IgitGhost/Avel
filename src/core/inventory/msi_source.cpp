#include "core/inventory/msi_source.h"

#include <windows.h>
#include <msi.h>

#include <array>
#include <string>

#include "core/inventory/uninstall_entry.h"
#include "core/platform/text.h"

namespace avel::inventory {

namespace {

constexpr wchar_t kAllUsers[] = L"s-1-1-0";  // "Everyone": enumerate every user's products

struct ProductKey {
    std::wstring code;
    MSIINSTALLCONTEXT context = MSIINSTALLCONTEXT_NONE;
    std::wstring sid;  // empty for per-machine products
};

std::wstring product_property(const ProductKey& product, const wchar_t* property) {
    const wchar_t* sid = product.context == MSIINSTALLCONTEXT_MACHINE ? nullptr : product.sid.c_str();
    std::wstring value(128, L'\0');
    DWORD length = static_cast<DWORD>(value.size());
    UINT status =
        ::MsiGetProductInfoExW(product.code.c_str(), sid, product.context, property, value.data(), &length);
    if (status == ERROR_MORE_DATA) {
        value.resize(++length);
        status = ::MsiGetProductInfoExW(product.code.c_str(), sid, product.context, property, value.data(),
                                        &length);
    }
    if (status != ERROR_SUCCESS) {
        return {};
    }
    value.resize(length);
    return std::wstring(platform::trim(value));
}

// Returns ERROR_SUCCESS or the first enumeration error.
UINT enumerate(const wchar_t* user_sid, std::vector<ProductKey>& products) {
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 39> code{};
        std::array<wchar_t, 256> sid{};
        DWORD sid_length = static_cast<DWORD>(sid.size());
        MSIINSTALLCONTEXT context = MSIINSTALLCONTEXT_NONE;
        const UINT status = ::MsiEnumProductsExW(nullptr, user_sid, MSIINSTALLCONTEXT_ALL, index, code.data(),
                                                 &context, sid.data(), &sid_length);
        if (status == ERROR_NO_MORE_ITEMS) {
            return ERROR_SUCCESS;
        }
        if (status != ERROR_SUCCESS) {
            return status;
        }
        products.push_back({code.data(), context, std::wstring(sid.data(), sid_length)});
    }
}

Program to_program(const ProductKey& product) {
    Program program;
    program.source = Source::Msi;
    program.installer_type = InstallerType::Msi;
    program.msi_product_code = extract_guid(product.code).value_or(product.code);
    program.scope = product.context == MSIINSTALLCONTEXT_MACHINE ? Scope::Machine : Scope::User;
    program.user_sid = product.context == MSIINSTALLCONTEXT_MACHINE ? L"" : product.sid;
    program.id =
        L"msi:" + program.msi_product_code + (program.user_sid.empty() ? L"" : L":" + program.user_sid);

    program.display_name = product_property(product, INSTALLPROPERTY_INSTALLEDPRODUCTNAME);
    if (program.display_name.empty()) {
        program.display_name = product_property(product, INSTALLPROPERTY_PRODUCTNAME);
    }
    program.display_version = product_property(product, INSTALLPROPERTY_VERSIONSTRING);
    program.publisher = product_property(product, INSTALLPROPERTY_PUBLISHER);
    program.install_location = product_property(product, INSTALLPROPERTY_INSTALLLOCATION);
    while (program.install_location.size() > 3 && program.install_location.back() == L'\\') {
        program.install_location.pop_back();
    }
    program.install_source = product_property(product, INSTALLPROPERTY_INSTALLSOURCE);
    program.install_date = parse_install_date(product_property(product, INSTALLPROPERTY_INSTALLDATE));
    program.display_icon = product_property(product, INSTALLPROPERTY_PRODUCTICON);
    program.about_url = product_property(product, INSTALLPROPERTY_URLINFOABOUT);
    program.uninstall_string = L"MsiExec.exe /X" + program.msi_product_code;
    return program;
}

}  // namespace

MsiScanResult scan_msi_products() {
    MsiScanResult result;
    std::vector<ProductKey> products;

    UINT status = enumerate(kAllUsers, products);
    if (status == ERROR_ACCESS_DENIED || status == ERROR_BAD_CONFIGURATION) {
        // Other users' products need elevation; fall back to this user and per-machine products.
        result.diagnostics.push_back(
            {DiagnosticCode::MsiAllUsersDenied, L"", static_cast<std::int32_t>(status)});
        products.clear();
        status = enumerate(nullptr, products);
    }
    if (status != ERROR_SUCCESS) {
        result.diagnostics.push_back(
            {DiagnosticCode::MsiEnumerationFailed, L"", static_cast<std::int32_t>(status)});
    }

    result.programs.reserve(products.size());
    for (const auto& product : products) {
        result.programs.push_back(to_program(product));
    }
    return result;
}

}  // namespace avel::inventory
