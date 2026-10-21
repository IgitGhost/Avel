#pragma once

#include <nlohmann/json.hpp>

#include "core/leftovers/confidence.h"
#include "core/leftovers/scan.h"

namespace avel::cli {

// A location's kind ("folder", "registry-key", ...), its text as shown to people
// ("HKLM\SOFTWARE\x : value"), and its JSON form.
std::string_view kind_of(const leftovers::Location& location);
std::wstring describe_location(const leftovers::Location& location);
nlohmann::ordered_json location_to_json(const leftovers::Location& location);

// The scan result as JSON: {"leftovers": [...], "protected_count", "elapsed_ms", "timings",
// "diagnostics"}.
// Items below `min_level` are left out; each item keeps its "index" in the full result, which
// "inside" refers to.
nlohmann::ordered_json leftovers_to_json(const leftovers::ScanResult& result, leftovers::Confidence min_level);

// One line per item at or above `min_level`, then a summary.
void print_leftovers(const leftovers::ScanResult& result, leftovers::Confidence min_level);

}  // namespace avel::cli
