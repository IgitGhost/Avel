#include "core/platform/com_security.h"

#include <windows.h>
#include <objbase.h>
#include <sddl.h>

#include <wil/resource.h>

#include <string>
#include <vector>

#include "core/platform/user_profiles.h"

namespace avel::platform {

void initialize_com_security() {
    // COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL for each trustee. The trustees follow the
    // System Restore sample, plus the process's own user so that its other processes (and
    // accessibility tools running as that user) can still call in.
    std::wstring sddl = L"O:BAG:BAD:(A;;0x3;;;SY)(A;;0x3;;;LS)(A;;0x3;;;NS)(A;;0x3;;;BA)";
    if (const std::wstring user = current_user_sid(); !user.empty()) {
        sddl += L"(A;;0x3;;;" + user + L")";
    }
    wil::unique_hlocal_security_descriptor relative;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &relative,
                                                                nullptr)) {
        return;
    }

    // CoInitializeSecurity wants an absolute security descriptor. It's kept for the life of the
    // process, which is always safe whether or not COM copies it.
    DWORD descriptor_size = 0, dacl_size = 0, sacl_size = 0, owner_size = 0, group_size = 0;
    ::MakeAbsoluteSD(relative.get(), nullptr, &descriptor_size, nullptr, &dacl_size, nullptr, &sacl_size,
                     nullptr, &owner_size, nullptr, &group_size);
    static std::vector<BYTE> descriptor, dacl, sacl, owner, group;
    descriptor.resize(descriptor_size);
    dacl.resize(dacl_size);
    sacl.resize(sacl_size);
    owner.resize(owner_size);
    group.resize(group_size);
    if (!::MakeAbsoluteSD(relative.get(), descriptor.data(), &descriptor_size,
                          reinterpret_cast<PACL>(dacl.data()), &dacl_size,
                          reinterpret_cast<PACL>(sacl.data()), &sacl_size, owner.data(), &owner_size,
                          group.data(), &group_size)) {
        return;
    }

    ::CoInitializeSecurity(descriptor.data(), -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                           RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_DISABLE_AAA | EOAC_NO_CUSTOM_MARSHAL,
                           nullptr);
}

}  // namespace avel::platform
