#include "cli/trace_command.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <system_error>

#include "cli/console.h"
#include "cli/program_lookup.h"
#include "cli/trace_output.h"
#include "core/changes/serialization.h"
#include "core/inventory/inventory.h"
#include "core/platform/elevation.h"
#include "core/platform/paths.h"
#include "core/platform/secure_folder.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/install_trace.h"
#include "core/tracker/linking.h"
#include "core/tracker/trace_export.h"
#include "core/tracker/trace_items.h"
#include "core/tracker/trace_session.h"
#include "core/tracker/trace_store.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using platform::to_utf8;
using tracker::TraceError;
using tracker::TraceInfo;
using tracker::TraceState;
using tracker::TraceStore;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli trace start  [--description <text>] [--json]\n"
    "       avel-cli trace stop   [--json]\n"
    "       avel-cli trace run    [--description <text>] [--timeout <seconds>] [--json] -- <installer> "
    "[<args>...]\n"
    "       avel-cli trace list   [--json]\n"
    "       avel-cli trace show   <id> [--all] [--json]\n"
    "       avel-cli trace link   <id> <program> [--json]\n"
    "       avel-cli trace export <id> <file>\n"
    "       avel-cli trace import <file> [--json]\n"
    "       avel-cli trace cancel\n"
    "       avel-cli trace delete <id>\n"
    "  start                 open a trace; install the program, then run `avel-cli trace stop`\n"
    "  stop                  finish the open trace, link it to the program it installed, and show\n"
    "                        what changed\n"
    "  run                   launch the installer, wait until the installation is over, then stop\n"
    "                        the trace; Ctrl+C stops the trace at once and leaves the installer\n"
    "                        running, a second Ctrl+C also ends the installer\n"
    "  list                  every trace, newest first\n"
    "  show <id>             what a trace recorded, and what of it is tied to its program\n"
    "  link <id> <program>   link a finished trace to an installed program (its id or exact name,\n"
    "                        as `avel-cli list --all` shows them); it becomes the trace's program\n"
    "  export <id> <file>    write a finished trace to one file, to import on another machine\n"
    "  import <file>         add a trace that `trace export` wrote, and link it again\n"
    "  cancel                discard the open trace\n"
    "  delete <id>           delete a finished trace\n"
    "  --description <text>  what is being installed\n"
    "  --timeout <seconds>   stop waiting for the installer this long after launching it\n"
    "  --all                 also show the changes set aside as noise\n"
    "  --json                machine-readable output\n"
    "Only one trace can be open at a time. A trace is kept while the program it's linked to is\n"
    "installed, then 30 days after; an unlinked one, 30 days. Run as administrator.\n";

enum class Command { Start, Stop, Run, List, Show, Link, Export, Import, Cancel, Delete };

struct Options {
    Command command = Command::List;
    std::wstring description;
    std::optional<std::chrono::seconds> timeout;
    bool json = false;
    bool all = false;
    std::wstring id;                   // show, link, export, delete
    std::wstring program;              // link: its id or exact name
    std::wstring file;                 // export, import
    std::wstring installer;            // run: the first argument after `--`
    std::wstring installer_arguments;  // run: the others, quoted again
};

// Ctrl+C during `trace run`: the first stops the trace at once and leaves the installer running, a
// second one also ends the installer.
uninstall::Cancellation g_cancellation;

BOOL WINAPI on_console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    if (g_cancellation.stop_batch.exchange(true)) {
        g_cancellation.terminate_current = true;
        print_error("\nEnding the installer...\n");
    } else {
        print_error("\nStopping the trace; the installer keeps running (Ctrl+C again to end it)...\n");
    }
    return TRUE;
}

std::optional<Command> parse_command(std::wstring_view name) {
    if (name == L"start") {
        return Command::Start;
    }
    if (name == L"stop") {
        return Command::Stop;
    }
    if (name == L"run") {
        return Command::Run;
    }
    if (name == L"list") {
        return Command::List;
    }
    if (name == L"show") {
        return Command::Show;
    }
    if (name == L"link") {
        return Command::Link;
    }
    if (name == L"export") {
        return Command::Export;
    }
    if (name == L"import") {
        return Command::Import;
    }
    if (name == L"cancel") {
        return Command::Cancel;
    }
    if (name == L"delete") {
        return Command::Delete;
    }
    return std::nullopt;
}

// Quotes an argument the way CommandLineToArgvW reads it back, so the installer gets the arguments
// avel-cli got.
std::wstring quote_argument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes before a quote are doubled and the quote escaped; others stay as they are.
        quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        quoted += c;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');  // Before the closing quote
    quoted += L'"';
    return quoted;
}

// A positive whole number of seconds (at most 9 digits), or nullopt.
std::optional<std::chrono::seconds> parse_seconds(std::wstring_view text) {
    if (text.empty() || text.size() > 9) {
        return std::nullopt;
    }
    std::int64_t seconds = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        seconds = seconds * 10 + (c - L'0');
    }
    return seconds > 0 ? std::optional(std::chrono::seconds(seconds)) : std::nullopt;
}

// The options after the subcommand (arguments[0]); false, with the message and the usage printed,
// on a usage error.
bool parse_options(const std::vector<std::wstring_view>& arguments, Options& options) {
    const Command command = options.command;
    const bool takes_description = command == Command::Start || command == Command::Run;
    const bool takes_json =
        command != Command::Cancel && command != Command::Delete && command != Command::Export;
    // The arguments that aren't options, in order: the trace's id, then the program or the file.
    std::vector<std::wstring*> wanted;
    if (command == Command::Show || command == Command::Delete || command == Command::Link ||
        command == Command::Export) {
        wanted.push_back(&options.id);
    }
    if (command == Command::Link) {
        wanted.push_back(&options.program);
    }
    if (command == Command::Export || command == Command::Import) {
        wanted.push_back(&options.file);
    }
    std::size_t given = 0;
    const auto fail = [&](const std::string& message) {
        print_error(std::format("trace {}: {}\n{}", to_utf8(arguments[0]), message, kUsage));
        return false;
    };
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        const bool has_value = i + 1 < arguments.size();
        if (command == Command::Run && argument == L"--") {
            // Everything after `--` is the installer's command line, whatever it looks like.
            if (has_value) {
                options.installer = arguments[i + 1];
            }
            for (std::size_t j = i + 2; j < arguments.size(); ++j) {
                options.installer_arguments += (j == i + 2 ? L"" : L" ") + quote_argument(arguments[j]);
            }
            break;
        } else if (takes_description && argument == L"--description") {
            if (!has_value) {
                return fail("--description needs a text");
            }
            options.description = arguments[++i];
        } else if (command == Command::Run && argument == L"--timeout") {
            const auto seconds = has_value ? parse_seconds(arguments[++i]) : std::nullopt;
            if (!seconds) {
                return fail("--timeout needs a whole number of seconds");
            }
            options.timeout = seconds;
        } else if (takes_json && argument == L"--json") {
            options.json = true;
        } else if (command == Command::Show && argument == L"--all") {
            options.all = true;
        } else if (argument.starts_with(L"--")) {
            return fail("unknown option " + to_utf8(argument));
        } else if (given < wanted.size() && !argument.empty()) {
            *wanted[given++] = argument;
        } else {
            return fail("unexpected argument " + to_utf8(argument));
        }
    }
    if (command == Command::Run && options.installer.empty()) {
        return fail("the installer goes after `--`");
    }
    if (given < wanted.size()) {
        const std::wstring* missing = wanted[given];
        if (missing == &options.id) {
            return fail("which trace? (its id from `avel-cli trace list`)");
        }
        if (missing == &options.program) {
            return fail("which program? (its id or exact name from `avel-cli list --all`)");
        }
        return fail(command == Command::Export ? "where to? (the file to write)"
                                               : "which file? (one that `avel-cli trace export` wrote)");
    }
    return true;
}

bool is_finished(TraceState state) {
    return state == TraceState::Complete || state == TraceState::Incomplete;
}

std::string error_text(TraceError error) {
    switch (error) {
        case TraceError::InUse:
            return "another process is using it";
        case TraceError::NotFound:
            return "it doesn't exist";
        case TraceError::Damaged:
            return "its files can't be read";
        case TraceError::WriteFailed:
            return "a file of the trace couldn't be written or deleted";
        case TraceError::Busy:
            return "another Avel process holds the trace store; try again";
        case TraceError::NotFinished:
            return "it isn't finished yet (`avel-cli trace stop` finishes it)";
        default:
            return std::string(tracker::to_string(error));
    }
}

// What is installed, for linking and retention. Packaged apps are left out: Avel doesn't trace
// their installation.
inventory::Inventory installed_programs() {
    inventory::ScanOptions scan;
    scan.appx = false;
    return inventory::scan_inventory(scan);
}

// Retention (docs/trace.md): traces whose programs are gone are deleted 30 days later. An
// inventory that found nothing says nothing about what was uninstalled, so it's skipped then.
void apply_retention(TraceStore& store, const inventory::Inventory& inventory) {
    if (!inventory.programs.empty()) {
        store.purge([&inventory](TraceInfo& info) { return tracker::refresh_links(info, inventory); });
    }
}

// Which of a finished trace's changes are tied to its program; nullopt when the program isn't
// installed (anymore), so nothing can be tied to it.
std::optional<std::vector<tracker::TiedItem>> tie_items(const TraceInfo& info,
                                                        const tracker::TraceResult& result,
                                                        const inventory::Inventory& inventory) {
    const auto resolved = tracker::resolve_trace_program(info, inventory);
    if (!resolved) {
        return std::nullopt;
    }
    return tracker::tie_trace_items(result, tracker::program_places(*resolved->program));
}

std::string_view step_text(tracker::SessionStep step) {
    switch (step) {
        case tracker::SessionStep::Volumes:
            return "Recording the journals' positions...";
        case tracker::SessionStep::KnownValues:
            return "Saving the known values...";
        case tracker::SessionStep::Snapshot:
            return "Taking the registry snapshot...";
        case tracker::SessionStep::Draining:
            return "Reading the journals...";
        case tracker::SessionStep::Files:
            return "Finding the changed files...";
        case tracker::SessionStep::Registry:
            return "Comparing the registry...";
        case tracker::SessionStep::Saving:
            return "Saving the result...";
    }
    return "...";
}

// Progress lines on stderr as a trace starts or stops; none with --json.
tracker::SessionProgress step_printer(bool json) {
    if (json) {
        return {};
    }
    return [](tracker::SessionStep step) { print_error(std::string(step_text(step)) + "\n"); };
}

// "setup.exe (1234), msiexec.exe (5678)", the first few.
std::string process_list(std::span<const tracker::WatchProcess> processes) {
    constexpr std::size_t kShown = 5;
    std::string text;
    for (std::size_t i = 0; i < processes.size() && i < kShown; ++i) {
        const auto& process = processes[i];
        const std::wstring_view name =
            process.image.empty() ? std::wstring_view(L"?") : platform::last_path_component(process.image);
        text += std::format("{}{} ({})", i == 0 ? "" : ", ", to_utf8(name), process.pid);
    }
    if (processes.size() > kShown) {
        text += std::format(" and {} more", processes.size() - kShown);
    }
    return text;
}

std::string watch_text(tracker::WatchPhase phase, std::span<const tracker::WatchProcess> running) {
    switch (phase) {
        case tracker::WatchPhase::Running:
            return running.empty() ? "Waiting for the installer..."
                                   : "Waiting for the installer: " + process_list(running);
        case tracker::WatchPhase::WaitingForHelpers:
            return "Waiting for the installer's helpers: " + process_list(running);
        case tracker::WatchPhase::WaitingForWindowsInstaller:
            return "Waiting for Windows Installer...";
        case tracker::WatchPhase::Settling:
            return "Settling...";
    }
    return "...";
}

void report_start_error(const TraceStore& store, std::string_view verb, TraceError error) {
    if (error != TraceError::AlreadyOpen) {
        print_error(std::format("trace {}: the trace couldn't start: {}\n", verb, error_text(error)));
        return;
    }
    std::string text = std::format("trace {}: another trace is already open", verb);
    if (const auto open = store.open_trace()) {
        text += std::format(" ({}, {})", to_utf8(open->id), tracker::to_string(open->state));
    }
    print_error(text + "; finish it with `avel-cli trace stop` or discard it with `avel-cli trace cancel`\n");
}

void report_resume_error(std::string_view verb, const TraceInfo& trace, TraceError error) {
    const std::string id = to_utf8(trace.id);
    switch (error) {
        case TraceError::InUse:
            print_error(std::format(
                "trace {}: trace {} is in use by another avel-cli process (`trace start` or `trace run`); "
                "wait until it has finished\n",
                verb, id));
            break;
        case TraceError::NotOpen:
            print_error(std::format(
                "trace {}: trace {} died while starting; discard it with `avel-cli trace cancel`\n", verb,
                id));
            break;
        case TraceError::Damaged:
            print_error(std::format(
                "trace {}: trace {} can't be read; discard it with `avel-cli trace cancel`\n", verb, id));
            break;
        default:
            print_error(std::format("trace {}: trace {} can't be opened: {}\n", verb, id, error_text(error)));
            break;
    }
}

// What `stop`, `run` and `show` print for a finished trace.
void print_result(const TraceInfo& info, const tracker::TraceResult& result, bool json, bool all,
                  const std::vector<tracker::TiedItem>* tied, const tracker::TraceReview* review = nullptr) {
    if (json) {
        print(trace_result_to_json(info, result, all, tied, review).dump(2) + "\n");
    } else {
        print_trace_header(info);
        print_trace_changes(info, result, all, tied, review);
    }
    warn_if_incomplete(info);
}

// A trace that just stopped, linked to what it installed (the inventory is taken now), with
// retention applied; prints the result as `stop` and `run` do.
void finish_stopped(TraceStore& store, TraceInfo info, const tracker::TraceResult& result, bool json) {
    if (!json) {
        print_error("Linking the trace to the installed program...\n");
    }
    const auto inventory = installed_programs();
    TraceError error = TraceError::None;
    if (auto linked = tracker::link_trace(store, info.id, result, inventory, error)) {
        info = std::move(*linked);
    } else {
        print_error(
            std::format("warning: trace {} couldn't be linked: {}; `avel-cli trace link` links it by hand\n",
                        to_utf8(info.id), error_text(error)));
    }
    apply_retention(store, inventory);
    const auto tied = tie_items(info, result, inventory);
    print_result(info, result, json, false, tied ? &*tied : nullptr);
}

int start_trace(TraceStore& store, const Options& options) {
    tracker::StartOptions start;
    start.description = options.description;
    start.mode = tracker::TraceMode::Manual;
    TraceError error = TraceError::None;
    // The session holds the trace's lock file until it's destroyed on return; the trace stays open.
    const auto session = tracker::TraceSession::start(store, start, error, step_printer(options.json));
    if (!session) {
        report_start_error(store, "start", error);
        return 1;
    }
    const TraceInfo& info = session->info();
    if (options.json) {
        print(trace_root_to_json(info).dump(2) + "\n");
    } else {
        print_trace_header(info);
        print(
            std::format("\nTrace {} is open. Install the program, then run `avel-cli trace stop`\n"
                        "(or `avel-cli trace cancel` to discard the trace).\n",
                        to_utf8(info.id)));
    }
    return 0;
}

int stop_trace(TraceStore& store, const Options& options) {
    const auto open = store.open_trace();
    if (!open) {
        print_error("trace stop: no trace is open\n");
        return 1;
    }
    TraceInfo info;
    tracker::TraceResult result;
    {
        TraceError error = TraceError::None;
        const auto session = tracker::TraceSession::resume(store, open->id, error);
        if (!session) {
            report_resume_error("stop", *open, error);
            return 1;
        }
        result = session->stop(tracker::StopReason::Manual, step_printer(options.json));
        info = session->info();
    }  // The session lets go of the trace, so it can be linked
    if (!is_finished(info.state)) {
        // Still `stopping`: everything it needs is on disk, so stopping again finishes it.
        print_error(std::format(
            "trace stop: the result of trace {} couldn't be saved; run `avel-cli trace stop` again\n",
            to_utf8(info.id)));
        return 1;
    }
    finish_stopped(store, std::move(info), result, options.json);
    return 0;
}

std::string launch_error_text(const std::wstring& installer, std::uint32_t error) {
    std::string text = "trace run: " + to_utf8(installer) + " couldn't be started";
    switch (error) {
        case ERROR_NOT_SUPPORTED:
            text += ": MSIX and AppX packages are installed by Windows itself, and Avel doesn't trace them";
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            text += ": it doesn't exist";
            break;
        case ERROR_ACCESS_DENIED:
            text += ": access denied";
            break;
        case ERROR_BAD_EXE_FORMAT:
            text += ": it isn't a program Windows can run";
            break;
        default:
            text += std::format(" (error {})", error);
            break;
    }
    return text + "; the trace was discarded\n";
}

int run_installer(TraceStore& store, const Options& options) {
    std::wstring installer = options.installer;
    if (::GetFileAttributesW(installer.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Its full path, so it runs in its own folder and the trace's command says where it was.
        std::error_code failed;
        if (auto full = std::filesystem::absolute(installer, failed); !failed) {
            installer = full.wstring();
        }
    } else if (installer.find_first_of(L"\\/:") != std::wstring::npos) {
        // Checked before the trace takes its snapshot; a bare name is left to CreateProcess's search.
        print_error("trace run: no such installer: " + to_utf8(installer) + "\n");
        return 1;
    }

    tracker::InstallTraceOptions install;
    install.installer = installer;
    install.arguments = options.installer_arguments;
    install.description = options.description;
    install.watch.timeout = options.timeout;

    // One line on stderr whenever what it's doing or waiting for changes.
    std::string last_line;
    tracker::InstallTraceProgress progress;
    if (!options.json) {
        progress = [&last_line](const tracker::InstallTraceStatus& status) {
            std::string line = status.phase == tracker::InstallTracePhase::Watching
                                   ? watch_text(status.watch, status.running)
                                   : std::string(step_text(status.step));
            if (line != last_line) {
                print_error(line + "\n");
                last_line = std::move(line);
            }
        };
    }

    ::SetConsoleCtrlHandler(on_console_control, TRUE);
    const auto result = tracker::install_and_trace(store, install, g_cancellation, progress);
    ::SetConsoleCtrlHandler(on_console_control, FALSE);

    if (!result.trace) {
        switch (result.error) {
            case TraceError::Cancelled:
                print_error(
                    "trace run: cancelled before the installer was launched; the trace was discarded\n");
                break;
            case TraceError::LaunchFailed:
                print_error(launch_error_text(installer, result.watch ? result.watch->error : 0));
                break;
            default:
                report_start_error(store, "run", result.error);
                break;
        }
        return 1;
    }
    const TraceInfo& info = *result.trace;
    if (info.stop_reason == tracker::StopReason::RebootPending) {
        // What the installer still does at the restart belongs to the trace, so it stays open.
        if (result.error != TraceError::None) {
            print_error("warning: the trace's state couldn't be saved; it stays open all the same\n");
        }
        constexpr std::string_view kRestart = "Restart Windows, then run `avel-cli trace stop`.\n";
        if (options.json) {
            print(trace_root_to_json(info).dump(2) + "\n");
            print_error(kRestart);
        } else {
            print_trace_header(info);
            print(std::string("\nThe installer asked for a restart. ") + std::string(kRestart));
        }
        return 0;
    }
    if (!is_finished(info.state)) {
        print_error(std::format(
            "trace run: the result of trace {} couldn't be saved; run `avel-cli trace stop` to try again\n",
            to_utf8(info.id)));
        return 1;
    }
    finish_stopped(store, info, result.result, options.json);
    return 0;
}

int list_traces(TraceStore& store, const Options& options) {
    apply_retention(store, installed_programs());
    const auto traces = store.list();
    if (options.json) {
        Json root = Json::array();
        for (const auto& info : traces) {
            root.push_back(trace_to_json(info));
        }
        print(root.dump(2) + "\n");
    } else {
        print_trace_list(traces);
    }
    return 0;
}

int show_trace(TraceStore& store, const Options& options) {
    const auto inventory = installed_programs();
    apply_retention(store, inventory);
    const auto info = store.read(options.id);
    if (!info) {
        print_error("trace show: no such trace: " + to_utf8(options.id) + "\n");
        return 1;
    }
    if (!is_finished(info->state)) {
        // Nothing is known about its changes before it's stopped: only the trace itself is shown.
        if (options.json) {
            print(trace_root_to_json(*info).dump(2) + "\n");
        } else {
            print_trace_header(*info);
            print(std::format(
                "\nThe trace is {}; what it recorded is known once it's stopped (`avel-cli trace stop`).\n",
                tracker::to_string(info->state)));
        }
        return 0;
    }
    const auto result = store.read_result(options.id);
    if (!result) {
        print_error("trace show: the result of trace " + to_utf8(options.id) + " can't be read\n");
        return 1;
    }
    // What the GUI's review excluded is marked.
    const auto review = store.read_review(options.id);
    if (!review) {
        print_error(
            std::format("warning: the review of trace {} can't be read, so uninstalling and forcing "
                        "don't use the trace\n",
                        to_utf8(options.id)));
    }
    const auto tied = tie_items(*info, *result, inventory);
    print_result(*info, *result, options.json, options.all, tied ? &*tied : nullptr,
                 review ? &*review : nullptr);
    return 0;
}

int link_by_hand(TraceStore& store, const Options& options) {
    const std::string id = to_utf8(options.id);
    const auto inventory = installed_programs();
    std::vector<const inventory::Program*> found;
    if (!resolve(inventory, {options.program}, found)) {
        return 1;
    }
    const inventory::Program& program = *found.front();
    TraceError error = TraceError::None;
    const auto info = store.update(
        options.id,
        [&](TraceInfo& trace) {
            tracker::add_manual_link(trace, program);
            return true;
        },
        error);
    if (!info) {
        print_error(std::format("trace link: trace {} can't be linked: {}\n", id, error_text(error)));
        return 1;
    }
    apply_retention(store, inventory);
    if (options.json) {
        print(trace_root_to_json(*info).dump(2) + "\n");
    } else {
        print(std::format("Trace {} linked to {}.\n", id, name_of(program)));
    }
    return 0;
}

int export_to_file(const TraceStore& store, const Options& options) {
    const std::string id = to_utf8(options.id);
    TraceError error = TraceError::None;
    const auto trace = tracker::read_export(store, options.id, platform::enumerate_user_profiles(), error);
    if (!trace) {
        switch (error) {
            case TraceError::NotFound:
                print_error("trace export: no such trace: " + id + "\n");
                break;
            case TraceError::NotFinished:
                print_error(std::format(
                    "trace export: trace {} isn't finished; only finished traces can be exported\n", id));
                break;
            default:
                print_error(std::format("trace export: the files of trace {} can't be read\n", id));
                break;
        }
        return 1;
    }
    std::wstring file = options.file;
    std::error_code failed;
    if (auto full = std::filesystem::absolute(file, failed); !failed) {
        file = full.wstring();
    }
    const std::string text = tracker::to_json(*trace, std::chrono::system_clock::now()).dump(1, '\t');
    if (!changes::write_file_durably(file, text, true)) {
        print_error(std::format("trace export: {} couldn't be written\n", to_utf8(file)));
        return 1;
    }
    if (const auto count = tracker::environment_values(*trace); count != 0) {
        print_error(std::format(
            "warning: {} includes {} environment value{} as they were on this machine (PATH and the other\n"
            "variables the installer changed, before and after); share it only where those may be seen\n",
            to_utf8(file), count, count == 1 ? "" : "s"));
    }
    print(std::format("Trace {} exported to {}.\n", id, to_utf8(file)));
    return 0;
}

int import_from_file(TraceStore& store, const Options& options) {
    const std::string file = to_utf8(options.file);
    const auto text = changes::read_file(options.file);
    if (!text) {
        print_error(std::format("trace import: {} can't be read; nothing was imported\n", file));
        return 1;
    }
    tracker::TraceExport trace;
    int version = 0;
    switch (tracker::parse_export(*text, trace, version)) {
        case tracker::ImportError::None:
            break;
        case tracker::ImportError::OtherFormat:
            print_error(
                std::format("trace import: {} isn't an Avel trace (`avel-cli trace export` writes them); "
                            "nothing was imported\n",
                            file));
            return 1;
        case tracker::ImportError::OtherVersion:
            print_error(
                std::format("trace import: {} is an Avel trace of format version {}, which Avel {} can't "
                            "read (it reads version {}); nothing was imported\n",
                            file, version, kVersionString, tracker::kExportSchemaVersion));
            return 1;
        case tracker::ImportError::NotFinished:
            print_error(std::format(
                "trace import: the trace in {} was never finished; nothing was imported\n", file));
            return 1;
        case tracker::ImportError::Damaged:
            print_error(
                std::format("trace import: {} is damaged or incomplete; nothing was imported\n", file));
            return 1;
    }
    const auto inventory = installed_programs();
    TraceError error = TraceError::None;
    const auto info =
        tracker::import_trace(store, std::move(trace), inventory, tracker::live_import_options(), error);
    if (!info) {
        print_error(std::format("trace import: the trace couldn't be added: {}\n", error_text(error)));
        return 1;
    }
    apply_retention(store, inventory);
    for (const auto& diagnostic : info->diagnostics) {
        if (diagnostic.code == tracker::TraceDiagnosticCode::UserNotRemapped) {
            print_error(std::format(
                "note: user {}'s items are left as they were recorded: only a trace of a single user whose\n"
                "profile folder is known is moved to the importing user\n",
                to_utf8(diagnostic.subject)));
        }
    }
    if (options.json) {
        print(trace_root_to_json(*info).dump(2) + "\n");
    } else {
        print_trace_header(*info);
        print(std::format("\nTrace {} imported as {}.\n", to_utf8(info->imported->origin_id),
                          to_utf8(info->id)));
    }
    return 0;
}

int cancel_trace(TraceStore& store) {
    const auto open = store.open_trace();
    if (!open) {
        print_error("trace cancel: no trace is open\n");
        return 1;
    }
    const std::string id = to_utf8(open->id);
    TraceError error = TraceError::None;
    if (const auto session = tracker::TraceSession::resume(store, open->id, error)) {
        if (!session->cancel()) {
            print_error(std::format("trace cancel: trace {}'s folder couldn't be deleted completely\n", id));
            return 1;
        }
    } else if (error == TraceError::NotOpen || error == TraceError::Damaged) {
        // It died while starting (or can't be read): there's nothing to stop, only files to delete.
        // Unless another process finished it in the meantime.
        if (const auto now = store.read(open->id); now && is_finished(now->state)) {
            print_error(
                std::format("trace cancel: trace {} was stopped in the meantime; `avel-cli trace delete {}` "
                            "deletes it\n",
                            id, id));
            return 1;
        }
        if (!store.remove(open->id, error)) {
            print_error(
                std::format("trace cancel: trace {} couldn't be deleted: {}\n", id, error_text(error)));
            return 1;
        }
    } else {
        report_resume_error("cancel", *open, error);
        return 1;
    }
    print(std::format("Trace {} discarded.\n", id));
    return 0;
}

int delete_trace(TraceStore& store, const Options& options) {
    const std::string id = to_utf8(options.id);
    const auto info = store.read(options.id);
    if (!info) {
        print_error("trace delete: no such trace: " + id + "\n");
        return 1;
    }
    if (!is_finished(info->state)) {
        print_error(std::format(
            "trace delete: trace {} is {}; finish it with `avel-cli trace stop` or discard it with "
            "`avel-cli trace cancel`\n",
            id, tracker::to_string(info->state)));
        return 1;
    }
    TraceError error = TraceError::None;
    if (!store.remove(options.id, error)) {
        print_error(std::format("trace delete: trace {} couldn't be deleted: {}\n", id, error_text(error)));
        return 1;
    }
    print(std::format("Trace {} deleted.\n", id));
    return 0;
}

}  // namespace

int run_trace_command(const std::vector<std::wstring_view>& arguments) {
    if (arguments.empty()) {
        print_error(kUsage);
        return 2;
    }
    const auto name = arguments.front();
    if (name == L"help" || name == L"--help" || name == L"-h" || name == L"/?") {
        print(kUsage);
        return 0;
    }
    const auto command = parse_command(name);
    if (!command) {
        print_error("trace: unknown command " + to_utf8(name) + "\n" + std::string(kUsage));
        return 2;
    }
    Options options;
    options.command = *command;
    if (!parse_options(arguments, options)) {
        return 2;
    }

    // The journals, the whole registry and the trace store (SYSTEM and Administrators only).
    if (!platform::is_process_elevated()) {
        print_error("trace: Avel Trace needs administrator rights; run avel-cli as administrator.\n");
        return 1;
    }
    platform::FolderStatus status{};
    const auto store = TraceStore::open(tracker::default_trace_store_options(), status);
    if (!store) {
        print_error(
            std::format("trace: the trace store can't be trusted ({})\n", platform::to_string(status)));
        return 1;
    }
    // Deletes what traces that died while starting left, and damaged trace folders, and finishes
    // tidying finished traces. Retention needs the inventory: the commands that take one (stop,
    // run, list, show, link, import) apply it themselves.
    store->purge();

    switch (options.command) {
        case Command::Start:
            return start_trace(*store, options);
        case Command::Stop:
            return stop_trace(*store, options);
        case Command::Run:
            return run_installer(*store, options);
        case Command::List:
            return list_traces(*store, options);
        case Command::Show:
            return show_trace(*store, options);
        case Command::Link:
            return link_by_hand(*store, options);
        case Command::Export:
            return export_to_file(*store, options);
        case Command::Import:
            return import_from_file(*store, options);
        case Command::Cancel:
            return cancel_trace(*store);
        case Command::Delete:
            return delete_trace(*store, options);
    }
    return 2;
}

}  // namespace avel::cli
