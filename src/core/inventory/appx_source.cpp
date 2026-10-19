#include "core/inventory/appx_source.h"

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "core/platform/text.h"
#include "core/platform/time.h"

namespace avel::inventory {

namespace {

namespace wam = winrt::Windows::ApplicationModel;
namespace wmd = winrt::Windows::Management::Deployment;

Architecture to_architecture(winrt::Windows::System::ProcessorArchitecture architecture) {
    using winrt::Windows::System::ProcessorArchitecture;
    switch (architecture) {
        case ProcessorArchitecture::X86:
            return Architecture::X86;
        case ProcessorArchitecture::X64:
            return Architecture::X64;
        case ProcessorArchitecture::Arm:
            return Architecture::Arm;
        case ProcessorArchitecture::Arm64:
            return Architecture::Arm64;
        case ProcessorArchitecture::Neutral:
            return Architecture::Neutral;
        default:
            return Architecture::Unknown;
    }
}

// Package properties resolve resources and file paths and can throw for broken packages; a bad
// property must not lose the whole entry.
template <typename Getter>
auto try_get(Getter&& getter) -> decltype(getter()) {
    try {
        return getter();
    } catch (...) {
        return {};
    }
}

// "file:///C:/Program%20Files/..." -> "C:\Program Files\...".
std::wstring file_url_to_path(const winrt::hstring& url) {
    std::array<wchar_t, MAX_PATH * 2> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (FAILED(::PathCreateFromUrlW(url.c_str(), path.data(), &length, 0))) {
        return {};
    }
    return std::wstring(path.data(), length);
}

// Resolves a manifest string for an installed package. Plain text is returned as is. Resource
// references come in several forms, all seen in real manifests:
//   ms-resource://Package/Path            absolute
//   ms-resource:/Path                     relative to the package root
//   ms-resource:LensSDK/Resources/Title   usually relative to the root...
//   ms-resource:AppName/Text              ...but sometimes a key in the default "Resources" map
//   ms-resource:AppName                   shorthand for Resources/AppName
// so relative forms are tried both ways. Returns an empty string when nothing resolves.
std::wstring resolve_manifest_string(std::wstring_view text, std::wstring_view package_name,
                                     std::wstring_view package_full_name) {
    constexpr std::wstring_view kPrefix = L"ms-resource:";
    if (!platform::istarts_with(text, kPrefix)) {
        return std::wstring(text);
    }
    const std::wstring_view key = text.substr(kPrefix.size());
    const std::wstring root = L"ms-resource://" + std::wstring(package_name);
    std::vector<std::wstring> candidates;
    if (key.starts_with(L"//")) {
        candidates.push_back(std::wstring(kPrefix) + std::wstring(key));
    } else if (key.starts_with(L'/')) {
        candidates.push_back(root + std::wstring(key));
    } else if (key.find(L'/') != std::wstring_view::npos) {
        candidates.push_back(root + L"/" + std::wstring(key));
        candidates.push_back(root + L"/Resources/" + std::wstring(key));
    } else {
        candidates.push_back(root + L"/Resources/" + std::wstring(key));
    }
    for (const auto& uri : candidates) {
        const std::wstring source = L"@{" + std::wstring(package_full_name) + L"?" + uri + L"}";
        std::array<wchar_t, 512> buffer{};
        if (SUCCEEDED(::SHLoadIndirectString(source.c_str(), buffer.data(), static_cast<UINT>(buffer.size()),
                                             nullptr))) {
            return buffer.data();
        }
    }
    return {};
}

// The (unresolved) display names of every application the package declares, including ones
// hidden from the Start menu: Package/Applications/Application/VisualElements/@DisplayName.
// Uses XmlLite's streaming reader. AppxPackaging's manifest reader would be the official API, but it
// validates the whole schema: it's several times slower and rejects some current inbox apps.
std::vector<std::wstring> manifest_app_display_names(const std::wstring& install_path) {
    std::vector<std::wstring> names;
    if (install_path.empty()) {
        return names;
    }
    wil::com_ptr_nothrow<IStream> stream;
    const std::wstring manifest = install_path + L"\\AppxManifest.xml";
    if (FAILED(::SHCreateStreamOnFileEx(manifest.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                        FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.put()))) {
        return names;
    }
    wil::com_ptr_nothrow<IXmlReader> reader;
    if (FAILED(::CreateXmlReader(__uuidof(IXmlReader), reader.put_void(), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(stream.get()))) {
        return names;
    }

    // Depths: Package 0, Applications 1, Application 2, VisualElements 3 (namespace prefixes vary).
    bool in_applications = false;
    bool in_application = false;
    XmlNodeType type{};
    while (reader->Read(&type) == S_OK) {
        if (type != XmlNodeType_Element && type != XmlNodeType_EndElement) {
            continue;
        }
        const wchar_t* local_name = nullptr;
        UINT depth = 0;
        if (FAILED(reader->GetLocalName(&local_name, nullptr)) || FAILED(reader->GetDepth(&depth))) {
            break;
        }
        const std::wstring_view name(local_name);
        if (type == XmlNodeType_EndElement) {
            if (depth == 2 && name == L"Application") {
                in_application = false;
            } else if (depth == 1 && name == L"Applications") {
                break;  // Nothing of interest after the applications
            }
            continue;
        }
        const bool empty_element = reader->IsEmptyElement();
        if (depth == 1 && name == L"Applications") {
            in_applications = !empty_element;
        } else if (in_applications && depth == 2 && name == L"Application") {
            names.emplace_back();
            in_application = !empty_element;
        } else if (in_application && depth == 3 && name == L"VisualElements") {
            const wchar_t* value = nullptr;
            if (reader->MoveToAttributeByName(L"DisplayName", nullptr) == S_OK &&
                SUCCEEDED(reader->GetValue(&value, nullptr)) && value != nullptr) {
                names.back() = value;
            }
            reader->MoveToElement();
        }
    }
    return names;
}

std::optional<std::chrono::sys_days> to_date(winrt::Windows::Foundation::DateTime time) {
    if (time.time_since_epoch().count() == 0) {
        return std::nullopt;
    }
    const std::uint64_t ticks = winrt::clock::to_file_time(time).value;
    FILETIME file_time{};
    file_time.dwLowDateTime = static_cast<DWORD>(ticks);
    file_time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return platform::local_date(file_time);
}

Program to_program(const wam::Package& package) {
    const auto id = package.Id();
    const auto version = id.Version();

    Program program;
    program.source = Source::Appx;
    program.scope = Scope::User;
    program.installer_type = InstallerType::Appx;
    program.package_full_name = id.FullName();
    program.package_family_name = id.FamilyName();
    program.id = L"appx:" + program.package_full_name;
    program.architecture = to_architecture(id.Architecture());
    program.display_version = std::to_wstring(version.Major) + L'.' + std::to_wstring(version.Minor) + L'.' +
                              std::to_wstring(version.Build) + L'.' + std::to_wstring(version.Revision);

    program.install_location = try_get([&] { return std::wstring(package.InstalledPath()); });
    // Windows 10 2004 and later; on older systems the property is missing and this stays empty.
    program.external_location = try_get([&] { return std::wstring(package.EffectiveExternalPath()); });

    // Same naming rule as Settings: a package with a single application shows that application's
    // name ("Calculator" rather than the package's "Windows Calculator"), even if the application
    // is hidden from the Start menu; a package with several shows the package name ("Windows Clock").
    const auto app_names = manifest_app_display_names(program.install_location);
    if (app_names.size() == 1) {
        program.display_name =
            resolve_manifest_string(app_names.front(), id.Name(), program.package_full_name);
    }
    if (program.display_name.empty()) {
        program.display_name = try_get([&] { return std::wstring(package.DisplayName()); });
    }
    if (program.display_name.empty()) {
        program.display_name = id.Name();
    }
    program.publisher = try_get([&] { return std::wstring(package.PublisherDisplayName()); });
    program.install_date = try_get([&] { return to_date(package.InstalledDate()); });
    program.display_icon = try_get([&] {
        const auto logo = package.Logo();
        return logo ? file_url_to_path(logo.AbsoluteUri()) : std::wstring();
    });

    program.is_framework = try_get([&] { return package.IsFramework() || package.IsResourcePackage(); });
    // System-signed packages are part of Windows; removing them breaks the OS.
    const bool system_signed =
        try_get([&] { return package.SignatureKind() == wam::PackageSignatureKind::System; });
    program.system_component = system_signed;
    program.no_remove = system_signed;
    return program;
}

std::optional<Program> inspect(const wam::Package& package) {
    try {
        const auto status = package.Status();
        if (status.NotAvailable() || status.Disabled()) {
            return std::nullopt;  // Being installed, serviced or on an unplugged drive
        }
        return to_program(package);
    } catch (const winrt::hresult_error&) {
        return std::nullopt;  // Skip packages that can't be inspected
    }
}

// Runs on a thread that stays in the multithreaded apartment (MTA) until every WinRT object is
// released. That matters: when the last thread leaves the MTA, COM tears it down and may unload
// the DLLs behind live objects, so workers must never outlive the thread that keeps it alive.
void scan_in_mta(AppxScanResult& result) {
    std::vector<wam::Package> packages;
    try {
        const wmd::PackageManager manager;
        // An empty SID means the current user.
        for (const auto& package :
             manager.FindPackagesForUserWithPackageTypes(L"", wmd::PackageTypes::Main)) {
            packages.push_back(package);
        }
    } catch (const winrt::hresult_error& error) {
        result.diagnostics.push_back({DiagnosticCode::AppxEnumerationFailed, L"", error.code().value});
    }

    // Reading a package's display name, install date and logo costs 1-3 ms each (resource lookups
    // and calls into the AppX service) and the packages are independent, so spread them over a
    // few threads. Package objects are agile and may be used from any thread in the MTA.
    std::vector<std::optional<Program>> inspected(packages.size());
    const unsigned worker_count = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < worker_count; ++i) {
        workers.emplace_back([&] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            for (std::size_t index = next++; index < packages.size(); index = next++) {
                inspected[index] = inspect(packages[index]);
            }
            winrt::uninit_apartment();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    packages.clear();

    for (auto& program : inspected) {
        if (program) {
            result.programs.push_back(std::move(*program));
        }
    }
}

}  // namespace

AppxScanResult scan_appx_packages() {
    AppxScanResult result;
    // A dedicated thread gives WinRT the MTA it wants without touching the caller's COM state
    // (the Qt GUI thread is a single-threaded apartment).
    std::thread worker([&result] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        scan_in_mta(result);
        winrt::uninit_apartment();
    });
    worker.join();
    return result;
}

}  // namespace avel::inventory
