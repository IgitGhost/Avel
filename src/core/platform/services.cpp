#include "core/platform/services.h"

#include <wil/resource.h>

#include <algorithm>
#include <cstddef>

#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::platform {

namespace {

constexpr DWORD kDriverTypes =
    SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_ADAPTER | SERVICE_RECOGNIZER_DRIVER;

// The ServiceDll of a service svchost.exe hosts: under its Parameters key, or (older services) its
// own key.
std::wstring service_dll(const std::wstring& name) {
    const std::wstring path = LR"(SYSTEM\CurrentControlSet\Services\)" + name;
    for (const auto* subkey : {LR"(\Parameters)", L""}) {
        RegistryKey key;
        if (RegistryKey::open(HKEY_LOCAL_MACHINE, path + subkey, KEY_READ | KEY_WOW64_64KEY, key) !=
            ERROR_SUCCESS) {
            continue;
        }
        const auto values = key.values();
        if (const auto* value = find_value(values, L"ServiceDll")) {
            if (const auto text = value->as_string(); text && !text->empty()) {
                return expand_environment(*text);
            }
        }
    }
    return {};
}

// The error of the call that just failed, never ERROR_SUCCESS.
DWORD last_error() {
    const DWORD error = ::GetLastError();
    return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
}

// Returns ERROR_SUCCESS or the error that kept the configuration from being read.
DWORD read_config(SC_HANDLE manager, ServiceInfo& info) {
    wil::unique_schandle service(::OpenServiceW(manager, info.name.c_str(), SERVICE_QUERY_CONFIG));
    if (!service) {
        return last_error();
    }
    DWORD needed = 0;
    ::QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    if (needed == 0) {
        return last_error();
    }
    // QUERY_SERVICE_CONFIGW holds pointers: allocate it as such so it is aligned.
    std::vector<std::uint64_t> buffer((needed + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!::QueryServiceConfigW(service.get(), config,
                               static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t)), &needed)) {
        return last_error();
    }
    if (info.display_name.empty() && config->lpDisplayName) {
        info.display_name = config->lpDisplayName;
    }
    info.type = config->dwServiceType;
    info.start_type = config->dwStartType;
    info.image_path = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    if (::QueryServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                               reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &needed)) {
        info.delayed = delayed.fDelayedAutostart != FALSE;
    }
    if (icontains(info.image_path, L"svchost.exe")) {
        info.service_dll = service_dll(info.name);
    }
    return ERROR_SUCCESS;
}

}  // namespace

bool is_driver_type(std::uint32_t type) { return (type & kDriverTypes) != 0; }

DWORD win32_services(std::vector<ServiceInfo>& services) {
    services.clear();
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!manager) {
        return last_error();
    }
    std::vector<ServiceInfo> listed;
    // ENUM_SERVICE_STATUS_PROCESSW holds pointers: allocate the buffer as such so it is aligned.
    std::vector<std::uint64_t> buffer(64 * 1024 / sizeof(std::uint64_t));
    DWORD resume = 0;
    while (true) {
        DWORD needed = 0;
        DWORD count = 0;
        const BOOL done = ::EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                                                  SERVICE_STATE_ALL, reinterpret_cast<BYTE*>(buffer.data()),
                                                  static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t)),
                                                  &needed, &count, &resume, nullptr);
        const DWORD error = done ? ERROR_SUCCESS : ::GetLastError();
        if (!done && error != ERROR_MORE_DATA) {
            return error;
        }
        const auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            ServiceInfo info;
            info.name = entries[i].lpServiceName ? entries[i].lpServiceName : L"";
            info.display_name = entries[i].lpDisplayName ? entries[i].lpDisplayName : L"";
            info.type = entries[i].ServiceStatusProcess.dwServiceType;
            listed.push_back(std::move(info));
        }
        if (done) {
            break;
        }
        if (count == 0) {
            // Not even one entry fit: make room for what the next call needs.
            buffer.resize(std::max<std::size_t>(
                buffer.size() * 2, (needed + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t)));
        }
    }
    for (auto& info : listed) {
        // Services this account may not query, or that were deleted meanwhile, are left out.
        if (!info.name.empty() && !is_driver_type(info.type) &&
            read_config(manager.get(), info) == ERROR_SUCCESS) {
            services.push_back(std::move(info));
        }
    }
    return ERROR_SUCCESS;
}

DWORD query_service(const std::wstring& name, ServiceInfo& info) {
    info = {};
    info.name = name;
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return last_error();
    }
    return read_config(manager.get(), info);
}

std::wstring service_file(const ServiceInfo& info) {
    if (!info.service_dll.empty()) {
        return info.service_dll;
    }
    const auto paths = referenced_paths(info.image_path);
    return paths.empty() ? std::wstring() : paths.front();
}

}  // namespace avel::platform
