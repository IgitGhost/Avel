// avel-cli: command-line front end for the Avel engine.

#include <windows.h>
#include <objbase.h>

#include <string>
#include <string_view>
#include <vector>

#include "cli/console.h"
#include "cli/force_command.h"
#include "cli/list_command.h"
#include "cli/orphans_command.h"
#include "cli/scan_command.h"
#include "cli/startup_command.h"
#include "cli/trace_command.h"
#include "cli/undo_command.h"
#include "cli/uninstall_command.h"
#include "core/platform/com_security.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/version.h"

namespace {

using avel::cli::print;
using avel::cli::print_error;

constexpr std::string_view kUsage =
    "Usage: avel-cli <command> [options]\n"
    "\n"
    "Commands:\n"
    "  list [--all | --broken] [--json] [--timing]\n"
    "             List installed programs. By default shows what Settings > Apps shows;\n"
    "             --all adds updates, system components and frameworks; --broken shows\n"
    "             the entries that can't uninstall themselves (see force).\n"
    "  uninstall <program>... [--quiet] [--dry-run] [--yes] [--no-restore-point]\n"
    "            [--no-leftover-scan | --remove-leftovers [--min-level <level>]] [--no-trace]\n"
    "            [--deep] [--json]\n"
    "             Uninstall programs (by id or exact name) one after another, after\n"
    "             creating a System Restore point, then show what they left behind,\n"
    "             or remove it with a backup. A program's install trace adds what its\n"
    "             installer created and its edits to Path and SharedDLLs (--no-trace:\n"
    "             it doesn't). Run as administrator.\n"
    "  scan <program>... | --all [--deep] [--json] [--min-level <level>] [--all-levels] [--no-trace]\n"
    "             Show what programs would leave behind if their uninstallers removed\n"
    "             nothing, with what their install traces recorded. Read-only.\n"
    "  force <program>... | --folder <path> [--name <name>] | --exe <path> | --trace <id>\n"
    "        [--dry-run] [--yes] [--end-processes] [--deep] [--min-level <level>] [--no-trace]\n"
    "        [--json]\n"
    "             Remove programs whose uninstaller is missing or broken, without\n"
    "             running it, backed up so undo can put them back. A program installed\n"
    "             with a trace is reverted by it; --trace reverts one trace even when its\n"
    "             program's entry is gone. Run as administrator.\n"
    "  orphans [--json] [--all-levels]\n"
    "             Show folders in Program Files, ProgramData and AppData\\Local\\Programs\n"
    "             that no installed program owns. Read-only.\n"
    "  undo --list | --show <id> | <id> [--item <n>]... | --purge | --recover [--json]\n"
    "             List removals, or put back what one removed.\n"
    "             Run as administrator.\n"
    "  trace start [--description <text>] | stop | list | show <id> [--all] | cancel | delete <id>\n"
    "  trace run [--description <text>] [--timeout <seconds>] -- <installer> [<args>...]\n"
    "  trace link <id> <program> | export <id> <file> | import <file>\n"
    "             Record what an installer changes: between start and stop, or around an\n"
    "             installer that run launches and waits for. A finished trace is linked to\n"
    "             the program it installed, and can be moved to another machine. --json for\n"
    "             all but cancel, delete and export. Run as administrator.\n"
    "  startup list [--all-users] [--json]\n"
    "  startup disable <id>... [--service-start manual|disabled] | enable <id>... | delete <id>...\n"
    "          [--dry-run] [--yes] [--json]\n"
    "             List what starts with Windows (Run values, Startup folders, tasks,\n"
    "             automatic services), or disable, enable or delete entries, backed up so\n"
    "             undo can put them back. list runs without administrator rights; the\n"
    "             other commands need them.\n"
    "  version    Show version and environment information\n"
    "  help       Show this help\n";

void print_version() {
    std::string text = "Avel (Ավել) ";
    text += avel::kVersionString;
    text += "\nElevated: ";
    text += avel::platform::is_process_elevated() ? "yes" : "no";
    text += '\n';
    print(text);
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const std::wstring_view command = argc > 1 ? argv[1] : L"help";
    const std::vector<std::wstring_view> arguments(argv + (argc > 1 ? 2 : 1), argv + argc);

    // The main thread joins the multithreaded apartment for the life of the process; System
    // Restore needs the process-wide COM security set right after.
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    avel::platform::initialize_com_security();

    if (command == L"list") {
        return avel::cli::run_list_command(arguments);
    }
    if (command == L"uninstall") {
        return avel::cli::run_uninstall_command(arguments);
    }
    if (command == L"scan") {
        return avel::cli::run_scan_command(arguments);
    }
    if (command == L"force") {
        return avel::cli::run_force_command(arguments);
    }
    if (command == L"orphans") {
        return avel::cli::run_orphans_command(arguments);
    }
    if (command == L"undo") {
        return avel::cli::run_undo_command(arguments);
    }
    if (command == L"trace") {
        return avel::cli::run_trace_command(arguments);
    }
    if (command == L"startup") {
        return avel::cli::run_startup_command(arguments);
    }
    if (command == L"version" || command == L"--version" || command == L"-v") {
        print_version();
        return 0;
    }
    if (command == L"help" || command == L"--help" || command == L"-h" || command == L"/?") {
        print(kUsage);
        return 0;
    }

    print_error("Unknown command: " + avel::platform::to_utf8(command) + "\n\n");
    print_error(kUsage);
    return 2;
}
