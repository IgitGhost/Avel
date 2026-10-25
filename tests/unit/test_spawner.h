// Where the avel_test_spawner helper (tests/helpers/spawner.cpp) is: next to the test program when
// it is there, as in the build's bin folder wherever that is mapped (Windows Sandbox runs the
// elevated tests from C:\Avel\bin), otherwise where the build put it.
#pragma once

#include <windows.h>

#include <algorithm>
#include <string>

namespace avel::testing {

inline std::wstring spawner_path() {
    wchar_t module[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring path(module, length);
        path.resize(path.find_last_of(L'\\') + 1);
        path += L"avel_test_spawner.exe";
        if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    std::wstring path = AVEL_TEST_SPAWNER;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

}  // namespace avel::testing
