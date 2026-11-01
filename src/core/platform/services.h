// Services as the Service Control Manager reports them, read with query rights only (no
// administrator rights needed for most services).
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace avel::platform {

struct ServiceInfo {
    std::wstring name;  // The service's key name
    std::wstring display_name;
    std::uint32_t type = 0;  // SERVICE_WIN32_OWN_PROCESS, SERVICE_WIN32_SHARE_PROCESS, ...
    // SERVICE_AUTO_START, SERVICE_DEMAND_START (Manual), SERVICE_DISABLED, ...
    std::uint32_t start_type = SERVICE_DEMAND_START;
    bool delayed = false;     // An automatic start delayed until shortly after boot
    std::wstring image_path;  // The command line as configured, not expanded
    // For a service svchost.exe hosts: its DLL (Parameters\ServiceDll), expanded. Empty otherwise.
    std::wstring service_dll;
};

// Kernel, file system and other drivers.
bool is_driver_type(std::uint32_t type);

// Every Win32 service (own or shared process; never drivers) whose configuration this account may
// read. Returns ERROR_SUCCESS, or the error from opening the Service Control Manager or
// enumerating its services.
DWORD win32_services(std::vector<ServiceInfo>& services);

}  // namespace avel::platform
