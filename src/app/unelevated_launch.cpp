#include "unelevated_launch.h"

#include <objbase.h>
#include <oleauto.h>
#include <exdisp.h>
#include <sddl.h>
#include <servprov.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <cstddef>

namespace avel::app {

namespace {

VARIANT stringVariant(BSTR text) {
    VARIANT variant;
    ::VariantInit(&variant);
    variant.vt = VT_BSTR;
    variant.bstrVal = text;  // Borrowed: the caller's unique_bstr frees it
    return variant;
}

// Raymond Chen's "How can I launch an unelevated process from my elevated process?": the desktop's
// folder view, its automation object, and that object's Shell.Application.
HRESULT shellExecuteThroughDesktop(const std::wstring& program, const std::wstring& arguments) {
    wil::com_ptr_nothrow<IShellWindows> windows;
    HRESULT hr =
        ::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put()));
    if (FAILED(hr)) {
        return hr;
    }
    VARIANT location;
    ::VariantInit(&location);
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT empty;
    ::VariantInit(&empty);
    long window = 0;
    wil::com_ptr_nothrow<IDispatch> desktop;
    hr = windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, desktop.put());
    if (hr != S_OK || !desktop) {
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);  // S_FALSE: no desktop window
    }
    const auto provider = desktop.try_query<IServiceProvider>();
    if (!provider) {
        return E_NOINTERFACE;
    }
    wil::com_ptr_nothrow<IShellBrowser> browser;
    hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.put()));
    if (FAILED(hr)) {
        return hr;
    }
    wil::com_ptr_nothrow<IShellView> view;
    hr = browser->QueryActiveShellView(view.put());
    if (FAILED(hr)) {
        return hr;
    }
    wil::com_ptr_nothrow<IDispatch> background;
    hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.put()));
    if (FAILED(hr)) {
        return hr;
    }
    const auto folderView = background.try_query<IShellFolderViewDual>();
    if (!folderView) {
        return E_NOINTERFACE;
    }
    wil::com_ptr_nothrow<IDispatch> application;
    hr = folderView->get_Application(application.put());
    if (FAILED(hr)) {
        return hr;
    }
    const auto shell = application.try_query<IShellDispatch2>();
    if (!shell) {
        return E_NOINTERFACE;
    }
    const auto file = wil::make_bstr_nothrow(program.c_str());
    const auto parameters = wil::make_bstr_nothrow(arguments.c_str());
    const auto none = wil::make_bstr_nothrow(L"");
    if (!file || !parameters || !none) {
        return E_OUTOFMEMORY;
    }
    VARIANT show;
    ::VariantInit(&show);
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    // No folder and no verb: the program's default ("open") in Explorer's current folder.
    return shell->ShellExecute(file.get(), stringVariant(parameters.get()), stringVariant(none.get()),
                               stringVariant(none.get()), show);
}

// One argument as CommandLineToArgvW reads it back: quoted when it is empty or holds a space, a tab or
// a quote; a quote escaped, and the backslashes before a quote (or the closing one) doubled.
std::wstring quoted(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring result = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            result.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
        } else {
            result.append(backslashes, L'\\');
        }
        result.push_back(*it);
    }
    result.push_back(L'"');
    return result;
}

}  // namespace

std::wstring shellUserSid() {
    const HWND shell = ::GetShellWindow();
    DWORD pid = 0;
    if (shell == nullptr || ::GetWindowThreadProcessId(shell, &pid) == 0 || pid == 0) {
        return {};
    }
    const wil::unique_handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wil::unique_handle token;
    if (!process || !::OpenProcessToken(process.get(), TOKEN_QUERY, token.put())) {
        return {};
    }
    DWORD size = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return {};
    }
    std::vector<std::byte> buffer(size);
    if (!::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
        return {};
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wil::unique_hlocal_string text;
    if (!::ConvertSidToStringSidW(user->User.Sid, text.put())) {
        return {};
    }
    return text.get();
}

HRESULT launchUnelevated(const std::wstring& program, const std::wstring& arguments) {
    // S_OK and S_FALSE each need their CoUninitialize; RPC_E_CHANGED_MODE (a multithreaded
    // apartment already) works as it is.
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT hr = shellExecuteThroughDesktop(program, arguments);
    if (SUCCEEDED(init)) {
        ::CoUninitialize();
    }
    return hr;
}

std::wstring joinArguments(const std::vector<std::wstring>& arguments) {
    std::wstring line;
    for (const auto& argument : arguments) {
        if (!line.empty()) {
            line.push_back(L' ');
        }
        line += quoted(argument);
    }
    return line;
}

}  // namespace avel::app
