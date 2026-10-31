#include "core/tracker/trace_review.h"

#include <algorithm>

#include "core/tracker/known_values.h"

namespace avel::tracker {

namespace {

using changes::Json;

bool same_value(const KnownValueId& id, const KnownValueChange& change) {
    return id.spot == change.spot && platform::same_address(id.key, change.key) &&
           platform::iequals(id.name, change.name);
}

}  // namespace

bool is_excluded(const TraceReview& review, const TraceChange& change) {
    if (review.excluded.empty()) {
        return false;
    }
    const std::wstring wanted = leftovers::identity(change.location);
    return std::any_of(
        review.excluded.begin(), review.excluded.end(),
        [&](const leftovers::Location& location) { return leftovers::identity(location) == wanted; });
}

bool is_excluded(const TraceReview& review, const KnownValueChange& change) {
    return std::any_of(review.excluded_values.begin(), review.excluded_values.end(),
                       [&](const KnownValueId& id) { return same_value(id, change); });
}

bool set_excluded(TraceReview& review, const TraceChange& change, bool excluded) {
    if (is_excluded(review, change) == excluded) {
        return false;
    }
    if (excluded) {
        review.excluded.push_back(change.location);
    } else {
        const std::wstring wanted = leftovers::identity(change.location);
        std::erase_if(review.excluded, [&](const leftovers::Location& location) {
            return leftovers::identity(location) == wanted;
        });
    }
    return true;
}

bool set_excluded(TraceReview& review, const KnownValueChange& change, bool excluded) {
    if (is_excluded(review, change) == excluded) {
        return false;
    }
    if (excluded) {
        review.excluded_values.push_back({change.spot, change.key, change.name});
    } else {
        std::erase_if(review.excluded_values, [&](const KnownValueId& id) { return same_value(id, change); });
    }
    return true;
}

TraceResult apply_review(const TraceResult& result, const TraceReview& review) {
    if (review.empty()) {
        return result;
    }
    TraceResult kept;
    for (const auto& change : result.changes) {
        if (!is_excluded(review, change)) {
            kept.changes.push_back(change);
        }
    }
    for (const auto& change : result.known_values) {
        if (!is_excluded(review, change)) {
            kept.known_values.push_back(change);
        }
    }
    return kept;
}

Json to_json(const TraceReview& review) {
    Json json;
    json["schema_version"] = kReviewSchemaVersion;
    json["excluded"] = Json::array();
    for (const auto& location : review.excluded) {
        json["excluded"].push_back(changes::to_json(location));
    }
    json["excluded_values"] = Json::array();
    for (const auto& id : review.excluded_values) {
        // A known-value change without its values: {"spot", "key", "name"}.
        json["excluded_values"].push_back(to_json(KnownValueChange{id.spot, id.key, id.name, {}, {}}));
    }
    return json;
}

bool from_json(const Json& json, TraceReview& review) {
    if (!json.is_object() || json.value("schema_version", 0) != kReviewSchemaVersion) {
        return false;
    }
    const auto excluded = json.find("excluded");
    const auto values = json.find("excluded_values");
    if (excluded == json.end() || !excluded->is_array() || values == json.end() || !values->is_array()) {
        return false;
    }
    TraceReview parsed;
    for (const auto& item : *excluded) {
        if (!changes::from_json(item, parsed.excluded.emplace_back())) {
            return false;
        }
    }
    for (const auto& item : *values) {
        KnownValueChange change;
        if (!from_json(item, change)) {
            return false;
        }
        parsed.excluded_values.push_back({change.spot, std::move(change.key), std::move(change.name)});
    }
    review = std::move(parsed);
    return true;
}

}  // namespace avel::tracker
