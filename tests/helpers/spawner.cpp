// Test helper for process_runner_test and installer_watch_test: behaves like uninstallers and
// installers that hand off to other processes and exit at once.
//
//   avel_test_spawner parent <exit-code> <child-sleep-ms> <marker-file>
//       Starts itself in child mode and exits immediately with <exit-code>.
//   avel_test_spawner child <sleep-ms> <marker-file>
//       Sleeps, then creates <marker-file>.
//   avel_test_spawner sleep <sleep-ms> [<exit-code>]
//       Sleeps, then exits with <exit-code> (0 by default).
//   avel_test_spawner installer-app <app-sleep-ms>
//       Like an installer that ends with "Run the program now": starts itself, from where it is
//       (not a temporary folder), in sleep mode as a detached program and exits at once.
//   avel_test_spawner installer-helper <helper-sleep-ms>
//       Like NSIS and Inno Setup: copies itself into %TEMP%\avel-test-<pid>.tmp\, starts the copy in
//       sleep mode and exits at once. The test removes the folder.
//   avel_test_spawner installer-breakaway <child-sleep-ms>
//       Like WiX Burn: starts itself in sleep mode with CREATE_BREAKAWAY_FROM_JOB, so the child
//       leaves the job it was started in, and exits at once. Exits with 102 if the child couldn't
//       be started that way.
//   avel_test_spawner print <text>
//       Like a console installer (sc.exe in a setup script): writes <text> to its standard output
//       and standard error, and exits.

#include <windows.h>

#include <string>

namespace {

std::wstring self_path() {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    return self;
}

bool start(const std::wstring& executable, const std::wstring& arguments, DWORD flags) {
    std::wstring command = L"\"" + executable + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup,
                          &process)) {
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"parent" && argc == 5) {
        if (!start(self_path(), std::wstring(L"child ") + argv[3] + L" \"" + argv[4] + L"\"",
                   DETACHED_PROCESS)) {
            return 100;
        }
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
    if (mode == L"sleep" && (argc == 3 || argc == 4)) {
        ::Sleep(static_cast<DWORD>(_wtoi(argv[2])));
        return argc == 4 ? _wtoi(argv[3]) : 0;
    }
    if (mode == L"installer-app" && argc == 3) {
        return start(self_path(), std::wstring(L"sleep ") + argv[2], DETACHED_PROCESS) ? 0 : 100;
    }
    if (mode == L"installer-helper" && argc == 3) {
        wchar_t temp[MAX_PATH + 1] = {};
        ::GetTempPathW(MAX_PATH + 1, temp);
        const std::wstring folder =
            std::wstring(temp) + L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
        const std::wstring copy = folder + L"\\avel_test_spawner.exe";
        if (!::CreateDirectoryW(folder.c_str(), nullptr) ||
            !::CopyFileW(self_path().c_str(), copy.c_str(), FALSE)) {
            return 103;
        }
        return start(copy, std::wstring(L"sleep ") + argv[2], DETACHED_PROCESS) ? 0 : 100;
    }
    if (mode == L"installer-breakaway" && argc == 3) {
        return start(self_path(), std::wstring(L"sleep ") + argv[2],
                     DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB)
                   ? 0
                   : 102;
    }
    if (mode == L"print" && argc == 3) {
        const std::wstring text = argv[2];
        for (const DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
            DWORD written = 0;
            ::WriteFile(::GetStdHandle(which), text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                        &written, nullptr);
        }
        return 0;
    }
    return 2;
}
