#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>

#include "core/leftovers/confidence.h"
#include "core/leftovers/scan.h"

namespace avel::cli {

// A location's kind ("folder", "registry-key", ...), its text as shown to people
// ("HKLM\SOFTWARE\x : value"), and its JSON form.
std::string_view kind_of(const leftovers::Location& location);
std::wstring describe_location(const leftovers::Location& location);
nlohmann::ordered_json location_to_json(const leftovers::Location& location);

// The scan result as JSON: {"leftovers": [...], "protected_count", "elapsed_ms", "timings",
// "diagnostics", "traces"}.
// Items below `min_level` are left out; each item keeps its "index" in the full result, which
// "inside" refers to. "held_back": never selected automatically (leftovers::held_back).
nlohmann::ordered_json leftovers_to_json(const leftovers::ScanResult& result, leftovers::Confidence min_level);

// What the targets' install traces brought: [{"program_id", "trace_id", "items", "tied",
// "value_edits", "gone", "shown_only", "protected", "split", "split_into", "excluded",
// "values": [{"key", "name", "basis"}]}].
nlohmann::ordered_json traces_to_json(const leftovers::ScanResult& result);

// One line per item at or above `min_level`, then a summary.
void print_leftovers(const leftovers::ScanResult& result, leftovers::Confidence min_level);

// The removable items at or above `min_level` that removal by that level leaves in place, since
// they are never selected automatically (leftovers::held_back), and a line saying so when there
// are any.
std::size_t held_back_count(const leftovers::ScanResult& result, leftovers::Confidence min_level);
void print_held_back(const leftovers::ScanResult& result, leftovers::Confidence min_level);

}  // namespace avel::cli
