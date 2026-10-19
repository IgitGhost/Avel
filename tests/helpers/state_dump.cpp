// avel_state_dump: writes a canonical text dump of registry keys, files, services and scheduled
// tasks, for the Sandbox to compare the system before a removal and after its undo.
//
//   avel_state_dump <spec>...
//     key:HKLM\SOFTWARE\Contoso   (also HKCU\..., HKU\<sid>\...)
//     value:HKLM\SOFTWARE\...\Run|Contoso   (one value of a key other programs share)
//     file:C:\ProgramData\Contoso  (a file, or a folder and everything in it)
//     service:ContosoService       (configuration, description, security and its key)
//     task:\Contoso\Updater        (definition and security)
//
// Each spec's dump follows a line "== <spec>"; something missing dumps as "<missing>".
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <sddl.h>
#include <taskschd.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <format>
#include <string>

#include "core/platform/text.h"
#include "registry_dump.h"
#include "tree_dump.h"

namespace {

using avel::platform::to_utf8;

std::string dump_registry(const std::wstring& spec) {
    const std::size_t slash = spec.find(L'\\');
    const std::wstring root_name = spec.substr(0, slash);
    std::wstring path = slash == std::wstring::npos ? L"" : spec.substr(slash + 1);
    HKEY root = nullptr;
    if (root_name == L"HKLM") {
        root = HKEY_LOCAL_MACHINE;
    } else if (root_name == L"HKCU") {
        root = HKEY_CURRENT_USER;
    } else if (root_name == L"HKU") {
        root = HKEY_USERS;
    } else {
        return "<bad root>\n";
    }
    const std::size_t last = path.find_last_of(L'\\');
    const std::wstring parent = last == std::wstring::npos ? L"" : path.substr(0, last);
    const std::wstring name = last == std::wstring::npos ? path : path.substr(last + 1);
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(root, parent.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &raw) != ERROR_SUCCESS) {
        return "<missing>\n";
    }
    const wil::unique_hkey key(raw);
    return avel::testing::dump_key(key.get(), name);
}

// "HKLM\path|name": one value of a shared key (Run, Approved), whose other values other programs
// may change meanwhile.
std::string dump_value(const std::wstring& spec) {
    const std::size_t bar = spec.find(L'|');
    if (bar == std::wstring::npos) {
        return "<bad spec>\n";
    }
    const std::wstring key_spec = spec.substr(0, bar);
    const std::wstring name = spec.substr(bar + 1);
    const std::size_t slash = key_spec.find(L'\\');
    const std::wstring root_name = key_spec.substr(0, slash);
    const HKEY root = root_name == L"HKLM" ? HKEY_LOCAL_MACHINE : root_name == L"HKCU" ? HKEY_CURRENT_USER : HKEY_USERS;
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(root, key_spec.substr(slash + 1).c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &raw) != ERROR_SUCCESS) {
        return "<missing>\n";
    }
    const wil::unique_hkey key(raw);
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        return "<missing>\n";
    }
    std::vector<std::byte> data(size);
    ::RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size);
    return std::format("type={} data={}\n", type, avel::changes::to_hex(data.data(), size));
}

std::string sddl_of(PSECURITY_DESCRIPTOR descriptor, SECURITY_INFORMATION parts) {
    wil::unique_hlocal_string sddl;
    if (!::ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, parts, &sddl, nullptr)) {
        return "<unreadable>";
    }
    return to_utf8(sddl.get());
}

std::string dump_service(const std::wstring& name) {
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    wil::unique_schandle service(
        manager ? ::OpenServiceW(manager.get(), name.c_str(), SERVICE_QUERY_CONFIG | READ_CONTROL) : nullptr);
    if (!service) {
        return "<missing>\n";
    }
    DWORD needed = 0;
    ::QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<std::byte> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    std::string text;
    if (needed && ::QueryServiceConfigW(service.get(), config, needed, &needed)) {
        std::string dependencies;
        for (const wchar_t* dependency = config->lpDependencies; dependency && *dependency;
             dependency += wcslen(dependency) + 1) {
            dependencies += to_utf8(dependency) + ";";
        }
        const auto text_of = [](const wchar_t* value) { return value ? to_utf8(value) : std::string(); };
        text += std::format("type={} start={} error={} path={} group={} dependencies={} account={} display={}\n",
                            config->dwServiceType, config->dwStartType, config->dwErrorControl,
                            text_of(config->lpBinaryPathName), text_of(config->lpLoadOrderGroup), dependencies,
                            text_of(config->lpServiceStartName), text_of(config->lpDisplayName));
    }
    needed = 0;
    ::QueryServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &needed);
    std::vector<std::byte> description(needed);
    if (needed && ::QueryServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION,
                                         reinterpret_cast<BYTE*>(description.data()), needed, &needed)) {
        const auto* value = reinterpret_cast<SERVICE_DESCRIPTIONW*>(description.data())->lpDescription;
        text += "description=" + (value ? to_utf8(value) : std::string()) + "\n";
    }
    needed = 0;
    ::QueryServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
    std::vector<std::byte> security(needed);
    if (needed && ::QueryServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION, security.data(), needed, &needed)) {
        text += "security=" + sddl_of(security.data(), DACL_SECURITY_INFORMATION) + "\n";
    }
    // The key, without its Security subkey (the security is above, and only SYSTEM may read it).
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, (LR"(SYSTEM\CurrentControlSet\Services\)" + name).c_str(), 0,
                        KEY_READ | KEY_WOW64_64KEY, &raw) == ERROR_SUCCESS) {
        const wil::unique_hkey key(raw);
        std::string dump = avel::testing::dump_key(key.get(), L"");
        // Drop the Security subkey's lines (they start with its hex name at depth 1).
        const std::string security_key = "  key " + avel::testing::hex_of(L"Security");
        std::string filtered;
        bool skipping = false;
        for (std::size_t start = 0; start < dump.size();) {
            std::size_t end = dump.find('\n', start);
            end = end == std::string::npos ? dump.size() : end + 1;
            const std::string line = dump.substr(start, end - start);
            if (line.rfind(security_key, 0) == 0) {
                skipping = true;
            } else if (skipping && line.rfind("    ", 0) != 0) {
                skipping = false;
            }
            if (!skipping) {
                filtered += line;
            }
            start = end;
        }
        text += filtered;
    }
    return text;
}

std::string dump_task(const std::wstring& path) {
    wil::com_ptr_nothrow<ITaskService> service;
    wil::com_ptr_nothrow<ITaskFolder> root;
    wil::com_ptr_nothrow<IRegisteredTask> task;
    const VARIANT empty{};
    const auto root_path = wil::make_bstr_nothrow(L"\\");
    const auto task_path = wil::make_bstr_nothrow(path.c_str());
    if (FAILED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service.put()))) ||
        FAILED(service->Connect(empty, empty, empty, empty)) || FAILED(service->GetFolder(root_path.get(), root.put())) ||
        FAILED(root->GetTask(task_path.get(), task.put()))) {
        return "<missing>\n";
    }
    wil::unique_bstr xml;
    wil::unique_bstr sddl;
    task->get_Xml(&xml);
    task->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &sddl);
    VARIANT_BOOL enabled = VARIANT_FALSE;
    task->get_Enabled(&enabled);
    return std::format("enabled={}\nsecurity={}\n{}\n", enabled != VARIANT_FALSE, sddl ? to_utf8(sddl.get()) : "",
                       xml ? to_utf8(xml.get()) : "");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string output;
    for (int i = 1; i < argc; ++i) {
        const std::wstring spec = argv[i];
        output += "== " + to_utf8(spec) + "\n";
        if (spec.starts_with(L"key:")) {
            output += dump_registry(spec.substr(4));
        } else if (spec.starts_with(L"value:")) {
            output += dump_value(spec.substr(6));
        } else if (spec.starts_with(L"file:")) {
            output += avel::testing::dump_tree(spec.substr(5));
            output += "\n";
        } else if (spec.starts_with(L"service:")) {
            output += dump_service(spec.substr(8));
        } else if (spec.starts_with(L"task:")) {
            output += dump_task(spec.substr(5));
        } else {
            std::fprintf(stderr, "unknown spec: %s\n", to_utf8(spec).c_str());
            return 2;
        }
    }
    std::fwrite(output.data(), 1, output.size(), stdout);
    return 0;
}
