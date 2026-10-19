#include "core/changes/locks.h"

#include <windows.h>
#include <restartmanager.h>

#include <wil/resource.h>

#include <algorithm>

namespace avel::changes {

std::vector<LockingProcess> processes_using(const std::vector<std::wstring>& files) {
    std::vector<LockingProcess> processes;
    if (files.empty()) {
        return processes;
    }
    DWORD session = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1] = {};
    if (::RmStartSession(&session, 0, key) != ERROR_SUCCESS) {
        return processes;
    }
    const auto end = wil::scope_exit([&] { ::RmEndSession(session); });

    std::vector<LPCWSTR> names;
    for (std::size_t i = 0; i < files.size() && i < 1000; ++i) {
        names.push_back(files[i].c_str());
    }
    if (::RmRegisterResources(session, static_cast<UINT>(names.size()), names.data(), 0, nullptr, 0, nullptr) !=
        ERROR_SUCCESS) {
        return processes;
    }
    std::vector<RM_PROCESS_INFO> info(16);
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT needed = 0;
        UINT count = static_cast<UINT>(info.size());
        DWORD reasons = 0;
        const DWORD status = ::RmGetList(session, &needed, &count, info.data(), &reasons);
        if (status == ERROR_MORE_DATA) {
            info.resize(needed + 4);  // The list can grow between calls
            continue;
        }
        if (status != ERROR_SUCCESS) {
            return processes;
        }
        for (UINT i = 0; i < count; ++i) {
            const auto& entry = info[i];
            processes.push_back({entry.Process.dwProcessId, entry.strAppName,
                                 entry.ApplicationType == RmService, entry.ApplicationType == RmCritical});
        }
        return processes;
    }
    return processes;
}

}  // namespace avel::changes
