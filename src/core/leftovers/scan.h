#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/leftover.h"
#include "core/leftovers/scan_target.h"

namespace avel::leftovers {

struct ScanTiming {
    std::string_view stage;  // "folders", "keys", "shortcuts", ..., "matching", "protection"
    std::chrono::milliseconds elapsed{};
};

// What became of a value a target's install trace saved (TraceUse::values).
struct TracedValueOutcome {
    platform::RegistryAddress key;
    std::wstring name;
    // Undo or Restore: an edit is planned (one edit per value, shared by every trace that changed
    // it). Otherwise why not; Unsupported also when the value couldn't be read.
    ValueEditBasis basis = ValueEditBasis::Unsupported;
};

// What a target's install trace brought to the scan (Scanner::Trace). The counts are of the
// reported leftovers, after merging and protection.
struct TraceUse {
    std::wstring program_id;
    std::wstring trace_id;
    std::size_t items = 0;            // Leftovers with the trace's evidence, value edits aside
    std::size_t tied = 0;             // Of them, tied to the program
    std::size_t value_edits = 0;      // Value edits it planned
    std::size_t gone = 0;             // Items it created that no longer exist (left out)
    std::size_t shown_only = 0;       // Items that existed before the install and changed: never offered
    std::size_t protected_count = 0;  // Its items left out because they are protected
    // Created folders and keys that now hold more than the trace saw in them: not offered whole,
    // but as the items the trace saw in them (`split_into` of them, counted in `items` or
    // `protected_count`). One whose items the trace didn't keep is offered whole, with
    // Evidence::ContainerHoldsMore, and isn't counted here.
    std::size_t split = 0;
    std::size_t split_into = 0;
    std::vector<TracedValueOutcome> values;  // In the order of ScanTarget::traced_values
};

struct ScanResult {
    std::vector<Leftover> leftovers;  // Sorted by category and location; see Leftover::inside
    // The targets as the scan used them: protected folders cleared, and an MSI program's folder
    // found from its components when it records none.
    std::vector<ScanTarget> targets;
    std::vector<ScanDiagnostic> diagnostics;
    std::size_t protected_count = 0;  // Matches dropped because they are protected (see protection.h)
    std::vector<ScanTiming> timings;
    std::chrono::milliseconds elapsed{};
    std::vector<TraceUse> traces;  // One per target that has an install trace, in target order
};

struct ScanOptions {
    // Also look for folders named after the programs anywhere on the fixed NTFS volumes, read
    // from their master file tables (ADR 0005). Needs administrator rights; takes a few seconds.
    bool deep = false;
};

// Finds what the target programs left behind, or would leave if their uninstallers removed
// nothing (for a program that is still installed). A target with an install trace also gets what
// the trace saw the installer create and the value edits that take back its changes to the
// environment and SharedDLLs (Scanner::Trace, docs/leftovers.md).
// `installed` is what is installed now; the targets' own entries in it are ignored, the rest
// tells which folders other programs still use. Every match passes the hard floor and the
// protected-folder policy before it is reported.
// Takes a few hundred milliseconds: it reads folders, keys and shortcuts once for all targets.
ScanResult scan_leftovers(const ScanEnvironment& environment, std::span<const ScanTarget> targets,
                          std::span<const inventory::Program> installed, const ScanOptions& options = {});

}  // namespace avel::leftovers
