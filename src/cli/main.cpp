// avel-cli: command-line front end for the Avel engine.

#include <windows.h>
#include <objbase.h>

#include <string>
#include <string_view>
#include <vector>

#include "cli/console.h"
#include "cli/list_command.h"
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
    "  list [--all] [--json] [--timing]\n"
    "             List installed programs. By default shows what Settings > Apps shows;\n"
    "             --all adds updates, system components and frameworks.\n"
    "  uninstall <program>... [--quiet] [--dry-run] [--yes] [--no-restore-point] [--json]\n"
    "             Uninstall programs (by id or exact name) one after another, after\n"
    "             creating a System Restore point. Run as administrator.\n"
    "  version    Show version and environment information\n"
    "  help       Show this help\n"
    "\n"
    "Planned: scan, force, trace, undo.\n";

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
