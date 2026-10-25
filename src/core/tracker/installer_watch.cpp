#include "core/tracker/installer_watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::tracker {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr wchar_t kMsiExecuteMutex[] = LR"(Global\_MSIExecute)";
constexpr DWORD kMutantQueryState = 0x0001;  // MUTANT_QUERY_STATE
constexpr std::chrono::milliseconds kCancellationCheck = 100ms;
constexpr std::chrono::milliseconds kShortestPoll = 10ms;
constexpr std::chrono::seconds kTerminationWait = 5s;

// Windows installs and removes these packages itself.
constexpr std::array<std::wstring_view, 4> kPackageExtensions = {L".msix", L".appx", L".msixbundle",
                                                                 L".appxbundle"};

// ntdll's NtQueryMutant (MutantBasicInformation); winternl.h doesn't declare it.
struct MutantBasicInformation {
    LONG current_count;  // 1 when free; 0 or less while a thread owns it
    BOOLEAN owned_by_caller;
    BOOLEAN abandoned;
};
constexpr int kMutantBasicInformation = 0;
using NtQueryMutantFunction = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

std::wstring image_of(HANDLE process) {
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = static_cast<DWORD>(path.size());
        if (::QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            path.resize(size);
            // A program started through an 8.3 path reports it that way; helper folders are long.
            return path.find(L'~') != std::wstring::npos ? platform::long_path(path)
                                                         : platform::normalize_path(path);
        }
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        path.resize(path.size() * 4);
    }
    return {};
}

std::wstring working_directory_for(const std::wstring& file) {
    const std::filesystem::path path(file);
    if (!path.is_absolute()) {
        return {};
    }
    std::error_code error;
    const auto parent = path.parent_path();
    return std::filesystem::is_directory(parent, error) ? parent.wstring() : std::wstring();
}

std::wstring_view extension_of(std::wstring_view file) {
    const std::wstring_view name = platform::last_path_component(file);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring_view::npos ? std::wstring_view() : name.substr(dot);
}

std::wstring system_folder() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
}

std::wstring windows_folder() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
}

std::uint64_t as_number(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// One of the installation's processes. The open handle pins it: its PID can't be reused while
// the watch runs, so PIDs (and parent PIDs) keep naming the same process.
struct TrackedProcess {
    std::uint32_t pid = 0;
    wil::unique_handle handle;
    bool can_wait = false;      // The handle has SYNCHRONIZE
    std::uint64_t created = 0;  // Creation time (FILETIME)
    std::wstring image;
    bool helper = false;
    bool in_job = false;
    // Windows' console host for a console program: it lives as long as that program, so it is
    // neither waited for nor reported.
    bool hidden = false;

    bool running() const {
        if (can_wait) {
            return ::WaitForSingleObject(handle.get(), 0) == WAIT_TIMEOUT;
        }
        DWORD code = 0;
        return ::GetExitCodeProcess(handle.get(), &code) && code == STILL_ACTIVE;
    }

    WatchProcess describe() const { return {pid, image, helper}; }
};

bool fill_in(TrackedProcess& process) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!::GetProcessTimes(process.handle.get(), &created, &exited, &kernel, &user)) {
        return false;
    }
    process.created = as_number(created);
    process.image = image_of(process.handle.get());
    return true;
}

std::optional<TrackedProcess> open_process(DWORD pid) {
    TrackedProcess process;
    process.pid = pid;
    process.handle.reset(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    process.can_wait = static_cast<bool>(process.handle);
    if (!process.handle) {
        process.handle.reset(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    }
    if (!process.handle || !fill_in(process)) {
        return std::nullopt;
    }
    return process;
}

// The installation's processes: the installer, what its job reports, and what any of them started
// outside the job.
class ProcessTracker {
public:
    ProcessTracker(HANDLE job, std::vector<std::wstring> helper_folders)
        : job_(job), helper_folders_(std::move(helper_folders)) {}

    void add_root(HANDLE handle, DWORD pid) {
        TrackedProcess root;
        root.pid = pid;
        root.handle.reset(handle);
        root.can_wait = true;
        fill_in(root);
        root.helper = true;  // The installer holds the trace for as long as it runs
        root.in_job = true;
        processes_.push_back(std::move(root));
    }

    const TrackedProcess& root() const { return processes_.front(); }

    // A process the job reported; false if it was known already or has exited.
    bool add_from_job(DWORD pid) {
        if (find(pid)) {
            return false;
        }
        auto process = open_process(pid);
        // If the process exited at once, its PID may already name another one.
        BOOL in_job = FALSE;
        if (!process || !::IsProcessInJob(process->handle.get(), job_, &in_job) || !in_job) {
            return false;
        }
        process->in_job = true;
        add(std::move(*process));
        return true;
    }

    // Adds the processes that tracked ones started, wherever they run; how many were added.
    // Children that broke away from the job are found only this way.
    std::size_t add_children() {
        std::vector<std::pair<DWORD, DWORD>> entries;  // PID, parent PID
        const wil::unique_hfile snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) {
            return 0;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more;
             more = ::Process32NextW(snapshot.get(), &entry)) {
            if (entry.th32ProcessID != 0) {
                entries.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID);
            }
        }
        std::size_t added = 0;
        // Repeated until nothing new turns up: a grandchild may come before its parent.
        for (bool found = true; found;) {
            found = false;
            for (const auto [pid, parent_pid] : entries) {
                const TrackedProcess* parent = find(parent_pid);
                if (!parent || find(pid)) {
                    continue;
                }
                const std::uint64_t parent_created = parent->created;
                auto process = open_process(pid);
                // A parent PID names a tracked process only for processes started after it.
                if (!process || process->created < parent_created) {
                    continue;
                }
                BOOL in_job = FALSE;
                process->in_job = ::IsProcessInJob(process->handle.get(), job_, &in_job) && in_job;
                add(std::move(*process));
                ++added;
                found = true;
            }
        }
        return added;
    }

    // The processes still running, the installer first.
    std::vector<WatchProcess> running() const {
        std::vector<WatchProcess> list;
        for (const auto& process : processes_) {
            if (!process.hidden && process.running()) {
                list.push_back(process.describe());
            }
        }
        return list;
    }

    // After TerminateJobObject: lets the job's processes finish exiting.
    void wait_for_job(std::chrono::milliseconds limit) const {
        const auto deadline = Clock::now() + limit;
        for (const auto& process : processes_) {
            if (!process.in_job || !process.can_wait) {
                continue;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            ::WaitForSingleObject(process.handle.get(), static_cast<DWORD>(std::max(left.count(), 0LL)));
        }
    }

private:
    const TrackedProcess* find(DWORD pid) const {
        const auto found = std::find_if(processes_.begin(), processes_.end(),
                                        [&](const auto& process) { return process.pid == pid; });
        return found == processes_.end() ? nullptr : &*found;
    }

    void add(TrackedProcess process) {
        process.helper = is_installer_helper(process.image, helper_folders_);
        process.hidden = platform::iequals(platform::last_path_component(process.image), L"conhost.exe");
        processes_.push_back(std::move(process));
    }

    HANDLE job_;
    std::vector<std::wstring> helper_folders_;
    std::vector<TrackedProcess> processes_;  // The installer first
};

// What launch_and_watch last told its progress callback.
struct Reported {
    std::optional<WatchPhase> phase;
    std::vector<std::uint32_t> pids;

    bool changed(WatchPhase new_phase, std::span<const WatchProcess> running) {
        std::vector<std::uint32_t> new_pids;
        for (const auto& process : running) {
            new_pids.push_back(process.pid);
        }
        if (phase == new_phase && pids == new_pids) {
            return false;
        }
        phase = new_phase;
        pids = std::move(new_pids);
        return true;
    }
};

}  // namespace

bool windows_installer_busy() {
    const wil::unique_handle mutex(::OpenMutexW(SYNCHRONIZE | kMutantQueryState, FALSE, kMsiExecuteMutex));
    if (!mutex) {
        // It exists but may not be queried: that it exists is all there is to go on.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    static const auto query = reinterpret_cast<NtQueryMutantFunction>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryMutant"));
    MutantBasicInformation information{};
    if (!query ||
        query(mutex.get(), kMutantBasicInformation, &information, sizeof(information), nullptr) < 0) {
        return true;
    }
    return information.current_count <= 0;
}

std::vector<std::wstring> helper_folders() {
    std::vector<std::wstring> folders;
    const auto add = [&folders](const std::wstring& path) {
        if (!platform::is_absolute_path(path) ||
            std::any_of(folders.begin(), folders.end(),
                        [&](const auto& folder) { return platform::iequals(folder, path); })) {
            return;
        }
        folders.push_back(path);
    };
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
    if (length > 0 && length <= MAX_PATH) {
        add(platform::long_path(temp));  // TEMP may hold 8.3 names
    }
    if (const std::wstring windows = windows_folder(); !windows.empty()) {
        for (const auto* folder : {L"Temp", L"SystemTemp", L"Installer"}) {
            add(platform::normalize_path(windows + L"\\" + folder));
        }
    }
    for (const auto& profile : platform::enumerate_user_profiles()) {
        if (!profile.profile_path.empty()) {
            add(platform::normalize_path(profile.profile_path + LR"(\AppData\Local\Temp)"));
        }
    }
    if (const std::wstring program_data = platform::expand_environment(L"%ProgramData%");
        program_data.find(L'%') == std::wstring::npos) {
        add(platform::normalize_path(program_data + LR"(\Package Cache)"));
    }
    return folders;
}

bool is_installer_helper(std::wstring_view image, std::span<const std::wstring> folders) {
    if (image.empty()) {
        return false;
    }
    const std::wstring_view name = platform::last_path_component(image);
    if (platform::iequals(name, L"msiexec.exe") || platform::iends_with(name, L".tmp")) {
        return true;
    }
    if (std::any_of(folders.begin(), folders.end(),
                    [&](const auto& folder) { return platform::normalized_path_is_under(image, folder); })) {
        return true;
    }
    // The folders it is in: installers unpack themselves to *.tmp (Inno Setup's is-*.tmp, NSIS's
    // ns*.tmp and ~nsu.tmp) and WiX Burn to .cr and .be.
    std::wstring_view folder = image.substr(0, image.size() - name.size());
    while (!folder.empty()) {
        const size_t end = folder.find(L'\\');
        const std::wstring_view part = folder.substr(0, end);
        if (platform::iends_with(part, L".tmp") || platform::istarts_with(part, L"~nsu") ||
            platform::iequals(part, L".cr") || platform::iequals(part, L".be")) {
            return true;
        }
        folder.remove_prefix(end == std::wstring_view::npos ? folder.size() : end + 1);
    }
    return false;
}

WatchDecider::WatchDecider(const WatchOptions& options, bool settles)
    : quiet_(options.quiet),
      app_grace_(options.app_grace),
      settle_window_(options.settle_window),
      settle_cap_(options.settle_cap),
      settles_(settles) {}

bool WatchDecider::holds(const WatchProcess& process, Clock::time_point now) const {
    if (process.helper || !root_exit_) {
        return true;
    }
    Clock::time_point since = *root_exit_;
    if (const auto seen = first_seen_.find(process.pid); seen != first_seen_.end()) {
        since = std::max(since, seen->second);
    }
    return now - since < app_grace_;
}

bool WatchDecider::update(const Poll& poll) {
    for (const auto& process : poll.others) {
        first_seen_.try_emplace(process.pid, poll.now);
    }
    if (!poll.root_running && !root_exit_) {
        root_exit_ = poll.now;
    }

    if (poll.root_running) {
        phase_ = WatchPhase::Running;
    } else if (std::any_of(poll.others.begin(), poll.others.end(),
                           [&](const auto& process) { return holds(process, poll.now); })) {
        phase_ = WatchPhase::WaitingForHelpers;
    } else if (poll.installer_busy) {
        phase_ = WatchPhase::WaitingForWindowsInstaller;
    } else {
        phase_ = WatchPhase::Settling;
    }
    if (phase_ != WatchPhase::Settling) {
        idle_since_.reset();
        last_activity_.reset();
        return false;
    }

    if (!idle_since_) {
        idle_since_ = poll.now;
        last_activity_.reset();
    } else if (poll.new_process) {
        idle_since_ = poll.now;  // Something started: the quiet period begins again
    }
    if (poll.activity) {
        // The first reading covers everything since the previous call, idle or not; records in it
        // restart the window, so it errs on the side of waiting.
        if (*poll.activity > 0 || !last_activity_) {
            last_activity_ = *poll.activity > 0 ? poll.now : *idle_since_;
        }
    }
    const Clock::time_point quiet_end = *idle_since_ + quiet_;
    if (poll.now < quiet_end) {
        return false;
    }
    if (!settles_) {
        return true;
    }
    if (!last_activity_) {
        return false;  // No reading yet
    }
    return poll.now - *last_activity_ >= settle_window_ || poll.now - quiet_end >= settle_cap_;
}

WatchResult launch_and_watch(const std::wstring& installer, const std::wstring& arguments,
                             const WatchOptions& options, const uninstall::Cancellation& cancellation,
                             const WatchProgress& progress) {
    WatchResult result;
    const auto fail = [&result](DWORD error) {
        result.end = WatchEnd::LaunchFailed;
        result.error = error;
        return result;
    };

    const std::wstring_view extension = extension_of(installer);
    if (std::any_of(kPackageExtensions.begin(), kPackageExtensions.end(),
                    [&](auto package) { return platform::iequals(extension, package); })) {
        return fail(ERROR_NOT_SUPPORTED);
    }
    // Quoting the program stops CreateProcess from guessing at unquoted paths with spaces.
    std::wstring command_line = platform::iequals(extension, L".msi")
                                    ? L"\"" + system_folder() + L"\\msiexec.exe\" /i \"" + installer + L"\""
                                    : L"\"" + installer + L"\"";
    if (!arguments.empty()) {
        command_line += L' ';
        command_line += arguments;
    }
    const std::wstring directory = working_directory_for(installer);

    const wil::unique_handle job(::CreateJobObjectW(nullptr, nullptr));
    const wil::unique_handle port(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!job || !port) {
        return fail(::GetLastError());
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = job.get();
    association.CompletionPort = port.get();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!::SetInformationJobObject(job.get(), JobObjectAssociateCompletionPortInformation, &association,
                                   sizeof(association)) ||
        !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return fail(::GetLastError());
    }

    // The installer's standard handles are NUL, the only handle it inherits. Without them Windows
    // gives a console installer Avel's own (a setup script's sc.exe would write into the JSON
    // `trace run --json` prints), and a program it leaves running would keep them open.
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    const wil::unique_hfile null_device(::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                                      FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                                      OPEN_EXISTING, 0, nullptr));
    if (!null_device) {
        return fail(::GetLastError());
    }
    HANDLE inherited = null_device.get();
    SIZE_T list_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<std::byte> list_buffer(list_size);
    const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
    if (!::InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
        return fail(::GetLastError());
    }
    const auto delete_list = wil::scope_exit([&] { ::DeleteProcThreadAttributeList(list); });
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited,
                                     sizeof(inherited), nullptr, nullptr)) {
        return fail(::GetLastError());
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited;
    startup.StartupInfo.hStdOutput = inherited;
    startup.StartupInfo.hStdError = inherited;
    startup.lpAttributeList = list;
    wil::unique_process_information process;
    // Suspended until it's in the job, so it can't start children outside of it.
    if (!::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                          CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                          directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process)) {
        return fail(::GetLastError());
    }
    if (!::AssignProcessToJobObject(job.get(), process.hProcess)) {
        const DWORD error = ::GetLastError();
        ::TerminateProcess(process.hProcess, ERROR_CANCELLED);
        return fail(error);
    }
    ProcessTracker tracker(job.get(), helper_folders());
    tracker.add_root(std::exchange(process.hProcess, nullptr), process.dwProcessId);
    ::ResumeThread(process.hThread);

    WatchDecider decider(options, static_cast<bool>(options.activity));
    const std::chrono::milliseconds poll_every = std::max(options.poll, kShortestPoll);
    const Clock::time_point started = Clock::now();
    Clock::time_point next_poll = started;
    bool new_process = false;
    Reported reported;
    for (;;) {
        if (cancellation.terminate_current) {
            ::TerminateJobObject(job.get(), ERROR_CANCELLED);
            tracker.wait_for_job(kTerminationWait);
            result.end = WatchEnd::Stopped;
            break;
        }
        if (cancellation.stop_batch) {
            result.end = WatchEnd::Stopped;
            break;
        }
        const Clock::time_point now = Clock::now();
        if (options.timeout && now - started >= *options.timeout) {
            result.end = WatchEnd::TimedOut;
            break;
        }
        if (now >= next_poll) {
            new_process |= tracker.add_children() > 0;
            const std::vector<WatchProcess> running = tracker.running();
            const bool root_running = !running.empty() && running.front().pid == tracker.root().pid;
            WatchDecider::Poll poll;
            poll.now = now;
            poll.root_running = root_running;
            poll.others = std::span(running).subspan(root_running ? 1 : 0);
            poll.new_process = std::exchange(new_process, false);
            poll.installer_busy = options.installer_busy && options.installer_busy();
            if (decider.wants_activity() && options.activity) {
                poll.activity = options.activity();
            }
            const bool done = decider.update(poll);
            if (progress && reported.changed(decider.phase(), running)) {
                progress(decider.phase(), running);
            }
            if (done) {
                result.end = WatchEnd::InstallerDone;
                break;
            }
            next_poll = now + poll_every;
        }

        // Job notifications until the next poll, looking at the cancellation in between.
        const auto wait =
            std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(next_poll - Clock::now()), 0ms,
                       kCancellationCheck);
        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        if (::GetQueuedCompletionStatus(port.get(), &message, &key, &overlapped,
                                        static_cast<DWORD>(wait.count())) &&
            key == reinterpret_cast<ULONG_PTR>(job.get()) && message == JOB_OBJECT_MSG_NEW_PROCESS) {
            // For process messages the "overlapped" pointer carries the PID.
            new_process |= tracker.add_from_job(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(overlapped)));
        }
    }

    result.left_running = tracker.running();
    if (!tracker.root().running()) {
        DWORD exit_code = 0;
        if (::GetExitCodeProcess(tracker.root().handle.get(), &exit_code)) {
            result.exit_code = exit_code;
            result.reboot_requested =
                exit_code == ERROR_SUCCESS_REBOOT_REQUIRED || exit_code == ERROR_SUCCESS_REBOOT_INITIATED;
        }
    }
    return result;
}

std::string_view to_string(WatchPhase phase) {
    switch (phase) {
        case WatchPhase::Running:
            return "running";
        case WatchPhase::WaitingForHelpers:
            return "waiting-for-helpers";
        case WatchPhase::WaitingForWindowsInstaller:
            return "waiting-for-windows-installer";
        case WatchPhase::Settling:
            return "settling";
    }
    return "unknown";
}

std::string_view to_string(WatchEnd end) {
    switch (end) {
        case WatchEnd::InstallerDone:
            return "installer-done";
        case WatchEnd::Stopped:
            return "stopped";
        case WatchEnd::TimedOut:
            return "timed-out";
        case WatchEnd::LaunchFailed:
            return "launch-failed";
    }
    return "unknown";
}

}  // namespace avel::tracker
