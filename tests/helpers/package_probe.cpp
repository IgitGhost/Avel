// avel_package_probe: the Phase 6b spike's probe (docs/adr/0008). It makes the exact public calls
// Avel would make to remove, register and provision packaged apps (C++/WinRT
// Windows.Management.Deployment.PackageManager) and to list, enable and disable Windows features
// and capabilities (DismAPI), so the Sandbox can tell which of the Appx and DISM cmdlets' results
// Avel's own code can reach. It shares no code with avel_core. Every command prints one JSON
// object; the exit code is 0 when the call succeeded, 1 when it failed, 2 on a usage error.
//
//   whoami                               the calling user, its SID and whether it is elevated
//   list [--user <sid> | --all-users] [--filter <text>] [--users]
//                                        packages of the current user, of <sid>, or of every user
//                                        (FindPackagesWithPackageTypes, all types); --users adds
//                                        FindUsers for each
//   find <full-name> [--user <sid>]      FindPackage, FindPackageForUser and FindUsers of one package
//   provisioned [--filter <text>]        FindProvisionedPackages
//   add <path> [--allow-unsigned] [--stage-only]
//                                        AddPackageByUriAsync / StagePackageByUriAsync
//   remove <full-name> [--all-users] [--preserve-data]
//                                        RemovePackageAsync(name, RemovalOptions) for the calling
//                                        user, or RemoveForAllUsers
//   cleanup-user <name> <sid>            CleanupPackageForUserAsync
//   register-full <full-name> [--allow-unsigned]
//                                        RegisterPackageByFullNameAsync, or with --allow-unsigned
//                                        RegisterPackagesByFullNameAsync with RegisterPackageOptions
//   register-family <family-name>        RegisterPackageByFamilyNameAsync
//   register-manifest <path> [--allow-unsigned] [--dev-mode]
//                                        RegisterPackageByUriAsync (Add-AppxPackage -Register)
//   provision <family-name>              ProvisionPackageForAllUsersAsync
//   deprovision <family-name>            DeprovisionPackageForAllUsersAsync
//   removal-pending <full-name> [--user <sid>]
//                                        IsPackageRemovalPending(ForUser)
//   dism-features [--filter <text>]      DismGetFeatures of the online image
//   dism-feature-info <name>             DismGetFeatureInfo
//   dism-enable <name> [--enable-all]    DismEnableFeature, LimitAccess = TRUE, no sources
//   dism-disable <name> [--remove-payload]
//                                        DismDisableFeature, RemovePayload = FALSE by default
//   dism-capabilities [--filter <text>]  DismGetCapabilities
//   dism-capability-info <name>          DismGetCapabilityInfo
//   dism-add-capability <name>           DismAddCapability, LimitAccess = TRUE, no sources
//   dism-remove-capability <name>        DismRemoveCapability
//   The DISM commands take --log <file> for DismInitialize's log (default: DISM's own).
//
// DismAPI's header and import library ship with the Windows ADK, not the Windows SDK, so the probe
// declares the few types and functions it uses (as documented on Microsoft Learn) and loads
// DismApi.dll from System32.

#include <windows.h>
#include <sddl.h>
#include <shlwapi.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.System.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace wam = winrt::Windows::ApplicationModel;
namespace wf = winrt::Windows::Foundation;
namespace wmd = winrt::Windows::Management::Deployment;
using json = nlohmann::ordered_json;

std::string utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                          nullptr, nullptr);
    return result;
}

std::string hex(std::int32_t code) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<std::uint32_t>(code));
    return buffer;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towlower(c); });
    return text;
}

bool contains(std::wstring_view text, const std::wstring& filter) {
    return filter.empty() || lower(std::wstring(text)).find(lower(filter)) != std::wstring::npos;
}

class Stopwatch {
public:
    long long ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                     start_)
            .count();
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

struct Arguments {
    std::vector<std::wstring> positional;
    std::map<std::wstring, std::wstring> values;
    std::set<std::wstring> flags;

    bool flag(const std::wstring& name) const { return flags.count(name) != 0; }
    std::wstring value(const std::wstring& name) const {
        const auto found = values.find(name);
        return found == values.end() ? std::wstring() : found->second;
    }
    bool has_value(const std::wstring& name) const { return values.count(name) != 0; }
};

Arguments parse(int argc, wchar_t** argv) {
    static const std::set<std::wstring> with_value = {L"--user", L"--filter", L"--log"};
    Arguments arguments;
    for (int i = 2; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (with_value.count(argument) && i + 1 < argc) {
            arguments.values[argument] = argv[++i];
        } else if (argument.rfind(L"--", 0) == 0) {
            arguments.flags.insert(argument);
        } else {
            arguments.positional.push_back(argument);
        }
    }
    return arguments;
}

// Runs a getter that may throw (package properties of broken or staged packages); null on error.
json try_json(const std::function<json()>& getter) {
    try {
        return getter();
    } catch (const winrt::hresult_error& error) {
        return json{{"error", hex(error.code())}};
    } catch (...) {
        return json{{"error", "exception"}};
    }
}

const char* architecture_name(winrt::Windows::System::ProcessorArchitecture architecture) {
    using winrt::Windows::System::ProcessorArchitecture;
    switch (architecture) {
        case ProcessorArchitecture::X86:
            return "x86";
        case ProcessorArchitecture::X64:
            return "x64";
        case ProcessorArchitecture::Arm:
            return "arm";
        case ProcessorArchitecture::Arm64:
            return "arm64";
        case ProcessorArchitecture::Neutral:
            return "neutral";
        default:
            return "unknown";
    }
}

const char* signature_name(wam::PackageSignatureKind kind) {
    switch (kind) {
        case wam::PackageSignatureKind::None:
            return "none";
        case wam::PackageSignatureKind::Developer:
            return "developer";
        case wam::PackageSignatureKind::Enterprise:
            return "enterprise";
        case wam::PackageSignatureKind::Store:
            return "store";
        case wam::PackageSignatureKind::System:
            return "system";
        default:
            return "unknown";
    }
}

const char* install_state_name(wmd::PackageInstallState state) {
    switch (state) {
        case wmd::PackageInstallState::NotInstalled:
            return "not-installed";
        case wmd::PackageInstallState::Staged:
            return "staged";
        case wmd::PackageInstallState::Installed:
            return "installed";
        case wmd::PackageInstallState::Paused:
            return "paused";
        default:
            return "unknown";
    }
}

json users_json(const wmd::PackageManager& manager, const winrt::hstring& full_name) {
    json users = json::array();
    for (const auto& user : manager.FindUsers(full_name)) {
        users.push_back(
            {{"sid", utf8(user.UserSecurityId())}, {"state", install_state_name(user.InstallState())}});
    }
    return users;
}

json package_json(const wmd::PackageManager& manager, const wam::Package& package, bool with_users) {
    json result;
    const auto id = package.Id();
    result["full_name"] = utf8(id.FullName());
    result["family_name"] = utf8(id.FamilyName());
    result["name"] = utf8(id.Name());
    result["publisher"] = utf8(id.Publisher());
    const auto version = id.Version();
    result["version"] = std::to_string(version.Major) + "." + std::to_string(version.Minor) + "." +
                        std::to_string(version.Build) + "." + std::to_string(version.Revision);
    result["architecture"] = architecture_name(id.Architecture());
    result["resource_id"] = utf8(id.ResourceId());
    result["is_framework"] = try_json([&] { return json(package.IsFramework()); });
    result["is_resource"] = try_json([&] { return json(package.IsResourcePackage()); });
    result["is_bundle"] = try_json([&] { return json(package.IsBundle()); });
    result["is_optional"] = try_json([&] { return json(package.IsOptional()); });
    result["is_development_mode"] = try_json([&] { return json(package.IsDevelopmentMode()); });
    result["is_stub"] = try_json([&] { return json(package.IsStub()); });
    result["signature_kind"] = try_json([&] { return json(signature_name(package.SignatureKind())); });
    result["installed_path"] = try_json([&] { return json(utf8(package.InstalledPath())); });
    result["status"] = try_json([&] {
        const auto status = package.Status();
        json flags = json::array();
        if (status.NotAvailable()) flags.push_back("not-available");
        if (status.PackageOffline()) flags.push_back("offline");
        if (status.DataOffline()) flags.push_back("data-offline");
        if (status.Disabled()) flags.push_back("disabled");
        if (status.NeedsRemediation()) flags.push_back("needs-remediation");
        if (status.LicenseIssue()) flags.push_back("license-issue");
        if (status.Modified()) flags.push_back("modified");
        if (status.Tampered()) flags.push_back("tampered");
        if (status.DependencyIssue()) flags.push_back("dependency-issue");
        if (status.Servicing()) flags.push_back("servicing");
        if (status.DeploymentInProgress()) flags.push_back("deployment-in-progress");
        return json{{"ok", status.VerifyIsOK()}, {"flags", flags}};
    });
    result["dependencies"] = try_json([&] {
        json dependencies = json::array();
        for (const auto& dependency : package.Dependencies()) {
            dependencies.push_back(utf8(dependency.Id().FullName()));
        }
        return dependencies;
    });
    if (with_users) {
        result["users"] = try_json([&] { return users_json(manager, id.FullName()); });
    }
    return result;
}

const char* async_status_name(wf::AsyncStatus status) {
    switch (status) {
        case wf::AsyncStatus::Started:
            return "started";
        case wf::AsyncStatus::Completed:
            return "completed";
        case wf::AsyncStatus::Canceled:
            return "canceled";
        case wf::AsyncStatus::Error:
            return "error";
        default:
            return "unknown";
    }
}

using DeploymentOperation = wf::IAsyncOperationWithProgress<wmd::DeploymentResult, wmd::DeploymentProgress>;

// Starts a deployment operation, waits for it and reports everything its result says: the
// operation's error code, the result's extended error code and text, its activity id (for the
// AppXDeployment-Server event log) and whether the package is registered afterwards.
json run_deployment(const std::function<DeploymentOperation()>& start) {
    json result;
    Stopwatch watch;
    DeploymentOperation operation{nullptr};
    try {
        operation = start();
    } catch (const winrt::hresult_error& error) {
        result["ok"] = false;
        result["hr"] = hex(error.code());
        result["message"] = utf8(error.message());
        result["stage"] = "start";
        result["elapsed_ms"] = watch.ms();
        return result;
    }
    std::int32_t code = 0;
    try {
        operation.get();
    } catch (const winrt::hresult_error& error) {
        code = error.code();
        result["message"] = utf8(error.message());
    }
    result["elapsed_ms"] = watch.ms();
    result["status"] = async_status_name(operation.Status());
    result["hr"] = hex(code);
    result["error_code"] = hex(operation.ErrorCode().value);
    try {
        const auto outcome = operation.GetResults();
        result["extended_error"] = hex(outcome.ExtendedErrorCode().value);
        result["error_text"] = utf8(outcome.ErrorText());
        result["activity_id"] = utf8(winrt::to_hstring(outcome.ActivityId()));
        try {
            result["is_registered"] = outcome.IsRegistered();
        } catch (...) {
        }
    } catch (const winrt::hresult_error& error) {
        result["results_hr"] = hex(error.code());
    }
    result["ok"] = code == 0 && operation.Status() == wf::AsyncStatus::Completed;
    return result;
}

wf::Uri path_uri(const std::wstring& path) {
    wchar_t url[2048];
    DWORD length = static_cast<DWORD>(std::size(url));
    if (FAILED(::UrlCreateFromPathW(path.c_str(), url, &length, 0))) {
        throw winrt::hresult_error(E_INVALIDARG, L"bad path");
    }
    return wf::Uri(url);
}

json whoami() {
    json result;
    wchar_t name[256];
    DWORD size = static_cast<DWORD>(std::size(name));
    if (::GetUserNameW(name, &size)) {
        result["user"] = utf8(name);
    }
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        BYTE buffer[512];
        DWORD length = 0;
        if (::GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &length)) {
            wchar_t* sid = nullptr;
            if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) {
                result["sid"] = utf8(sid);
                ::LocalFree(sid);
            }
        }
        TOKEN_ELEVATION elevation{};
        if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length)) {
            result["elevated"] = elevation.TokenIsElevated != 0;
        }
        DWORD session = 0;
        if (::GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &length)) {
            result["session"] = session;
        }
        ::CloseHandle(token);
    }
    wchar_t profile[MAX_PATH];
    if (::GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        result["profile"] = utf8(profile);
    }
    result["ok"] = true;
    return result;
}

// --- DismAPI, declared from its documentation (the header ships with the Windows ADK). ---

namespace dism {

using Session = UINT;
constexpr wchar_t kOnlineImage[] = L"DISM_{53BFAE52-B167-4E2F-A258-0A37B57FF845}";
constexpr HRESULT kRebootRequired = 3010;  // (HRESULT)ERROR_SUCCESS_REBOOT_REQUIRED

enum LogLevel { LogErrors = 0, LogErrorsWarnings, LogErrorsWarningsInfo };
enum PackageIdentifier { PackageNone = 0, PackageName, PackagePath };
enum FeatureState {
    StateNotPresent = 0,
    StateUninstallPending,
    StateStaged,
    StateRemoved,  // DismStateResolved
    StateInstalled,
    StateInstallPending,
    StateSuperseded,
    StatePartiallyInstalled
};
enum RestartType { RestartNo = 0, RestartPossible, RestartRequired };

#pragma pack(push, 1)
struct String {
    PCWSTR Value;
};
struct Feature {
    PCWSTR FeatureName;
    FeatureState State;
};
struct CustomProperty {
    PCWSTR Name;
    PCWSTR Value;
    PCWSTR Path;
};
struct FeatureInfo {
    PCWSTR FeatureName;
    FeatureState State;
    PCWSTR DisplayName;
    PCWSTR Description;
    RestartType RestartRequired;
    CustomProperty* CustomProperties;
    UINT CustomPropertyCount;
};
struct Capability {
    PCWSTR Name;
    FeatureState State;
};
struct CapabilityInfo {
    PCWSTR Name;
    FeatureState State;
    PCWSTR DisplayName;
    PCWSTR Description;
    DWORD DownloadSize;
    DWORD InstallSize;
};
#pragma pack(pop)

using ProgressCallback = void(CALLBACK*)(UINT current, UINT total, PVOID user_data);

struct Api {
    HRESULT(WINAPI* Initialize)(LogLevel, PCWSTR, PCWSTR) = nullptr;
    HRESULT(WINAPI* Shutdown)() = nullptr;
    HRESULT(WINAPI* OpenSession)(PCWSTR, PCWSTR, PCWSTR, Session*) = nullptr;
    HRESULT(WINAPI* CloseSession)(Session) = nullptr;
    HRESULT(WINAPI* GetLastErrorMessage)(String**) = nullptr;
    HRESULT(WINAPI* Delete)(VOID*) = nullptr;
    HRESULT(WINAPI* GetFeatures)(Session, PCWSTR, PackageIdentifier, Feature**, UINT*) = nullptr;
    HRESULT(WINAPI* GetFeatureInfo)(Session, PCWSTR, PCWSTR, PackageIdentifier, FeatureInfo**) = nullptr;
    HRESULT(WINAPI* EnableFeature)(Session, PCWSTR, PCWSTR, PackageIdentifier, BOOL, PCWSTR*, UINT, BOOL,
                                   HANDLE, ProgressCallback, PVOID) = nullptr;
    HRESULT(WINAPI* DisableFeature)(Session, PCWSTR, PCWSTR, BOOL, HANDLE, ProgressCallback, PVOID) = nullptr;
    HRESULT(WINAPI* GetCapabilities)(Session, Capability**, UINT*) = nullptr;
    HRESULT(WINAPI* GetCapabilityInfo)(Session, PCWSTR, CapabilityInfo**) = nullptr;
    HRESULT(WINAPI* AddCapability)(Session, PCWSTR, BOOL, PCWSTR*, UINT, HANDLE, ProgressCallback,
                                   PVOID) = nullptr;
    HRESULT(WINAPI* RemoveCapability)(Session, PCWSTR, HANDLE, ProgressCallback, PVOID) = nullptr;
};

template <typename Function>
bool bind(HMODULE module, const char* name, Function& function) {
    function = reinterpret_cast<Function>(reinterpret_cast<void*>(::GetProcAddress(module, name)));
    return function != nullptr;
}

const char* state_name(FeatureState state) {
    switch (state) {
        case StateNotPresent:
            return "not-present";
        case StateUninstallPending:
            return "uninstall-pending";
        case StateStaged:
            return "staged";
        case StateRemoved:
            return "removed";
        case StateInstalled:
            return "installed";
        case StateInstallPending:
            return "install-pending";
        case StateSuperseded:
            return "superseded";
        case StatePartiallyInstalled:
            return "partially-installed";
        default:
            return "unknown";
    }
}

const char* restart_name(RestartType restart) {
    switch (restart) {
        case RestartNo:
            return "no";
        case RestartPossible:
            return "possible";
        case RestartRequired:
            return "required";
        default:
            return "unknown";
    }
}

struct Progress {
    UINT calls = 0;
    UINT current = 0;
    UINT total = 0;
};

void CALLBACK on_progress(UINT current, UINT total, PVOID user_data) {
    auto* progress = static_cast<Progress*>(user_data);
    ++progress->calls;
    progress->current = current;
    progress->total = total;
}

// One DISM session on the online image, for one command.
class Client {
public:
    explicit Client(const std::wstring& log_path) {
        module_ = ::LoadLibraryExW(L"DismApi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) {
            error_ = "LoadLibrary DismApi.dll failed: " + hex(static_cast<std::int32_t>(::GetLastError()));
            return;
        }
        const bool bound = bind(module_, "DismInitialize", api_.Initialize) &&
                           bind(module_, "DismShutdown", api_.Shutdown) &&
                           bind(module_, "DismOpenSession", api_.OpenSession) &&
                           bind(module_, "DismCloseSession", api_.CloseSession) &&
                           bind(module_, "DismGetLastErrorMessage", api_.GetLastErrorMessage) &&
                           bind(module_, "DismDelete", api_.Delete) &&
                           bind(module_, "DismGetFeatures", api_.GetFeatures) &&
                           bind(module_, "DismGetFeatureInfo", api_.GetFeatureInfo) &&
                           bind(module_, "DismEnableFeature", api_.EnableFeature) &&
                           bind(module_, "DismDisableFeature", api_.DisableFeature) &&
                           bind(module_, "DismGetCapabilities", api_.GetCapabilities) &&
                           bind(module_, "DismGetCapabilityInfo", api_.GetCapabilityInfo) &&
                           bind(module_, "DismAddCapability", api_.AddCapability) &&
                           bind(module_, "DismRemoveCapability", api_.RemoveCapability);
        if (!bound) {
            error_ = "DismApi.dll lacks an export";
            return;
        }
        Stopwatch watch;
        HRESULT hr =
            api_.Initialize(LogErrorsWarningsInfo, log_path.empty() ? nullptr : log_path.c_str(), nullptr);
        initialize_ms_ = watch.ms();
        if (FAILED(hr)) {
            error_ = "DismInitialize " + hex(hr) + " " + last_error();
            return;
        }
        initialized_ = true;
        Stopwatch open_watch;
        hr = api_.OpenSession(kOnlineImage, nullptr, nullptr, &session_);
        open_ms_ = open_watch.ms();
        if (FAILED(hr)) {
            error_ = "DismOpenSession " + hex(hr) + " " + last_error();
            return;
        }
        open_ = true;
    }

    ~Client() {
        if (open_) {
            api_.CloseSession(session_);
        }
        if (initialized_) {
            api_.Shutdown();
        }
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool ready() const { return open_; }
    const std::string& error() const { return error_; }
    const Api& api() const { return api_; }
    Session session() const { return session_; }

    void describe(json& result) const {
        result["dism_initialize_ms"] = initialize_ms_;
        result["dism_open_session_ms"] = open_ms_;
        if (!error_.empty()) {
            result["ok"] = false;
            result["message"] = error_;
        }
    }

    std::string last_error() const {
        String* message = nullptr;
        if (api_.GetLastErrorMessage && SUCCEEDED(api_.GetLastErrorMessage(&message)) && message) {
            std::string text = message->Value ? utf8(message->Value) : std::string();
            api_.Delete(message);
            return text;
        }
        return {};
    }

    // Fills hr, ok, reboot_required and the error message of a call's result.
    void outcome(json& result, HRESULT hr) const {
        result["hr"] = hex(hr);
        result["reboot_required"] = hr == kRebootRequired;
        result["ok"] = SUCCEEDED(hr);
        if (FAILED(hr)) {
            result["message"] = last_error();
        }
    }

private:
    HMODULE module_ = nullptr;
    Api api_;
    Session session_ = 0;
    bool initialized_ = false;
    bool open_ = false;
    long long initialize_ms_ = 0;
    long long open_ms_ = 0;
    std::string error_;
};

json feature_info(const Client& client, const std::wstring& name) {
    json result;
    FeatureInfo* info = nullptr;
    const HRESULT hr =
        client.api().GetFeatureInfo(client.session(), name.c_str(), nullptr, PackageNone, &info);
    client.outcome(result, hr);
    if (SUCCEEDED(hr) && info) {
        result["name"] = utf8(info->FeatureName ? info->FeatureName : L"");
        result["state"] = state_name(info->State);
        result["display_name"] = utf8(info->DisplayName ? info->DisplayName : L"");
        result["restart"] = restart_name(info->RestartRequired);
        json properties = json::array();
        for (UINT i = 0; i < info->CustomPropertyCount && i < 50; ++i) {
            const auto& property = info->CustomProperties[i];
            properties.push_back({{"name", utf8(property.Name ? property.Name : L"")},
                                  {"value", utf8(property.Value ? property.Value : L"")},
                                  {"path", utf8(property.Path ? property.Path : L"")}});
        }
        result["custom_properties"] = properties;
        client.api().Delete(info);
    }
    return result;
}

json capability_info(const Client& client, const std::wstring& name) {
    json result;
    CapabilityInfo* info = nullptr;
    const HRESULT hr = client.api().GetCapabilityInfo(client.session(), name.c_str(), &info);
    client.outcome(result, hr);
    if (SUCCEEDED(hr) && info) {
        result["name"] = utf8(info->Name ? info->Name : L"");
        result["state"] = state_name(info->State);
        result["display_name"] = utf8(info->DisplayName ? info->DisplayName : L"");
        result["download_size"] = info->DownloadSize;
        result["install_size"] = info->InstallSize;
        client.api().Delete(info);
    }
    return result;
}

}  // namespace dism

int usage() {
    std::fputs("usage: avel_package_probe <command> [arguments]; see package_probe.cpp\n", stderr);
    return 2;
}

json run_package_command(const std::wstring& command, const Arguments& arguments, bool& usage_error) {
    wmd::PackageManager manager;
    const auto& positional = arguments.positional;
    auto need = [&](size_t count) {
        if (positional.size() < count) {
            usage_error = true;
            return false;
        }
        return true;
    };
    constexpr auto kAllTypes = wmd::PackageTypes::Main | wmd::PackageTypes::Framework |
                               wmd::PackageTypes::Resource | wmd::PackageTypes::Bundle |
                               wmd::PackageTypes::Optional;

    if (command == L"list") {
        json result;
        const std::wstring filter = arguments.value(L"--filter");
        const bool all_users = arguments.flag(L"--all-users");
        const bool with_users = arguments.flag(L"--users") || all_users;
        Stopwatch watch;
        json packages = json::array();
        auto add = [&](const wam::Package& package) {
            if (contains(package.Id().FullName(), filter)) {
                packages.push_back(package_json(manager, package, with_users));
            }
        };
        if (all_users) {
            for (const auto& package : manager.FindPackagesWithPackageTypes(kAllTypes)) add(package);
        } else {
            for (const auto& package :
                 manager.FindPackagesForUserWithPackageTypes(arguments.value(L"--user"), kAllTypes)) {
                add(package);
            }
        }
        result["ok"] = true;
        result["elapsed_ms"] = watch.ms();
        result["count"] = packages.size();
        result["packages"] = packages;
        return result;
    }
    if (command == L"find") {
        if (!need(1)) return {};
        json result;
        const winrt::hstring full_name{positional[0]};
        result["any_user"] = try_json([&] {
            const auto package = manager.FindPackage(full_name);
            return package ? package_json(manager, package, false) : json(nullptr);
        });
        result["for_user"] = try_json([&] {
            const auto package = manager.FindPackageForUser(arguments.value(L"--user"), full_name);
            return json(package != nullptr);
        });
        result["users"] = try_json([&] { return users_json(manager, full_name); });
        result["ok"] = true;
        return result;
    }
    if (command == L"provisioned") {
        json result;
        const std::wstring filter = arguments.value(L"--filter");
        json packages = json::array();
        for (const auto& package : manager.FindProvisionedPackages()) {
            if (contains(package.Id().FullName(), filter)) {
                packages.push_back(package_json(manager, package, false));
            }
        }
        result["ok"] = true;
        result["count"] = packages.size();
        result["packages"] = packages;
        return result;
    }
    if (command == L"add") {
        if (!need(1)) return {};
        const auto uri = path_uri(positional[0]);
        const bool unsigned_ok = arguments.flag(L"--allow-unsigned");
        if (arguments.flag(L"--stage-only")) {
            return run_deployment([&] {
                wmd::StagePackageOptions options;
                options.AllowUnsigned(unsigned_ok);
                return manager.StagePackageByUriAsync(uri, options);
            });
        }
        return run_deployment([&] {
            wmd::AddPackageOptions options;
            options.AllowUnsigned(unsigned_ok);
            return manager.AddPackageByUriAsync(uri, options);
        });
    }
    if (command == L"remove") {
        if (!need(1)) return {};
        auto options = wmd::RemovalOptions::None;
        if (arguments.flag(L"--all-users")) options |= wmd::RemovalOptions::RemoveForAllUsers;
        if (arguments.flag(L"--preserve-data")) options |= wmd::RemovalOptions::PreserveApplicationData;
        json result = run_deployment([&] { return manager.RemovePackageAsync(positional[0], options); });
        result["removal_options"] = static_cast<std::uint32_t>(options);
        return result;
    }
    if (command == L"cleanup-user") {
        if (!need(2)) return {};
        return run_deployment(
            [&] { return manager.CleanupPackageForUserAsync(positional[0], positional[1]); });
    }
    if (command == L"register-full") {
        if (!need(1)) return {};
        if (arguments.flag(L"--allow-unsigned")) {
            return run_deployment([&] {
                wmd::RegisterPackageOptions options;
                options.AllowUnsigned(true);
                return manager.RegisterPackagesByFullNameAsync(
                    std::vector<winrt::hstring>{winrt::hstring(positional[0])}, options);
            });
        }
        return run_deployment([&] {
            return manager.RegisterPackageByFullNameAsync(positional[0], nullptr,
                                                          wmd::DeploymentOptions::None);
        });
    }
    if (command == L"register-family") {
        if (!need(1)) return {};
        return run_deployment([&] {
            return manager.RegisterPackageByFamilyNameAsync(positional[0], nullptr,
                                                            wmd::DeploymentOptions::None, nullptr, nullptr);
        });
    }
    if (command == L"register-manifest") {
        if (!need(1)) return {};
        const auto uri = path_uri(positional[0]);
        return run_deployment([&] {
            wmd::RegisterPackageOptions options;
            options.AllowUnsigned(arguments.flag(L"--allow-unsigned"));
            options.DeveloperMode(arguments.flag(L"--dev-mode"));
            return manager.RegisterPackageByUriAsync(uri, options);
        });
    }
    if (command == L"provision") {
        if (!need(1)) return {};
        return run_deployment([&] { return manager.ProvisionPackageForAllUsersAsync(positional[0]); });
    }
    if (command == L"deprovision") {
        if (!need(1)) return {};
        return run_deployment([&] { return manager.DeprovisionPackageForAllUsersAsync(positional[0]); });
    }
    if (command == L"removal-pending") {
        if (!need(1)) return {};
        json result;
        result["pending"] = arguments.has_value(L"--user") ? manager.IsPackageRemovalPendingForUser(
                                                                 positional[0], arguments.value(L"--user"))
                                                           : manager.IsPackageRemovalPending(positional[0]);
        result["ok"] = true;
        return result;
    }
    usage_error = true;
    return {};
}

json run_dism_command(const std::wstring& command, const Arguments& arguments, bool& usage_error) {
    const auto& positional = arguments.positional;
    const bool named = command != L"dism-features" && command != L"dism-capabilities";
    if (named && positional.empty()) {
        usage_error = true;
        return {};
    }
    json result;
    dism::Client client(arguments.value(L"--log"));
    client.describe(result);
    if (!client.ready()) {
        return result;
    }
    const auto& api = client.api();
    const std::wstring filter = arguments.value(L"--filter");
    dism::Progress progress;
    Stopwatch watch;
    if (command == L"dism-features") {
        dism::Feature* features = nullptr;
        UINT count = 0;
        const HRESULT hr = api.GetFeatures(client.session(), nullptr, dism::PackageNone, &features, &count);
        client.outcome(result, hr);
        json list = json::array();
        for (UINT i = 0; SUCCEEDED(hr) && i < count; ++i) {
            if (features[i].FeatureName && contains(features[i].FeatureName, filter)) {
                list.push_back(
                    {{"name", utf8(features[i].FeatureName)}, {"state", state_name(features[i].State)}});
            }
        }
        if (features) api.Delete(features);
        result["count"] = count;
        result["features"] = list;
    } else if (command == L"dism-capabilities") {
        dism::Capability* capabilities = nullptr;
        UINT count = 0;
        const HRESULT hr = api.GetCapabilities(client.session(), &capabilities, &count);
        client.outcome(result, hr);
        json list = json::array();
        for (UINT i = 0; SUCCEEDED(hr) && i < count; ++i) {
            if (capabilities[i].Name && contains(capabilities[i].Name, filter)) {
                list.push_back(
                    {{"name", utf8(capabilities[i].Name)}, {"state", state_name(capabilities[i].State)}});
            }
        }
        if (capabilities) api.Delete(capabilities);
        result["count"] = count;
        result["capabilities"] = list;
    } else if (command == L"dism-feature-info") {
        result["info"] = dism::feature_info(client, positional[0]);
        result["ok"] = result["info"]["ok"];
        result["hr"] = result["info"]["hr"];
    } else if (command == L"dism-capability-info") {
        result["info"] = dism::capability_info(client, positional[0]);
        result["ok"] = result["info"]["ok"];
        result["hr"] = result["info"]["hr"];
    } else if (command == L"dism-enable") {
        result["before"] = dism::feature_info(client, positional[0]);
        Stopwatch call;
        const HRESULT hr = api.EnableFeature(
            client.session(), positional[0].c_str(), nullptr, dism::PackageNone, TRUE, nullptr, 0,
            arguments.flag(L"--enable-all") ? TRUE : FALSE, nullptr, dism::on_progress, &progress);
        result["call_ms"] = call.ms();
        client.outcome(result, hr);
        result["after"] = dism::feature_info(client, positional[0]);
    } else if (command == L"dism-disable") {
        result["before"] = dism::feature_info(client, positional[0]);
        Stopwatch call;
        const HRESULT hr = api.DisableFeature(client.session(), positional[0].c_str(), nullptr,
                                              arguments.flag(L"--remove-payload") ? TRUE : FALSE, nullptr,
                                              dism::on_progress, &progress);
        result["call_ms"] = call.ms();
        client.outcome(result, hr);
        result["after"] = dism::feature_info(client, positional[0]);
    } else if (command == L"dism-add-capability") {
        result["before"] = dism::capability_info(client, positional[0]);
        Stopwatch call;
        const HRESULT hr = api.AddCapability(client.session(), positional[0].c_str(), TRUE, nullptr, 0,
                                             nullptr, dism::on_progress, &progress);
        result["call_ms"] = call.ms();
        client.outcome(result, hr);
        result["after"] = dism::capability_info(client, positional[0]);
    } else if (command == L"dism-remove-capability") {
        result["before"] = dism::capability_info(client, positional[0]);
        Stopwatch call;
        const HRESULT hr = api.RemoveCapability(client.session(), positional[0].c_str(), nullptr,
                                                dism::on_progress, &progress);
        result["call_ms"] = call.ms();
        client.outcome(result, hr);
        result["after"] = dism::capability_info(client, positional[0]);
    } else {
        usage_error = true;
        return {};
    }
    result["elapsed_ms"] = watch.ms();
    if (progress.calls) {
        result["progress"] = {
            {"calls", progress.calls}, {"current", progress.current}, {"total", progress.total}};
    }
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::wstring command = argv[1];
    const Arguments arguments = parse(argc, argv);
    json result;
    bool usage_error = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (command == L"whoami") {
            result = whoami();
        } else if (command.rfind(L"dism-", 0) == 0) {
            result = run_dism_command(command, arguments, usage_error);
        } else {
            result = run_package_command(command, arguments, usage_error);
        }
    } catch (const winrt::hresult_error& error) {
        result["ok"] = false;
        result["hr"] = hex(error.code());
        result["message"] = utf8(error.message());
        result["stage"] = "call";
    }
    if (usage_error) {
        return usage();
    }
    json output;
    output["command"] = utf8(command);
    output["arguments"] = json::array();
    for (int i = 2; i < argc; ++i) {
        output["arguments"].push_back(utf8(argv[i]));
    }
    output.update(result);
    std::fputs(output.dump(1).c_str(), stdout);
    std::fputs("\n", stdout);
    return output.value("ok", false) ? 0 : 1;
}
