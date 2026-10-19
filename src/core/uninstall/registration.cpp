#include "core/uninstall/registration.h"

#include <msi.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <filesystem>
#include <system_error>
#include <thread>

namespace avel::uninstall {

namespace {

namespace wmd = winrt::Windows::Management::Deployment;

constexpr wchar_t kUninstallPath[] = LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall\)";
constexpr wchar_t kUninstallPath32[] = LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\)";

// Runs `work` on a fresh multithreaded-apartment thread, as WinRT wants; the caller's COM state
// (the GUI thread is a single-threaded apartment) is left alone.
template <typename Work>
auto run_in_mta(Work&& work) {
    decltype(work()) result{};
    std::thread worker([&] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        result = work();
        winrt::uninit_apartment();
    });
    worker.join();
    return result;
}

bool key_exists(const UninstallKeyAddress& address) {
    HKEY key = nullptr;
    const LSTATUS status =
        ::RegOpenKeyExW(address.root, address.path.c_str(), 0, KEY_READ | address.view, &key);
    if (status == ERROR_SUCCESS) {
        ::RegCloseKey(key);
        return true;
    }
    return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}

bool msi_product_installed(const std::wstring& product_code) {
    const INSTALLSTATE state = ::MsiQueryProductStateW(product_code.c_str());
    return state == INSTALLSTATE_DEFAULT || state == INSTALLSTATE_ADVERTISED;
}

}  // namespace

std::optional<UninstallKeyAddress> uninstall_key_address(const inventory::RegistryLocation& location,
                                                         std::wstring_view user_sid) {
    using inventory::RegistryView;
    UninstallKeyAddress address;
    switch (location.view) {
        case RegistryView::Machine64:
            address.root = HKEY_LOCAL_MACHINE;
            address.path = kUninstallPath + location.key_name;
            address.view = KEY_WOW64_64KEY;
            return address;
        case RegistryView::Machine32:
            address.root = HKEY_LOCAL_MACHINE;
            address.path = kUninstallPath + location.key_name;
            address.view = KEY_WOW64_32KEY;
            return address;
        case RegistryView::User:
        case RegistryView::User32:
            if (user_sid.empty()) {
                return std::nullopt;
            }
            address.root = HKEY_USERS;
            address.path = std::wstring(user_sid) + L'\\' +
                           (location.view == RegistryView::User ? kUninstallPath : kUninstallPath32) +
                           location.key_name;
            return address;
    }
    return std::nullopt;
}

bool is_still_registered(const inventory::Program& program) {
    if (program.source == inventory::Source::Appx) {
        return is_package_installed(program.package_full_name);
    }
    if (!program.msi_product_code.empty() && msi_product_installed(program.msi_product_code)) {
        return true;
    }
    if (program.registry) {
        const auto address = uninstall_key_address(*program.registry, program.user_sid);
        return !address || key_exists(*address);
    }
    return false;
}

bool install_location_exists(const inventory::Program& program) {
    std::error_code error;
    return !program.install_location.empty() &&
           std::filesystem::is_directory(program.install_location, error);
}

std::int32_t remove_package(const std::wstring& package_full_name) {
    return run_in_mta([&]() -> std::int32_t {
        try {
            wmd::PackageManager manager;
            const auto result = manager.RemovePackageAsync(package_full_name).get();
            return result.ExtendedErrorCode().value;
        } catch (const winrt::hresult_error& error) {
            return error.code().value;
        }
    });
}

bool is_package_installed(const std::wstring& package_full_name) {
    return run_in_mta([&]() -> bool {
        try {
            wmd::PackageManager manager;
            return manager.FindPackageForUser(L"", package_full_name) != nullptr;
        } catch (const winrt::hresult_error&) {
            return true;  // Unsure: assume it's still there
        }
    });
}

}  // namespace avel::uninstall
