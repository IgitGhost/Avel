// How `avel-cli trace` prints traces (docs/trace.md): the JSON of the contract, in display forms
// (the store's own JSON keeps exact forms instead), and the human-readable text.
#pragma once

#include <nlohmann/json.hpp>

#include <vector>

#include "core/tracker/trace_record.h"

namespace avel::cli {

// The `trace` object: what the store keeps, without what only reading it back needs.
nlohmann::ordered_json trace_to_json(const tracker::TraceInfo& info);

// {"avel_version", "trace"}: `start`, and traces that aren't finished.
nlohmann::ordered_json trace_root_to_json(const tracker::TraceInfo& info);

// {"avel_version", "trace", "changes", "known_values"}: `stop`, `run` and `show`. Changes set
// aside as noise are left out unless `all`; `counts` covers both.
nlohmann::ordered_json trace_result_to_json(const tracker::TraceInfo& info,
                                            const tracker::TraceResult& result, bool all);

// The id, state, times, volumes, registry and the installer's watch.
void print_trace_header(const tracker::TraceInfo& info);

// The changes grouped by kind (noise only with `all`), the known values, then a summary.
void print_trace_changes(const tracker::TraceInfo& info, const tracker::TraceResult& result, bool all);

// One line per trace.
void print_trace_list(const std::vector<tracker::TraceInfo>& traces);

// An incomplete trace's reasons, as a warning on stderr; nothing for any other trace.
void warn_if_incomplete(const tracker::TraceInfo& info);

}  // namespace avel::cli
