// How `avel-cli trace` prints traces (docs/trace.md): the JSON of the contract, in display forms
// (the store's own JSON keeps exact forms instead), and the human-readable text.
#pragma once

#include <nlohmann/json.hpp>

#include <vector>

#include "core/tracker/trace_items.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_review.h"

namespace avel::cli {

// The `trace` object: what the store keeps, without what only reading it back needs.
nlohmann::ordered_json trace_to_json(const tracker::TraceInfo& info);

// {"avel_version", "trace"}: `start`, and traces that aren't finished.
nlohmann::ordered_json trace_root_to_json(const tracker::TraceInfo& info);

// {"avel_version", "trace", "changes", "known_values"}: `stop`, `run` and `show`. Changes set
// aside as noise are left out unless `all`; `counts` covers both. With `tied` (the trace's program
// is installed), each change that isn't noise says whether it is tied to the program, and why. With
// `review`, changes and known values the user excluded in the trace's review say so ("excluded").
nlohmann::ordered_json trace_result_to_json(const tracker::TraceInfo& info,
                                            const tracker::TraceResult& result, bool all,
                                            const std::vector<tracker::TiedItem>* tied = nullptr,
                                            const tracker::TraceReview* review = nullptr);

// The id, state, times, volumes, registry, the installer's watch, and the links.
void print_trace_header(const tracker::TraceInfo& info);

// The changes grouped by kind (noise only with `all`), marked when tied or excluded by `review`, the
// known values, then a summary.
void print_trace_changes(const tracker::TraceInfo& info, const tracker::TraceResult& result, bool all,
                         const std::vector<tracker::TiedItem>* tied = nullptr,
                         const tracker::TraceReview* review = nullptr);

// One line per trace.
void print_trace_list(const std::vector<tracker::TraceInfo>& traces);

// An incomplete trace's reasons, as a warning on stderr; nothing for any other trace.
void warn_if_incomplete(const tracker::TraceInfo& info);

}  // namespace avel::cli
