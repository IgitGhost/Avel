// Install and trace with the avel_test_spawner helper (tests/helpers/spawner.cpp) as the installer
// and the made-up system of trace_fakes.h: the trace stops once the installation is over, stays
// open when the installer asks for a restart, and is discarded when the installer can't start.
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "core/platform/paths.h"
#include "core/platform/record_folders.h"
#include "core/tracker/install_trace.h"
#include "test_spawner.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using namespace fakes;

constexpr std::uint64_t kContoso = 0x0001000000002000;

using testing::spawner_path;

class InstallTraceTest : public ::testing::Test {
protected:
    InstallTraceTest() : system_(scratch_, temp_.path()) {
        platform::FolderStatus status{};
        store_ = TraceStore::open(test_store_options(temp_ / L"Traces"), status);
        EXPECT_EQ(status, platform::FolderStatus::Ok);
    }

    InstallTraceOptions options(const std::wstring& installer, const std::wstring& arguments) {
        InstallTraceOptions result;
        result.installer = installer;
        result.arguments = arguments;
        result.description = L"Contoso";
        result.watch.poll = 50ms;
        result.watch.quiet = 300ms;
        result.watch.app_grace = 500ms;
        result.watch.settle_window = 200ms;
        result.watch.settle_cap = 1s;
        result.watch.timeout = 30s;
        result.watch.installer_busy = [] {
            return false;
        };  // Windows Installer's real state could be anything
        result.drain_every = 50ms;
        result.sources = system_.sources();
        return result;
    }

    InstallTraceResult run(const InstallTraceOptions& options) {
        uninstall::Cancellation cancellation;
        return install_and_trace(*store_, options, cancellation, [this](const InstallTraceStatus& status) {
            phases_.push_back(status.phase);
            if (status.phase == InstallTracePhase::Watching && !installed_) {
                // What the installer writes while it runs.
                installed_ = true;
                next_clock_tick();
                scratch_.set_string(LR"(Machine\Contoso)", L"Version", L"1.0");
                system_.create(kContoso, kProgramFiles, L"Contoso", kFolder);
            }
        });
    }

    bool saw(InstallTracePhase phase) const {
        return std::find(phases_.begin(), phases_.end(), phase) != phases_.end();
    }

    TempFolder temp_;
    ScratchKey scratch_;
    FakeSystem system_;
    std::unique_ptr<TraceStore> store_;
    std::vector<InstallTracePhase> phases_;
    bool installed_ = false;
};

TEST_F(InstallTraceTest, StopsTheTraceOnceTheInstallationIsOver) {
    const InstallTraceResult result = run(options(spawner_path(), L"sleep 200"));

    EXPECT_EQ(result.error, TraceError::None) << to_string(result.error);
    ASSERT_TRUE(result.watch);
    EXPECT_EQ(result.watch->end, WatchEnd::InstallerDone);
    ASSERT_TRUE(result.trace);
    EXPECT_EQ(result.trace->state, TraceState::Complete);
    EXPECT_EQ(result.trace->mode, TraceMode::Launch);
    EXPECT_EQ(result.trace->stop_reason, StopReason::InstallerDone);
    EXPECT_EQ(result.trace->command, spawner_path() + L" sleep 200");
    EXPECT_EQ(result.trace->description, L"Contoso");
    const std::vector<std::string> expected = {
        R"(created folder C:\Program Files\Contoso)",
        R"(created registry-key HKLM\SOFTWARE\Contoso [1])",
    };
    EXPECT_EQ(describe(result.result.changes), expected);
    EXPECT_TRUE(saw(InstallTracePhase::Starting));
    EXPECT_TRUE(saw(InstallTracePhase::Watching));
    EXPECT_TRUE(saw(InstallTracePhase::Finishing));

    // The watch's summary is part of the trace.
    const auto stored = store_->read(result.trace->id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->state, TraceState::Complete);
    ASSERT_TRUE(stored->watch);
    EXPECT_EQ(stored->watch->end, WatchEnd::InstallerDone);
    EXPECT_EQ(stored->watch->exit_code, 0u);
    EXPECT_FALSE(stored->watch->reboot_requested);
    EXPECT_FALSE(store_->held(result.trace->id));
}

TEST_F(InstallTraceTest, ARestartRequestLeavesTheTraceOpen) {
    const InstallTraceResult result = run(options(spawner_path(), L"sleep 0 3010"));

    EXPECT_EQ(result.error, TraceError::None) << to_string(result.error);
    ASSERT_TRUE(result.watch);
    EXPECT_EQ(result.watch->exit_code, 3010u);
    EXPECT_TRUE(result.watch->reboot_requested);
    ASSERT_TRUE(result.trace);
    EXPECT_EQ(result.trace->state, TraceState::Open);
    EXPECT_EQ(result.trace->stop_reason, StopReason::RebootPending);
    EXPECT_TRUE(result.result.changes.empty());
    EXPECT_FALSE(saw(InstallTracePhase::Finishing));

    const auto open = store_->open_trace();
    ASSERT_TRUE(open);
    EXPECT_EQ(open->id, result.trace->id);
    EXPECT_EQ(open->stop_reason, StopReason::RebootPending);
    ASSERT_TRUE(open->watch);
    EXPECT_TRUE(open->watch->reboot_requested);

    // After the restart: `trace stop`.
    TraceError error{};
    auto session = TraceSession::resume(*store_, open->id, error, system_.sources());
    ASSERT_TRUE(session) << to_string(error);
    const TraceResult stopped = session->stop(StopReason::Manual);
    EXPECT_EQ(session->info().state, TraceState::Complete);
    EXPECT_EQ(session->info().stop_reason, StopReason::Manual);
    EXPECT_EQ(describe(stopped.changes).size(), 2u);
}

TEST_F(InstallTraceTest, AnInstallerThatCantStartLeavesNoTrace) {
    const InstallTraceResult result = run(options(LR"(C:\Avel.Tests\does-not-exist.exe)", L""));

    EXPECT_EQ(result.error, TraceError::LaunchFailed);
    ASSERT_TRUE(result.watch);
    EXPECT_EQ(result.watch->end, WatchEnd::LaunchFailed);
    EXPECT_TRUE(result.watch->error == ERROR_FILE_NOT_FOUND || result.watch->error == ERROR_PATH_NOT_FOUND)
        << result.watch->error;
    EXPECT_FALSE(result.trace);
    EXPECT_TRUE(store_->list().empty());
    EXPECT_TRUE(platform::record_folders(store_->options().root).empty());
}

TEST_F(InstallTraceTest, StoppingBeforeTheLaunchLaunchesNothing) {
    uninstall::Cancellation cancellation;
    cancellation.stop_batch = true;
    const InstallTraceResult result =
        install_and_trace(*store_, options(spawner_path(), L"sleep 0"), cancellation);

    EXPECT_EQ(result.error, TraceError::Cancelled);
    EXPECT_FALSE(result.watch);
    EXPECT_FALSE(result.trace);
    EXPECT_TRUE(store_->list().empty());
}

}  // namespace
}  // namespace avel::tracker
