// Install traces in `uninstall`, `force` and `scan` (docs/trace.md, "Uninstalling by the trace").
#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string_view>

#include "core/leftovers/scan_target.h"
#include "core/tracker/trace_store.h"

namespace avel::cli {

// The trace store, to use the traces in it. Null when nothing was ever traced on this machine (the
// store isn't created for that), and, with a warning naming `command`, when it can't be trusted.
// It is administrators' only: call it elevated.
std::unique_ptr<tracker::TraceStore> open_trace_store_for(std::string_view command);

// Retention right after programs were removed, so their traces' 30 days count from now
// (TraceStore::purge with a fresh inventory; skipped when the inventory finds nothing).
void apply_trace_retention(tracker::TraceStore& store);

// The target's trace id, or null.
nlohmann::ordered_json trace_id_json(const leftovers::ScanTarget& target);

}  // namespace avel::cli
