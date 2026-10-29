// Runs the avel_test_spawner helper (tests/helpers/spawner.cpp), which starts a child process and
// exits at once, the way NSIS and Inno Setup uninstallers do.
#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include "core/uninstall/process_runner.h"
#include "test_spawner.h"

namespace avel::uninstall {
namespace {

using namespace std::chrono_literals;

using testing::spawner_path;

class ProcessRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
        marker_ = std::filesystem::temp_directory_path() /
                  (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                   std::filesystem::path(test->name()).wstring() + L".marker");
        std::filesystem::remove(marker_);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove(marker_, ignored);
    }

    std::wstring parent_arguments(int exit_code, int child_sleep_ms) const {
        return L"parent " + std::to_wstring(exit_code) + L" " + std::to_wstring(child_sleep_ms) + L" \"" +
               marker_.wstring() + L"\"";
    }

    std::filesystem::path marker_;
};

TEST_F(ProcessRunnerTest, WaitsForProcessesStartedByTheUninstaller) {
    Cancellation cancellation;
    const auto started = std::chrono::steady_clock::now();
    const ProcessResult result = run_and_wait(spawner_path(), parent_arguments(7, 700), cancellation);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(result.started()) << result.error;
    EXPECT_EQ(result.exit_code, 7u);  // The first process's exit code, not the child's
    EXPECT_FALSE(result.terminated);
    EXPECT_GE(elapsed, 600ms);  // The parent exits at once; the child sleeps 700 ms
    EXPECT_TRUE(std::filesystem::exists(marker_));
}

TEST_F(ProcessRunnerTest, TerminationKillsTheWholeProcessTree) {
    Cancellation cancellation;
    auto run = std::async(std::launch::async, [&] {
        return run_and_wait(spawner_path(), parent_arguments(0, 20000), cancellation);
    });
    std::this_thread::sleep_for(500ms);
    cancellation.terminate_current = true;

    ASSERT_EQ(run.wait_for(5s), std::future_status::ready);
    const ProcessResult result = run.get();
    EXPECT_TRUE(result.started());
    EXPECT_TRUE(result.terminated);
    EXPECT_FALSE(std::filesystem::exists(marker_));  // The child never got to write it
}

// Sandbox run 1: with avel-cli's output piped to a script, a console uninstaller (sdbinst) wrote
// into it ahead of the JSON. One gets a console of its own then.
TEST_F(ProcessRunnerTest, AConsoleUninstallerNeverWritesIntoAvelsRedirectedOutput) {
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};  // As a shell's redirection is
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    ASSERT_TRUE(::CreatePipe(&read, &write, &inheritable, 1 << 16));
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE error = ::GetStdHandle(STD_ERROR_HANDLE);
    ::SetStdHandle(STD_OUTPUT_HANDLE, write);
    ::SetStdHandle(STD_ERROR_HANDLE, write);
    wchar_t system[MAX_PATH] = {};
    ::GetSystemDirectoryW(system, MAX_PATH);
    Cancellation cancellation;
    const ProcessResult result =
        run_and_wait(std::wstring(system) + L"\\cmd.exe",
                     L"/c echo avel-leak-marker & echo avel-leak-marker 1>&2", cancellation);
    ::SetStdHandle(STD_OUTPUT_HANDLE, output);
    ::SetStdHandle(STD_ERROR_HANDLE, error);
    ::CloseHandle(write);

    std::string leaked;
    char buffer[512];
    DWORD bytes = 0;
    while (::ReadFile(read, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0) {
        leaked.append(buffer, bytes);
    }
    ::CloseHandle(read);
    ASSERT_TRUE(result.started()) << result.error;
    EXPECT_EQ(result.exit_code, 0u);
    EXPECT_EQ(leaked, "");
}

TEST_F(ProcessRunnerTest, ReportsAMissingExecutable) {
    Cancellation cancellation;
    const ProcessResult result = run_and_wait(LR"(C:\Avel.Tests\does-not-exist.exe)", L"", cancellation);
    EXPECT_FALSE(result.started());
    EXPECT_TRUE(result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND) << result.error;
}

}  // namespace
}  // namespace avel::uninstall
