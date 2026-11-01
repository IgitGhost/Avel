#include "core/platform/shortcuts.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>

#include <cwchar>

namespace avel::platform {

std::optional<ShortcutTarget> resolve_shortcut(const std::wstring& file) {
    wil::com_ptr_nothrow<IShellLinkW> link;
    if (FAILED(
            ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) {
        return std::nullopt;
    }
    const auto persist = link.try_query<IPersistFile>();
    if (!persist || FAILED(persist->Load(file.c_str(), STGM_READ))) {
        return std::nullopt;
    }
    ShortcutTarget result;
    if (const auto data = link.try_query<IShellLinkDataList>()) {
        DWORD flags = 0;
        result.advertised = SUCCEEDED(data->GetFlags(&flags)) && (flags & SLDF_HAS_DARWINID) != 0;
    }
    wchar_t target[MAX_PATH] = {};
    // The raw path keeps %VARIABLES%. Advertised shortcuts have none (S_FALSE).
    if (link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH) == S_OK) {
        result.target = target;
    }
    // A link stores up to 64K characters of arguments; GetArguments cuts them at the buffer's size.
    std::wstring arguments(32768, L'\0');
    if (SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size())))) {
        arguments.resize(::wcsnlen(arguments.c_str(), arguments.size()));
        result.arguments = std::move(arguments);
    }
    return result;
}

}  // namespace avel::platform
