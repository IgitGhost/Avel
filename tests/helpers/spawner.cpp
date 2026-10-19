// Test helper for process_runner_test: behaves like uninstallers that hand off to a copy of
// themselves and exit at once.
//
//   avel_test_spawner parent <exit-code> <child-sleep-ms> <marker-file>
//       Starts itself in child mode and exits immediately with <exit-code>.
//   avel_test_spawner child <sleep-ms> <marker-file>
//       Sleeps, then creates <marker-file>.

#include <windows.h>

#include <string>

int wmain(int argc, wchar_t* argv[]) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"parent" && argc == 5) {
        wchar_t self[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = std::wstring(L"\"") + self + L"\" child " + argv[3] + L" \"" + argv[4] + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                              nullptr, &startup, &process)) {
            return 100;
        }
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return _wtoi(argv[2]);
    }
    if (mode == L"child" && argc == 4) {
        ::Sleep(static_cast<DWORD>(_wtoi(argv[2])));
        const HANDLE marker =
            ::CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker == INVALID_HANDLE_VALUE) {
            return 101;
        }
        ::CloseHandle(marker);
        return 0;
    }
    return 2;
}
