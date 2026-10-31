// A trace's review (docs/trace.md, "The review"): the items the user judged aren't the program's,
// kept in review.json in the trace's folder. Uninstalling and forcing by the trace
// (trace_source.h) never offer them; what the trace recorded (changes.json) stays as it was, so
// an item taken back in is offered again.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/changes/serialization.h"
#include "core/leftovers/leftover.h"
#include "core/platform/registry_address.h"
#include "core/tracker/trace.h"
#include "core/tracker/trace_record.h"

namespace avel::tracker {

inline constexpr std::wstring_view kReviewFile = L"review.json";
inline constexpr int kReviewSchemaVersion = 1;

// A known-value change as the review names it.
struct KnownValueId {
    KnownSpot spot = KnownSpot::MachineEnvironment;
    platform::RegistryAddress key;
    std::wstring name;
};

struct TraceReview {
    // Changes, by their location; compared by leftovers::identity, so names compare the way
    // Windows compares them.
    std::vector<leftovers::Location> excluded;
    // Known-value changes, by spot, key and value name.
    std::vector<KnownValueId> excluded_values;

    bool empty() const { return excluded.empty() && excluded_values.empty(); }
};

bool is_excluded(const TraceReview& review, const TraceChange& change);
bool is_excluded(const TraceReview& review, const KnownValueChange& change);

// Excludes the item, or takes it back in; true if the review changed.
bool set_excluded(TraceReview& review, const TraceChange& change, bool excluded);
bool set_excluded(TraceReview& review, const KnownValueChange& change, bool excluded);

// `result` without what the review excludes, in the same order.
TraceResult apply_review(const TraceResult& result, const TraceReview& review);

// {"schema_version": 1, "excluded": [<location>, ...], "excluded_values": [{"spot", "key",
// "name"}, ...]}, locations as changes::to_json writes them and names that UTF-8 can't carry as
// hex, so the review round-trips exactly.
changes::Json to_json(const TraceReview& review);
// False for malformed JSON and for another schema version.
bool from_json(const changes::Json& json, TraceReview& review);

}  // namespace avel::tracker
