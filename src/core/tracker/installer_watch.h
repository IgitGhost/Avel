// "Install and trace": Avel starts an installer and decides by itself when the installation is
// over, so the trace can stop.
//
// The installer runs in a job object, like an uninstaller (uninstall/process_runner.h). Its
// processes are followed through the job's notifications, and on every poll also through a process
// snapshot, which finds children that broke away from the job (WiX Burn's engine does). The
// installation is over once the installer has exited, none of its helpers runs (msiexec, copies of
// itself in Temp, ...), Windows Installer is idle, nothing new has started for a quiet period and
// the file activity has died down. Programs the installer started for the user ("Run X now") are
// reported, but never waited for and never ended.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/uninstall/process_runner.h"

namespace avel::tracker {

// Windows Installer is running an installation (some process owns Global\_MSIExecute). It only
// looks: taking the mutex could make a concurrent installation fail with 1618.
bool windows_installer_busy();

struct WatchOptions {
    std::chrono::milliseconds poll{500};            // How often processes and Windows Installer are checked
    std::chrono::milliseconds quiet{5000};          // How long everything must stay idle before settling
    std::chrono::milliseconds app_grace{10000};     // How long a program that isn't a helper still counts
    std::chrono::milliseconds settle_window{2000};  // Settling ends after this long without file activity...
    std::chrono::milliseconds settle_cap{
        20000};  // ...or this long after the quiet period, whichever is first
    std::optional<std::chrono::seconds> timeout;                    // Gives up this long after the launch
    std::function<bool()> installer_busy = windows_installer_busy;  // Tests inject their own
    // The journal records outside the noise since the previous call; the trace session supplies
    // it. It is called on every poll while everything is idle. Without it the watch ends once the
    // quiet period has passed.
    std::function<std::size_t()> activity;
};

enum class WatchPhase : std::uint8_t {
    Running,                     // The installer runs
    WaitingForHelpers,           // It has exited; processes it started still run
    WaitingForWindowsInstaller,  // Those have exited too, but Windows Installer is still busy
    Settling,                    // Everything is idle; waiting for it to stay so and for the files to settle
};

enum class WatchEnd : std::uint8_t {
    InstallerDone,  // The installation is over
    Stopped,        // Cancellation::stop_batch; with terminate_current the installer's job was ended too
    TimedOut,       // WatchOptions::timeout passed
    LaunchFailed,   // The installer couldn't be started; see WatchResult::error
};

struct WatchProcess {
    std::uint32_t pid = 0;
    std::wstring image;   // Normalized full path; empty when Windows won't tell
    bool helper = false;  // Holds the trace while it runs: the installer itself or one of its helpers
};

struct WatchResult {
    WatchEnd end = WatchEnd::LaunchFailed;
    std::uint32_t error = 0;        // LaunchFailed: the Win32 error (ERROR_NOT_SUPPORTED for packages)
    std::uint32_t exit_code = 0;    // The installer's, once it has exited
    bool reboot_requested = false;  // It exited with 3010 or 1641
    // Its processes still running at the end: after InstallerDone, the programs it started.
    std::vector<WatchProcess> left_running;
};

using WatchProgress = std::function<void(WatchPhase phase, std::span<const WatchProcess> running)>;

// Starts `"installer" arguments` (a .msi through `msiexec.exe /i`) in its folder and waits until
// the installation is over. MSIX and AppX packages are refused: Windows installs and removes them
// itself. `progress`, if set, is called on the calling thread whenever the phase or the set of
// running processes changes (the installer first).
//
// Cancellation::stop_batch ends the watch at once and leaves the installer running;
// terminate_current also ends every process in the installer's job.
WatchResult launch_and_watch(const std::wstring& installer, const std::wstring& arguments,
                             const WatchOptions& options, const uninstall::Cancellation& cancellation,
                             const WatchProgress& progress);

// The folders whose programs are installers' helpers on this machine: every user's Temp folder,
// Windows' Temp and SystemTemp, Windows Installer's cache and WiX Burn's Package Cache.
std::vector<std::wstring> helper_folders();

// A process running `image` is a helper of an installer rather than a program it started:
// msiexec, a program in one of `folders` (normalized), in a folder installers unpack themselves to
// (*.tmp such as Inno Setup's is-*.tmp, NSIS's ~nsu*, Burn's .cr and .be), or a *.tmp program.
bool is_installer_helper(std::wstring_view image, std::span<const std::wstring> folders);

// The decision half of launch_and_watch. Given what each poll saw, it tells what the watch is
// waiting for and when the installation is over. It has no processes or clock of its own, so it
// can be tested with made-up polls.
class WatchDecider {
public:
    using Clock = std::chrono::steady_clock;

    struct Poll {
        Clock::time_point now;
        bool root_running = false;             // The installer itself
        std::span<const WatchProcess> others;  // Its other processes that still run
        bool new_process = false;              // A process joined the installation since the previous poll
        bool installer_busy = false;           // Windows Installer is busy
        std::optional<std::size_t> activity;   // While settling: the records since the previous poll
    };

    // `settles`: an activity source exists. Without one, the installation is over once the quiet
    // period has passed.
    WatchDecider(const WatchOptions& options, bool settles);

    // Takes in one poll; true once the installation is over.
    bool update(const Poll& poll);

    WatchPhase phase() const { return phase_; }

    // Everything is idle, so the next polls should bring the file activity.
    bool wants_activity() const { return settles_ && phase_ == WatchPhase::Settling; }

    // The process still holds the trace: a helper while it runs; another program until app_grace
    // after both the installer's exit and the poll that first saw it.
    bool holds(const WatchProcess& process, Clock::time_point now) const;

private:
    std::chrono::milliseconds quiet_;
    std::chrono::milliseconds app_grace_;
    std::chrono::milliseconds settle_window_;
    std::chrono::milliseconds settle_cap_;
    bool settles_;
    WatchPhase phase_ = WatchPhase::Running;
    std::optional<Clock::time_point> root_exit_;
    std::unordered_map<std::uint32_t, Clock::time_point> first_seen_;
    std::optional<Clock::time_point> idle_since_;     // Settling began, or a process last appeared
    std::optional<Clock::time_point> last_activity_;  // The last poll that brought file activity
};

std::string_view to_string(WatchPhase phase);
std::string_view to_string(WatchEnd end);

}  // namespace avel::tracker
