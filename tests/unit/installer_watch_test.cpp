// The installer watch. Real runs use the avel_test_spawner helper (tests/helpers/spawner.cpp),
// which imitates installers that start the installed program, hand off to a copy of themselves in
// Temp, or start a child outside their job; the decisions are also tested on made-up polls.
#include <gtest/gtest.h>

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/tracker/installer_watch.h"
#include "test_spawner.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

using testing::spawner_path;

bool is_running(std::uint32_t pid) {
    const wil::unique_handle process(::OpenProcess(SYNCHRONIZE, FALSE, pid));
    return process && ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
}

std::wstring image_of(HANDLE process) {
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    return ::QueryFullProcessImageNameW(process, 0, path, &size) ? platform::long_path(path) : std::wstring();
}

class InstallerWatchTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Ends what the test's installers left running, then removes their copies in Temp.
        for (const auto& process : seen_) {
            const wil::unique_handle handle(::OpenProcess(
                PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid));
            if (handle && platform::paths_equal(image_of(handle.get()), process.image)) {
                ::TerminateProcess(handle.get(), 1);
                ::WaitForSingleObject(handle.get(), 5000);
            }
        }
        for (const auto& process : seen_) {
            const std::filesystem::path folder = std::filesystem::path(process.image).parent_path();
            if (platform::istarts_with(folder.filename().wstring(), L"avel-test-") &&
                platform::iequals(folder.extension().wstring(), L".tmp")) {
                std::error_code ignored;
                std::filesystem::remove_all(folder, ignored);
            }
        }
    }

    static WatchOptions fast_options() {
        WatchOptions options;
        options.poll = 50ms;
        options.quiet = 300ms;
        options.app_grace = 500ms;
        options.settle_window = 200ms;
        options.settle_cap = 1s;
        options.timeout = 30s;
        options.installer_busy = [] { return false; };  // Windows Installer's real state could be anything
        return options;
    }

    WatchResult watch(const std::wstring& arguments, const WatchOptions& options,
                      const uninstall::Cancellation& cancellation) {
        WatchResult result =
            launch_and_watch(spawner_path(), arguments, options, cancellation,
                             [this](WatchPhase phase, std::span<const WatchProcess> running) {
                                 phases_.push_back(phase);
                                 for (const auto& process : running) {
                                     remember(process);
                                 }
                             });
        for (const auto& process : result.left_running) {
            remember(process);
        }
        return result;
    }

    void remember(const WatchProcess& process) {
        if (std::none_of(seen_.begin(), seen_.end(),
                         [&](const auto& seen) { return seen.pid == process.pid; })) {
            seen_.push_back(process);
        }
    }

    bool saw_phase(WatchPhase phase) const {
        return std::find(phases_.begin(), phases_.end(), phase) != phases_.end();
    }

    std::vector<WatchProcess> seen_;
    std::vector<WatchPhase> phases_;
};

TEST_F(InstallerWatchTest, ReportsTheProgramTheInstallerStartedWithoutWaitingForIt) {
    uninstall::Cancellation cancellation;
    const auto started = Clock::now();
    const WatchResult result = watch(L"installer-app 30000", fast_options(), cancellation);
    const auto elapsed = Clock::now() - started;

    EXPECT_EQ(result.end, WatchEnd::InstallerDone);
    EXPECT_EQ(result.exit_code, 0u);
    EXPECT_FALSE(result.reboot_requested);
    ASSERT_EQ(result.left_running.size(), 1u);
    EXPECT_FALSE(result.left_running[0].helper);
    EXPECT_TRUE(platform::paths_equal(result.left_running[0].image, spawner_path()));
    EXPECT_TRUE(is_running(result.left_running[0].pid));  // Never ended
    EXPECT_GE(elapsed, 800ms);                            // The grace, then the quiet period
    EXPECT_LT(elapsed, 5s);
    EXPECT_TRUE(saw_phase(WatchPhase::WaitingForHelpers));
    EXPECT_TRUE(saw_phase(WatchPhase::Settling));
}

TEST_F(InstallerWatchTest, WaitsForAHelperInTemp) {
    uninstall::Cancellation cancellation;
    const auto started = Clock::now();
    const WatchResult result = watch(L"installer-helper 1500", fast_options(), cancellation);
    const auto elapsed = Clock::now() - started;

    EXPECT_EQ(result.end, WatchEnd::InstallerDone);
    EXPECT_EQ(result.exit_code, 0u);
    EXPECT_TRUE(result.left_running.empty());
    EXPECT_GE(elapsed, 1800ms);  // The helper's 1.5 s, then the quiet period
    EXPECT_LT(elapsed, 6s);
    const auto helper = std::find_if(seen_.begin(), seen_.end(), [](const auto& process) {
        return process.helper && platform::iends_with(process.image, LR"(.tmp\avel_test_spawner.exe)");
    });
    EXPECT_NE(helper, seen_.end());
}

TEST_F(InstallerWatchTest, FollowsAChildThatBrokeAwayFromTheJob) {
    uninstall::Cancellation cancellation;
    const WatchResult result = watch(L"installer-breakaway 30000", fast_options(), cancellation);

    EXPECT_EQ(result.end, WatchEnd::InstallerDone);
    ASSERT_EQ(result.exit_code, 0u) << "the child couldn't break away from the job";
    ASSERT_EQ(result.left_running.size(), 1u);
    EXPECT_FALSE(result.left_running[0].helper);
    EXPECT_TRUE(platform::paths_equal(result.left_running[0].image, spawner_path()));
    // Outside any job of the test's own, the child is in none: it left the watch's job, so only its
    // parent PID can have led the watch to it.
    BOOL test_in_job = TRUE;
    ::IsProcessInJob(::GetCurrentProcess(), nullptr, &test_in_job);
    if (!test_in_job) {
        const wil::unique_handle child(
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.left_running[0].pid));
        BOOL child_in_job = TRUE;
        ASSERT_TRUE(child && ::IsProcessInJob(child.get(), nullptr, &child_in_job));
        EXPECT_FALSE(child_in_job);
    }
}

TEST_F(InstallerWatchTest, WaitsWhileWindowsInstallerIsBusy) {
    uninstall::Cancellation cancellation;
    WatchOptions options = fast_options();
    const auto busy_until = Clock::now() + 1500ms;
    options.installer_busy = [busy_until] { return Clock::now() < busy_until; };
    const auto started = Clock::now();
    const WatchResult result = watch(L"sleep 0", options, cancellation);
    const auto elapsed = Clock::now() - started;

    EXPECT_EQ(result.end, WatchEnd::InstallerDone);
    EXPECT_TRUE(result.left_running.empty());
    EXPECT_GE(elapsed, 1800ms);  // Busy for 1.5 s, then the quiet period
    EXPECT_LT(elapsed, 6s);
    EXPECT_TRUE(saw_phase(WatchPhase::WaitingForWindowsInstaller));
}

TEST_F(InstallerWatchTest, StopsAtOnceAndLeavesTheInstallerRunning) {
    uninstall::Cancellation cancellation;
    auto run =
        std::async(std::launch::async, [&] { return watch(L"sleep 20000", fast_options(), cancellation); });
    std::this_thread::sleep_for(300ms);
    const auto stopped = Clock::now();
    cancellation.stop_batch = true;

    ASSERT_EQ(run.wait_for(5s), std::future_status::ready);
    const WatchResult result = run.get();
    EXPECT_LT(Clock::now() - stopped, 1s);
    EXPECT_EQ(result.end, WatchEnd::Stopped);
    ASSERT_EQ(result.left_running.size(), 1u);
    EXPECT_TRUE(result.left_running[0].helper);  // The installer itself
    EXPECT_TRUE(is_running(result.left_running[0].pid));
}

TEST_F(InstallerWatchTest, TerminationEndsTheInstallersJob) {
    uninstall::Cancellation cancellation;
    auto run = std::async(std::launch::async,
                          [&] { return watch(L"installer-app 20000", fast_options(), cancellation); });
    // Ends it while the program it started still holds the trace.
    std::this_thread::sleep_for(300ms);
    cancellation.stop_batch = true;
    cancellation.terminate_current = true;

    ASSERT_EQ(run.wait_for(10s), std::future_status::ready);
    const WatchResult result = run.get();
    EXPECT_EQ(result.end, WatchEnd::Stopped);
    EXPECT_TRUE(result.left_running.empty());
    for (const auto& process : seen_) {
        EXPECT_FALSE(is_running(process.pid)) << process.pid;
    }
}

TEST_F(InstallerWatchTest, GivesUpAfterTheTimeout) {
    uninstall::Cancellation cancellation;
    WatchOptions options = fast_options();
    options.timeout = 1s;
    const auto started = Clock::now();
    const WatchResult result = watch(L"sleep 20000", options, cancellation);
    const auto elapsed = Clock::now() - started;

    EXPECT_EQ(result.end, WatchEnd::TimedOut);
    EXPECT_GE(elapsed, 1s);
    EXPECT_LT(elapsed, 3s);
    EXPECT_EQ(result.left_running.size(), 1u);
}

TEST_F(InstallerWatchTest, ReportsARebootRequest) {
    uninstall::Cancellation cancellation;
    const WatchResult result = watch(L"sleep 0 3010", fast_options(), cancellation);

    EXPECT_EQ(result.end, WatchEnd::InstallerDone);
    EXPECT_EQ(result.exit_code, 3010u);
    EXPECT_TRUE(result.reboot_requested);
}

TEST_F(InstallerWatchTest, TheInstallerWritesNothingIntoAvelsOutput) {
    // Avel's standard output and error redirected, as `trace run --json > out.json` has them.
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    ASSERT_TRUE(::CreatePipe(&read, &write, nullptr, 0));
    const wil::unique_handle reader(read);
    wil::unique_handle writer(write);
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE error = ::GetStdHandle(STD_ERROR_HANDLE);
    ::SetStdHandle(STD_OUTPUT_HANDLE, writer.get());
    ::SetStdHandle(STD_ERROR_HANDLE, writer.get());
    uninstall::Cancellation cancellation;
    const WatchResult result = watch(L"print [SC]-CreateService-SUCCESS", fast_options(), cancellation);
    ::SetStdHandle(STD_OUTPUT_HANDLE, output);
    ::SetStdHandle(STD_ERROR_HANDLE, error);
    writer.reset();

    EXPECT_EQ(result.end, WatchEnd::InstallerDone);
    EXPECT_EQ(result.exit_code, 0u);
    DWORD available = 0;
    if (::PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr)) {
        EXPECT_EQ(available, 0u) << "the installer wrote into Avel's output";
    } else {
        EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_BROKEN_PIPE));  // Closed, and nothing in it
    }
}

TEST_F(InstallerWatchTest, RefusesPackagesAndReportsAMissingInstaller) {
    uninstall::Cancellation cancellation;
    for (const auto* package : {LR"(C:\Avel.Tests\app.msix)", LR"(C:\Avel.Tests\app.AppxBundle)"}) {
        const WatchResult result = launch_and_watch(package, L"", fast_options(), cancellation, {});
        EXPECT_EQ(result.end, WatchEnd::LaunchFailed) << package;
        EXPECT_EQ(result.error, static_cast<std::uint32_t>(ERROR_NOT_SUPPORTED)) << package;
    }
    const WatchResult missing =
        launch_and_watch(LR"(C:\Avel.Tests\does-not-exist.exe)", L"", fast_options(), cancellation, {});
    EXPECT_EQ(missing.end, WatchEnd::LaunchFailed);
    EXPECT_TRUE(missing.error == ERROR_FILE_NOT_FOUND || missing.error == ERROR_PATH_NOT_FOUND)
        << missing.error;
}

TEST(InstallerHelperTest, TellsHelpersFromInstalledPrograms) {
    const std::vector<std::wstring> folders = {LR"(C:\Users\me\AppData\Local\Temp)",
                                               LR"(C:\Windows\Installer)"};
    for (const auto* helper : {
             LR"(C:\Windows\System32\msiexec.exe)",
             LR"(C:\Windows\SysWOW64\MSIEXEC.EXE)",
             LR"(C:\Users\me\AppData\Local\Temp\{0E4D5C66-1A43-4A45-8A6C-3CE8F7D0C2B4}\setup.exe)",
             LR"(C:\Windows\Installer\MSI8F2A.tmp)",
             LR"(D:\Unpacked\is-K2H7Q.tmp\setup.tmp)",
             LR"(D:\Unpacked\nsq1A2B.tmp\helper.exe)",
             LR"(D:\Unpacked\~nsuA\Au_.exe)",
             LR"(C:\ProgramData\Unpacked\{0E4D5C66}\.be\bundle.exe)",
             LR"(D:\Unpacked\.cr\bundle.exe)",
         }) {
        EXPECT_TRUE(is_installer_helper(helper, folders)) << helper;
    }
    for (const auto* program : {
             LR"(C:\Program Files\Contoso\app.exe)",
             LR"(C:\Users\me\AppData\Local\Temperature\app.exe)",  // Not inside Temp
             LR"(C:\Users\me\AppData\Local\Programs\Contoso\app.exe)",
             LR"(C:\Tools\Contoso.tmp.old\app.exe)",
             L"",
         }) {
        EXPECT_FALSE(is_installer_helper(program, folders)) << program;
    }
}

TEST(InstallerHelperTest, HelperFoldersIncludeWindowsTempAndThisUsersTemp) {
    const std::vector<std::wstring> folders = helper_folders();
    wchar_t windows[MAX_PATH] = {};
    ::GetWindowsDirectoryW(windows, MAX_PATH);
    wchar_t temp[MAX_PATH + 1] = {};
    ::GetTempPathW(MAX_PATH + 1, temp);
    for (const std::wstring expected :
         {platform::normalize_path(std::wstring(windows) + L"\\Temp"),
          platform::normalize_path(std::wstring(windows) + L"\\Installer"), platform::long_path(temp)}) {
        EXPECT_TRUE(std::any_of(folders.begin(), folders.end(), [&](const auto& folder) {
            return platform::paths_equal(folder, expected);
        })) << expected;
    }
}

// The decisions on made-up polls, with a clock that only moves when the test says so.
class WatchDeciderTest : public ::testing::Test {
protected:
    static WatchOptions options() {
        WatchOptions options;
        options.quiet = 1000ms;
        options.app_grace = 3000ms;
        options.settle_window = 500ms;
        options.settle_cap = 2000ms;
        return options;
    }

    static Clock::time_point at(std::chrono::milliseconds time) { return Clock::time_point{} + time; }

    static WatchDecider::Poll poll(std::chrono::milliseconds time, bool root_running,
                                   std::span<const WatchProcess> others = {}) {
        WatchDecider::Poll poll;
        poll.now = at(time);
        poll.root_running = root_running;
        poll.others = others;
        return poll;
    }

    static WatchDecider::Poll busy(std::chrono::milliseconds time) {
        WatchDecider::Poll result = poll(time, false);
        result.installer_busy = true;
        return result;
    }

    static WatchDecider::Poll activity(std::chrono::milliseconds time, std::size_t records) {
        WatchDecider::Poll result = poll(time, false);
        result.activity = records;
        return result;
    }

    const std::vector<WatchProcess> helper_{{20, LR"(C:\Temp\is-1.tmp\setup.tmp)", true}};
    const std::vector<WatchProcess> app_{{30, LR"(C:\Program Files\Contoso\app.exe)", false}};
};

TEST_F(WatchDeciderTest, RunsForAsLongAsTheInstallerRuns) {
    WatchDecider decider(options(), false);
    EXPECT_FALSE(decider.update(poll(0ms, true)));
    EXPECT_EQ(decider.phase(), WatchPhase::Running);
    EXPECT_FALSE(decider.update(poll(60000ms, true, app_)));
    EXPECT_EQ(decider.phase(), WatchPhase::Running);
}

TEST_F(WatchDeciderTest, WaitsForHelpersThenForTheQuietPeriod) {
    WatchDecider decider(options(), false);
    EXPECT_FALSE(decider.update(poll(0ms, true, helper_)));
    EXPECT_FALSE(decider.update(poll(100ms, false, helper_)));
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForHelpers);
    EXPECT_FALSE(decider.update(poll(9000ms, false, helper_)));  // Helpers have no grace limit
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForHelpers);
    EXPECT_FALSE(decider.update(poll(9100ms, false)));
    EXPECT_EQ(decider.phase(), WatchPhase::Settling);
    EXPECT_FALSE(decider.update(poll(10000ms, false)));
    EXPECT_TRUE(decider.update(poll(10100ms, false)));
}

TEST_F(WatchDeciderTest, ProgramsTheInstallerStartedHoldOnlyForTheGrace) {
    WatchDecider decider(options(), false);
    EXPECT_FALSE(decider.update(poll(0ms, true, app_)));
    EXPECT_FALSE(decider.update(poll(100ms, false, app_)));  // The installer exits
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForHelpers);
    EXPECT_FALSE(decider.update(poll(3000ms, false, app_)));
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForHelpers);
    EXPECT_FALSE(decider.update(poll(3100ms, false, app_)));
    EXPECT_EQ(decider.phase(), WatchPhase::Settling);
    EXPECT_FALSE(decider.holds(app_[0], at(3100ms)));
    EXPECT_TRUE(decider.update(poll(4100ms, false, app_)));  // Over, with the program still running
}

TEST_F(WatchDeciderTest, TheGraceStartsWhenAProgramAppears) {
    WatchDecider decider(options(), false);
    EXPECT_FALSE(decider.update(poll(0ms, true)));
    EXPECT_FALSE(decider.update(poll(100ms, false, helper_)));
    std::vector<WatchProcess> both = helper_;
    both.push_back(app_[0]);
    EXPECT_FALSE(decider.update(poll(2000ms, false, both)));  // The helper starts the program, then exits
    EXPECT_FALSE(decider.update(poll(4900ms, false, app_)));
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForHelpers);
    EXPECT_FALSE(decider.update(poll(5000ms, false, app_)));
    EXPECT_EQ(decider.phase(), WatchPhase::Settling);
}

TEST_F(WatchDeciderTest, WaitsWhileWindowsInstallerIsBusy) {
    WatchDecider decider(options(), false);
    EXPECT_FALSE(decider.update(poll(0ms, true)));
    EXPECT_FALSE(decider.update(busy(100ms)));
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForWindowsInstaller);
    EXPECT_FALSE(decider.update(busy(5000ms)));
    EXPECT_FALSE(decider.update(poll(5100ms, false)));
    EXPECT_EQ(decider.phase(), WatchPhase::Settling);
    EXPECT_FALSE(decider.update(busy(5500ms)));  // Busy again: the quiet period starts over
    EXPECT_EQ(decider.phase(), WatchPhase::WaitingForWindowsInstaller);
    EXPECT_FALSE(decider.update(poll(5600ms, false)));
    EXPECT_FALSE(decider.update(poll(6500ms, false)));
    EXPECT_TRUE(decider.update(poll(6600ms, false)));
}

TEST_F(WatchDeciderTest, ANewProcessRestartsTheQuietPeriod) {
    WatchDecider decider(options(), false);
    EXPECT_FALSE(decider.update(poll(0ms, false)));
    WatchDecider::Poll started = poll(800ms, false);
    started.new_process = true;  // It came and went between two polls
    EXPECT_FALSE(decider.update(started));
    EXPECT_EQ(decider.phase(), WatchPhase::Settling);
    EXPECT_FALSE(decider.update(poll(1100ms, false)));
    EXPECT_FALSE(decider.update(poll(1700ms, false)));
    EXPECT_TRUE(decider.update(poll(1800ms, false)));
}

TEST_F(WatchDeciderTest, SettlesOnceTheFilesStopChanging) {
    WatchDecider decider(options(), true);
    EXPECT_FALSE(decider.update(poll(0ms, false)));
    EXPECT_TRUE(decider.wants_activity());
    for (auto time = 100ms; time <= 1200ms; time += 100ms) {
        EXPECT_FALSE(decider.update(activity(time, 7))) << time.count();
    }
    for (auto time = 1300ms; time <= 1600ms; time += 100ms) {
        EXPECT_FALSE(decider.update(activity(time, 0))) << time.count();
    }
    EXPECT_TRUE(decider.update(activity(1700ms, 0)));  // 500 ms without records
}

TEST_F(WatchDeciderTest, SettlingHasACap) {
    WatchDecider decider(options(), true);
    EXPECT_FALSE(decider.update(poll(0ms, false)));
    for (auto time = 100ms; time < 3000ms; time += 100ms) {
        EXPECT_FALSE(decider.update(activity(time, 3))) << time.count();
    }
    EXPECT_TRUE(decider.update(activity(3000ms, 3)));  // The quiet period, then the 2 s cap
}

TEST_F(WatchDeciderTest, WaitsForTheFirstActivityReading) {
    WatchDecider decider(options(), true);
    EXPECT_FALSE(decider.update(poll(0ms, false)));
    EXPECT_FALSE(decider.update(poll(1500ms, false)));  // Quiet long enough, but nothing read yet
    EXPECT_TRUE(decider.update(activity(1600ms, 0)));   // No records since the trace began
}

TEST_F(WatchDeciderTest, AnswersInWords) {
    EXPECT_EQ(to_string(WatchPhase::WaitingForWindowsInstaller), "waiting-for-windows-installer");
    EXPECT_EQ(to_string(WatchEnd::InstallerDone), "installer-done");
    EXPECT_EQ(to_string(WatchEnd::LaunchFailed), "launch-failed");
}

}  // namespace
}  // namespace avel::tracker
