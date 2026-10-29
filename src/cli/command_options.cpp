#include "cli/command_options.h"

#include "core/platform/text.h"

namespace avel::cli {

namespace {

using leftovers::Confidence;
using platform::to_utf8;

// The least likely items a removal takes: good or very-good.
bool parse_removal_level(std::wstring_view text, Confidence& level) {
    if (text == L"good") {
        level = Confidence::Good;
    } else if (text == L"very-good") {
        level = Confidence::VeryGood;
    } else {
        return false;
    }
    return true;
}

// The least likely items a preview shows: questionable, good or very-good.
bool parse_preview_level(std::wstring_view text, Confidence& level) {
    if (text == L"questionable") {
        level = Confidence::Questionable;
        return true;
    }
    return parse_removal_level(text, level);
}

}  // namespace

std::optional<UsageError> parse_uninstall_options(const std::vector<std::wstring_view>& arguments,
                                                  UninstallCommandOptions& options) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (argument == L"--remove-leftovers") {
            options.remove_leftovers = true;
        } else if (argument == L"--allow-irreversible") {
            options.allow_irreversible = true;
        } else if (argument == L"--min-level" && i + 1 < arguments.size()) {
            if (!parse_removal_level(arguments[++i], options.min_level)) {
                return UsageError{"uninstall: --min-level is good or very-good\n", false};
            }
        } else if (argument == L"--quiet") {
            options.quiet = true;
        } else if (argument == L"--dry-run") {
            options.dry_run = true;
        } else if (argument == L"--yes" || argument == L"-y") {
            options.yes = true;
        } else if (argument == L"--no-restore-point") {
            options.restore_point = false;
        } else if (argument == L"--deep") {
            options.deep = true;
        } else if (argument == L"--no-leftover-scan") {
            options.leftover_scan = false;
        } else if (argument == L"--no-trace") {
            options.trace = false;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument.starts_with(L"--")) {
            return UsageError{"uninstall: unknown option " + to_utf8(argument) + "\n"};
        } else {
            options.programs.emplace_back(argument);
        }
    }
    if (options.programs.empty()) {
        return UsageError{};
    }
    if (options.json && !options.yes && !options.dry_run) {
        return UsageError{"uninstall: --json needs --yes or --dry-run (it can't ask for confirmation)\n",
                          false};
    }
    if (options.remove_leftovers && (options.dry_run || !options.leftover_scan)) {
        return UsageError{"uninstall: --remove-leftovers can't go with --dry-run or --no-leftover-scan\n",
                          false};
    }
    return std::nullopt;
}

std::optional<UsageError> parse_force_options(const std::vector<std::wstring_view>& arguments,
                                              ForceCommandOptions& options) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        const bool has_value = i + 1 < arguments.size();
        if (argument == L"--folder" && has_value) {
            options.folder = arguments[++i];
        } else if (argument == L"--name" && has_value) {
            options.name = arguments[++i];
        } else if (argument == L"--exe" && has_value) {
            options.exe = arguments[++i];
        } else if (argument == L"--trace" && has_value) {
            options.trace_id = arguments[++i];
        } else if (argument == L"--dry-run") {
            options.dry_run = true;
        } else if (argument == L"--yes" || argument == L"-y") {
            options.yes = true;
        } else if (argument == L"--end-processes") {
            options.end_processes = true;
        } else if (argument == L"--deep") {
            options.deep = true;
        } else if (argument == L"--allow-irreversible") {
            options.allow_irreversible = true;
        } else if (argument == L"--no-trace") {
            options.trace = false;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument == L"--min-level" && has_value) {
            if (!parse_removal_level(arguments[++i], options.min_level)) {
                return UsageError{"force: --min-level is good or very-good\n", false};
            }
        } else if (argument.starts_with(L"--")) {
            return UsageError{"force: unknown option " + to_utf8(argument) + "\n"};
        } else {
            options.programs.emplace_back(argument);
        }
    }
    const int sources = !options.programs.empty() + !options.folder.empty() + !options.exe.empty() +
                        !options.trace_id.empty();
    if (sources != 1 || (!options.name.empty() && options.folder.empty())) {
        return UsageError{};
    }
    if (!options.trace_id.empty() && !options.trace) {
        return UsageError{"force: --trace can't go with --no-trace\n", false};
    }
    if (options.json && !options.yes && !options.dry_run) {
        return UsageError{"force: --json needs --yes or --dry-run (it can't ask for confirmation)\n", false};
    }
    return std::nullopt;
}

std::optional<UsageError> parse_scan_options(const std::vector<std::wstring_view>& arguments,
                                             ScanCommandOptions& options) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (argument == L"--all") {
            options.all = true;
        } else if (argument == L"--deep") {
            options.deep = true;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument == L"--timing") {
            options.timing = true;
        } else if (argument == L"--no-trace") {
            options.trace = false;
        } else if (argument == L"--all-levels") {
            options.min_level = Confidence::Bad;
        } else if (argument == L"--min-level" && i + 1 < arguments.size() &&
                   parse_preview_level(arguments[i + 1], options.min_level)) {
            ++i;
        } else if (argument.starts_with(L"--")) {
            return UsageError{};
        } else {
            options.programs.emplace_back(argument);
        }
    }
    if (options.all == !options.programs.empty()) {
        return UsageError{};
    }
    return std::nullopt;
}

}  // namespace avel::cli
