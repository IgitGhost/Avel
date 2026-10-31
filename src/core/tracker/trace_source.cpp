#include "core/tracker/trace_source.h"

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>

#include "core/leftovers/product_name.h"
#include "core/platform/nt_registry.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/tracker/linking.h"

namespace avel::tracker {

namespace {

using leftovers::CapturedValue;

static_assert(static_cast<int>(ChangeKind::Created) == static_cast<int>(leftovers::TracedChange::Created) &&
              static_cast<int>(ChangeKind::Deleted) == static_cast<int>(leftovers::TracedChange::Deleted) &&
              static_cast<int>(ChangeKind::Modified) == static_cast<int>(leftovers::TracedChange::Modified) &&
              static_cast<int>(ChangeKind::Renamed) == static_cast<int>(leftovers::TracedChange::Renamed));
static_assert(static_cast<int>(KnownSpot::MachineEnvironment) ==
                  static_cast<int>(leftovers::ValueSpot::MachineEnvironment) &&
              static_cast<int>(KnownSpot::UserEnvironment) ==
                  static_cast<int>(leftovers::ValueSpot::UserEnvironment) &&
              static_cast<int>(KnownSpot::SharedDlls) == static_cast<int>(leftovers::ValueSpot::SharedDlls));

bool finished(TraceState state) { return state == TraceState::Complete || state == TraceState::Incomplete; }

// The live registry: HKLM, or HKU\<sid> while the user's hive is loaded (a user who isn't signed in
// can't be read, which isn't the same as the value being absent).
CapturedValue read_live_value(const platform::RegistryAddress& key, const std::wstring& name) {
    const bool machine = key.hive == platform::Hive::Machine;
    const HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_USERS;
    if (!machine) {
        HKEY hive = nullptr;
        if (key.sid.empty() ||
            ::RegOpenKeyExW(root, key.sid.c_str(), 0, KEY_QUERY_VALUE, &hive) != ERROR_SUCCESS) {
            return {};
        }
        ::RegCloseKey(hive);
    }
    const std::wstring path = machine ? key.path : key.sid + L"\\" + key.path;
    HKEY handle = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &handle);
    if (status == ERROR_FILE_NOT_FOUND) {
        return {true, std::nullopt};
    }
    if (status != ERROR_SUCCESS) {
        return {};
    }
    const wil::unique_hkey opened(handle);
    platform::RawValue value;
    const LSTATUS query = platform::query_raw_value(opened.get(), name, value);
    if (query == ERROR_SUCCESS) {
        return {true, std::move(value)};
    }
    return query == ERROR_FILE_NOT_FOUND ? CapturedValue{true, std::nullopt} : CapturedValue{};
}

}  // namespace

StoreTraceSource::StoreTraceSource(const TraceStore& store, const inventory::Inventory& inventory,
                                   StoreTraceSourceOptions options)
    : store_(store), inventory_(inventory), options_(std::move(options)), traces_(store.list()) {
    if (!options_.read_value) {
        options_.read_value = read_live_value;
    }
}

bool StoreTraceSource::add_trace(const inventory::Program& program, leftovers::ScanTarget& target) {
    for (const auto& id : traces_of(program, traces_, inventory_)) {
        const auto info = std::find_if(traces_.begin(), traces_.end(),
                                       [&](const TraceInfo& trace) { return trace.id == id; });
        if (info == traces_.end()) {
            continue;
        }
        // A damaged review could hide what the user excluded: such a trace isn't used.
        const auto result = store_.read_result(id);
        const auto review = result ? store_.read_review(id) : std::nullopt;
        if (result && review) {
            add(*info, *result, *review, program_places(program), target);
            return true;
        }
    }
    return false;
}

std::optional<std::wstring> StoreTraceSource::trace_of(const inventory::Program& program) const {
    const auto ids = traces_of(program, traces_, inventory_);
    return ids.empty() ? std::nullopt : std::optional<std::wstring>(ids.front());
}

std::optional<leftovers::ScanTarget> StoreTraceSource::trace_target(const std::wstring& id,
                                                                    TraceError& error) {
    const auto info = store_.read(id);
    if (!info) {
        error = TraceError::NotFound;
        return std::nullopt;
    }
    if (!finished(info->state)) {
        error = TraceError::NotFinished;
        return std::nullopt;
    }
    const auto result = store_.read_result(id);
    const auto review = result ? store_.read_review(id) : std::nullopt;
    if (!result || !review) {
        error = TraceError::Damaged;
        return std::nullopt;
    }
    const auto resolved = resolve_trace_program(*info, inventory_);
    const ProgramPlaces places = resolved ? program_places(*resolved->program) : recorded_places(*info);
    leftovers::ScanTarget target = places.target;
    add(*info, *result, *review, places, target);
    error = TraceError::None;
    return target;
}

void StoreTraceSource::add(const TraceInfo& info, const TraceResult& recorded, const TraceReview& review,
                           const ProgramPlaces& places, leftovers::ScanTarget& target) {
    if (!options_.tie) {
        options_.tie = live_tie_sources();
    }
    // Left out before tying, so nothing is tied by an excluded item alone.
    const TraceResult result = apply_review(recorded, review);
    target.trace_id = info.id;
    // Other scanners may still find what the user excluded: the scan says so (leftovers::scan_leftovers).
    target.trace_excluded = review.excluded;
    for (const auto& value : review.excluded_values) {
        target.trace_excluded.push_back(leftovers::RegistryValueLocation{value.key, value.name});
    }
    target.traced.clear();
    for (const auto& tied : tie_trace_items(result, places, *options_.tie)) {
        const TraceChange& change = result.changes[tied.change];
        leftovers::TracedItem item;
        item.location = change.location;
        item.category = leftovers::category_of(change.location);
        item.change = static_cast<leftovers::TracedChange>(change.change);
        item.tied = tied.tied;
        if (tied.tied) {
            item.tie = platform::to_utf16(to_string(tied.reason));
        }
        item.contents = change.contents;
        item.inside = change.inside;
        target.traced.push_back(std::move(item));
    }
    target.traced_values.clear();
    target.captured_values.clear();
    for (const auto& known : result.known_values) {
        target.traced_values.push_back({static_cast<leftovers::ValueSpot>(known.spot), known.key, known.name,
                                        known.before, known.after});
        target.captured_values.push_back(options_.read_value(known.key, known.name));
    }
}

TraceOffer trace_offer(const TraceResult& result) {
    TraceOffer offer;
    for (const auto& change : result.changes) {
        if (change.noise == NoiseReason::None) {
            ++(change.change == ChangeKind::Created ? offer.created : offer.changed);
        }
    }
    offer.values = result.known_values.size();
    return offer;
}

ProgramPlaces recorded_places(const TraceInfo& info) {
    ProgramPlaces places;
    auto& target = places.target;
    target.program_id = L"trace:" + info.id;
    const auto primary = std::find_if(info.links.begin(), info.links.end(),
                                      [](const TraceLink& link) { return link.primary; });
    const TraceLink* link = primary != info.links.end() ? &*primary
                            : info.links.empty()        ? nullptr
                                                        : &info.links.front();
    if (link) {
        target.display_name = link->name;
        target.names = leftovers::make_product_names(link->name, link->publisher);
        target.uninstall_key = link->uninstall_key;
        target.msi_product_code = link->product_code;
        if (platform::is_absolute_path(link->install_location)) {
            target.install_location = platform::normalize_path(link->install_location);
        }
        if (link->uninstall_key && link->uninstall_key->hive == platform::Hive::User) {
            target.scope = inventory::Scope::User;
            target.user_sid = link->uninstall_key->sid;
        }
    } else {
        target.display_name = info.description;
        target.names = leftovers::make_product_names(info.description, L"");
    }
    if (target.display_name.empty()) {
        target.display_name = L"trace " + info.id;
    }
    return places;
}

}  // namespace avel::tracker
