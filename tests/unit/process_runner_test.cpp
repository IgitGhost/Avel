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

TEST_F(ProcessRunnerTest, ReportsAMissingExecutable) {
    Cancellation cancellation;
    const ProcessResult result = run_and_wait(LR"(C:\Avel.Tests\does-not-exist.exe)", L"", cancellation);
    EXPECT_FALSE(result.started());
    EXPECT_TRUE(result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND) << result.error;
}

}  // namespace
}  // namespace avel::uninstall
