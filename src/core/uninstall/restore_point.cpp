#include "core/uninstall/restore_point.h"

#include <windows.h>
#include <objbase.h>
#include <srrestoreptapi.h>

#include <algorithm>

namespace avel::uninstall {

namespace {

using SetRestorePointFunction = BOOL(WINAPI*)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);

// srclient.dll ships only when System Restore is part of Windows (not on Server Core, for
// example), and the SDK has no import library for it, so it's loaded on demand.
SetRestorePointFunction set_restore_point_function() {
    static const auto function = [] {
        const HMODULE module = ::LoadLibraryExW(L"srclient.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module
                   ? reinterpret_cast<SetRestorePointFunction>(::GetProcAddress(module, "SRSetRestorePointW"))
                   : nullptr;
    }();
    return function;
}

// System Restore talks to its service over COM, so the calling thread needs COM.
class ComScope {
public:
    ComScope() : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(result_)) {
            ::CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT result_;
};

}  // namespace

RestorePointResult begin_restore_point(std::wstring_view description) {
    RestorePointResult result;
    const auto set_restore_point = set_restore_point_function();
    if (set_restore_point == nullptr) {
        result.error = ERROR_NOT_SUPPORTED;
        return result;
    }
    const ComScope com;
    RESTOREPOINTINFOW info{};
    info.dwEventType = BEGIN_SYSTEM_CHANGE;
    info.dwRestorePtType = APPLICATION_UNINSTALL;
    const size_t length = std::min(description.size(), static_cast<size_t>(MAX_DESC_W - 1));
    std::copy_n(description.data(), length, info.szDescription);
    STATEMGRSTATUS status{};
    if (!set_restore_point(&info, &status)) {
        result.error = status.nStatus != ERROR_SUCCESS ? status.nStatus : ERROR_INTERNAL_ERROR;
        return result;
    }
    result.sequence = status.llSequenceNumber;
    return result;
}

void end_restore_point(std::int64_t sequence, bool changes_made) {
    const auto set_restore_point = set_restore_point_function();
    if (set_restore_point == nullptr) {
        return;
    }
    const ComScope com;
    RESTOREPOINTINFOW info{};
    info.dwEventType = END_SYSTEM_CHANGE;
    info.dwRestorePtType = changes_made ? APPLICATION_UNINSTALL : CANCELLED_OPERATION;
    info.llSequenceNumber = sequence;
    STATEMGRSTATUS status{};
    set_restore_point(&info, &status);
}

}  // namespace avel::uninstall
