// The options of `uninstall`, `force` and `scan`, parsed apart from running the commands so the
// parsing can be tested on its own.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/leftovers/confidence.h"

namespace avel::cli {

// A usage error (exit code 2): the message to print, then the command's usage text unless `usage`
// is false.
struct UsageError {
    std::string message;
    bool usage = true;
};

struct UninstallCommandOptions {
    std::vector<std::wstring> programs;
    bool quiet = false;
    bool dry_run = false;
    bool yes = false;
    bool restore_point = true;
    bool leftover_scan = true;
    bool remove_leftovers = false;
    leftovers::Confidence min_level = leftovers::Confidence::Good;
    bool allow_irreversible = false;
    bool json = false;
    bool deep = false;
    bool trace = true;  // The programs' install traces join the leftover scan (--no-trace: they don't)
};

struct ForceCommandOptions {
    std::vector<std::wstring> programs;
    std::wstring folder;
    std::wstring name;
    std::wstring exe;
    std::wstring trace_id;  // --trace: revert by this trace, whatever became of its program
    bool dry_run = false;
    bool yes = false;
    bool end_processes = false;
    leftovers::Confidence min_level = leftovers::Confidence::Good;
    bool allow_irreversible = false;
    bool json = false;
    bool deep = false;
    bool trace = true;  // A program's install trace joins the plan (--no-trace: it doesn't)
};

struct ScanCommandOptions {
    std::vector<std::wstring> programs;
    bool all = false;
    bool deep = false;
    bool json = false;
    bool timing = false;
    leftovers::Confidence min_level = leftovers::Confidence::Questionable;
    bool trace = true;  // Show what the programs' install traces recorded (--no-trace: don't)
};

// `arguments` are those after the command's name.
std::optional<UsageError> parse_uninstall_options(const std::vector<std::wstring_view>& arguments,
                                                  UninstallCommandOptions& options);
std::optional<UsageError> parse_force_options(const std::vector<std::wstring_view>& arguments,
                                              ForceCommandOptions& options);
std::optional<UsageError> parse_scan_options(const std::vector<std::wstring_view>& arguments,
                                             ScanCommandOptions& options);

}  // namespace avel::cli
