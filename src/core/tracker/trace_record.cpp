#include "core/tracker/trace_record.h"

#include <array>

#include "core/platform/record_folders.h"
#include "core/platform/text.h"
#include "core/tracker/known_values.h"

namespace avel::tracker {

namespace {

using changes::get_text;
using changes::Json;
using changes::put_text;

constexpr std::array kStates{TraceState::Starting, TraceState::Open, TraceState::Stopping,
                             TraceState::Complete, TraceState::Incomplete};
constexpr std::array kModes{TraceMode::Manual, TraceMode::Launch};
constexpr std::array kStopReasons{StopReason::None,    StopReason::Manual,   StopReason::InstallerDone,
                                  StopReason::Stopped, StopReason::TimedOut, StopReason::RebootPending};
constexpr std::array kDiagnostics{
    TraceDiagnosticCode::CheckpointDamaged, TraceDiagnosticCode::SnapshotDamaged,
    TraceDiagnosticCode::SavedValuesDamaged, TraceDiagnosticCode::UserNotRemapped};
constexpr std::array kLinkMatches{LinkMatch::CreatedKey, LinkMatch::ChangedKey, LinkMatch::ProductCode,
                                  LinkMatch::Manual};
constexpr std::array kChanges{ChangeKind::Created, ChangeKind::Deleted, ChangeKind::Modified,
                              ChangeKind::Renamed};
constexpr std::array kNoise{NoiseReason::None,        NoiseReason::SystemActivity, NoiseReason::UserActivity,
                            NoiseReason::Temporary,   NoiseReason::Avel,           NoiseReason::Unresolved,
                            NoiseReason::MetadataOnly};
constexpr std::array kJournalStatuses{JournalStatus::Ok,
                                      JournalStatus::Untracked,
                                      JournalStatus::VolumeMissing,
                                      JournalStatus::SerialChanged,
                                      JournalStatus::Replaced,
                                      JournalStatus::Wrapped,
                                      JournalStatus::Unreadable};
constexpr std::array kSnapshotCodes{SnapshotDiagnosticCode::RootUnavailable,
                                    SnapshotDiagnosticCode::KeyUnreadable,
                                    SnapshotDiagnosticCode::KeyUnreadableAtStart};
constexpr std::array kWatchEnds{WatchEnd::InstallerDone, WatchEnd::Stopped, WatchEnd::TimedOut,
                                WatchEnd::LaunchFailed};

// The enum value whose to_string is the string at `field`.
template <typename Enum, std::size_t N>
bool get_enum(const Json& json, std::string_view field, const std::array<Enum, N>& all, Enum& value) {
    const auto found = json.find(std::string(field));
    if (found == json.end() || !found->is_string()) {
        return false;
    }
    for (const Enum candidate : all) {
        if (to_string(candidate) == found->get_ref<const std::string&>()) {
            value = candidate;
            return true;
        }
    }
    return false;
}

// An integer at `field`, or false; a missing field leaves `value` alone when `optional`.
template <typename T>
bool get_number(const Json& json, std::string_view field, T& value, bool optional = true) {
    const auto found = json.find(std::string(field));
    if (found == json.end() || found->is_null()) {
        return optional;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    value = found->get<T>();
    return true;
}

bool get_bool(const Json& json, std::string_view field, bool& value) {
    const auto found = json.find(std::string(field));
    if (found == json.end()) {
        return true;
    }
    if (!found->is_boolean()) {
        return false;
    }
    value = found->get<bool>();
    return true;
}

const Json* get_array(const Json& json, std::string_view field) {
    const auto found = json.find(std::string(field));
    return found != json.end() && found->is_array() ? &*found : nullptr;
}

Json key_json(const platform::RegistryAddress& key) {
    return changes::to_json(leftovers::Location(leftovers::RegistryKeyLocation{key}));
}

bool key_from(const Json& json, platform::RegistryAddress& key) {
    leftovers::Location location;
    if (!changes::from_json(json, location)) {
        return false;
    }
    const auto* registry_key = std::get_if<leftovers::RegistryKeyLocation>(&location);
    if (!registry_key) {
        return false;
    }
    key = registry_key->key;
    return true;
}

bool is_container(const leftovers::Location& location) {
    const auto* file = std::get_if<leftovers::FileLocation>(&location);
    return (file && file->directory) || std::holds_alternative<leftovers::RegistryKeyLocation>(location);
}

Json volume_json(const VolumeTrace& volume) {
    Json json;
    put_text(json, "root", volume.root);
    json["status"] = to_string(volume.status);
    json["created_journal"] = volume.created_journal;
    json["error"] = volume.error;
    put_text(json, "guid_path", volume.guid_path);
    json["serial"] = volume.serial;
    json["root_frn"] = volume.root_frn;
    json["journal_id"] = volume.journal_id;
    json["start_usn"] = volume.start_usn;
    json["next_usn"] = volume.next_usn;
    return json;
}

bool volume_from(const Json& json, VolumeTrace& volume) {
    if (!json.is_object()) {
        return false;
    }
    const auto root = get_text(json, "root");
    if (!root || !get_enum(json, "status", kJournalStatuses, volume.status) ||
        !get_bool(json, "created_journal", volume.created_journal) ||
        !get_number(json, "error", volume.error) || !get_number(json, "serial", volume.serial) ||
        !get_number(json, "root_frn", volume.root_frn) ||
        !get_number(json, "journal_id", volume.journal_id) ||
        !get_number(json, "start_usn", volume.start_usn) || !get_number(json, "next_usn", volume.next_usn)) {
        return false;
    }
    volume.root = *root;
    volume.guid_path = get_text(json, "guid_path").value_or(L"");
    return true;
}

Json snapshot_diagnostic_json(const SnapshotDiagnostic& diagnostic) {
    Json json;
    json["code"] = to_string(diagnostic.code);
    json["key"] = key_json(diagnostic.key);
    json["error"] = diagnostic.error;
    return json;
}

bool snapshot_diagnostic_from(const Json& json, SnapshotDiagnostic& diagnostic) {
    const auto key = json.is_object() ? json.find("key") : json.end();
    return json.is_object() && get_enum(json, "code", kSnapshotCodes, diagnostic.code) && key != json.end() &&
           key_from(*key, diagnostic.key) && get_number(json, "error", diagnostic.error);
}

Json registry_json(const RegistrySummary& registry) {
    Json json;
    json["keys"] = registry.keys;
    json["values"] = registry.values;
    json["snapshot_ms"] = registry.snapshot_ms;
    json["diff_ms"] = registry.diff_ms;
    json["diagnostics"] = Json::array();
    for (const auto& diagnostic : registry.diagnostics) {
        json["diagnostics"].push_back(snapshot_diagnostic_json(diagnostic));
    }
    return json;
}

bool registry_from(const Json& json, RegistrySummary& registry) {
    if (!json.is_object() || !get_number(json, "keys", registry.keys) ||
        !get_number(json, "values", registry.values) ||
        !get_number(json, "snapshot_ms", registry.snapshot_ms) ||
        !get_number(json, "diff_ms", registry.diff_ms)) {
        return false;
    }
    if (const Json* diagnostics = get_array(json, "diagnostics")) {
        for (const auto& item : *diagnostics) {
            if (!snapshot_diagnostic_from(item, registry.diagnostics.emplace_back())) {
                return false;
            }
        }
    }
    return true;
}

Json watch_json(const WatchResult& watch) {
    Json json;
    json["end"] = to_string(watch.end);
    json["exit_code"] = watch.exit_code;
    json["reboot_requested"] = watch.reboot_requested;
    json["left_running"] = Json::array();
    for (const auto& process : watch.left_running) {
        Json item;
        item["pid"] = process.pid;
        put_text(item, "image", process.image);
        json["left_running"].push_back(std::move(item));
    }
    if (watch.error != 0) {
        json["error"] = watch.error;
    }
    return json;
}

bool watch_from(const Json& json, WatchResult& watch) {
    if (!json.is_object() || !get_enum(json, "end", kWatchEnds, watch.end) ||
        !get_number(json, "exit_code", watch.exit_code) ||
        !get_bool(json, "reboot_requested", watch.reboot_requested) ||
        !get_number(json, "error", watch.error)) {
        return false;
    }
    if (const Json* processes = get_array(json, "left_running")) {
        for (const auto& item : *processes) {
            WatchProcess process;
            if (!item.is_object() || !get_number(item, "pid", process.pid, false)) {
                return false;
            }
            process.image = get_text(item, "image").value_or(L"");
            watch.left_running.push_back(std::move(process));
        }
    }
    return true;
}

Json time_json(const std::optional<std::chrono::system_clock::time_point>& time) {
    return time ? Json(platform::iso_time(*time)) : Json(nullptr);
}

Json unix_json(const std::optional<std::chrono::system_clock::time_point>& time) {
    return time ? Json(platform::unix_seconds(*time)) : Json(nullptr);
}

// A time kept as `<field>_unix` (and `<field>` in ISO 8601 for people); null or missing is nullopt.
bool time_from(const Json& json, const std::string& field,
               std::optional<std::chrono::system_clock::time_point>& time) {
    const auto found = json.find(field + "_unix");
    if (found == json.end() || found->is_null()) {
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    time = platform::from_unix(found->get<std::int64_t>());
    return true;
}

Json import_json(const TraceImport& imported) {
    Json json;
    json["time"] = platform::iso_time(imported.time);
    json["time_unix"] = platform::unix_seconds(imported.time);
    put_text(json, "origin_id", imported.origin_id);
    put_text(json, "from_sid", imported.from_sid);
    put_text(json, "from_profile", imported.from_profile);
    put_text(json, "to_sid", imported.to_sid);
    put_text(json, "to_profile", imported.to_profile);
    return json;
}

bool import_from(const Json& json, TraceImport& imported) {
    std::optional<std::chrono::system_clock::time_point> time;
    if (!json.is_object() || !time_from(json, "time", time) || !time) {
        return false;
    }
    imported.time = *time;
    imported.origin_id = get_text(json, "origin_id").value_or(L"");
    imported.from_sid = get_text(json, "from_sid").value_or(L"");
    imported.from_profile = get_text(json, "from_profile").value_or(L"");
    imported.to_sid = get_text(json, "to_sid").value_or(L"");
    imported.to_profile = get_text(json, "to_profile").value_or(L"");
    return true;
}

}  // namespace

std::vector<IncompleteReason> incomplete_reasons(const TraceInfo& info) {
    std::vector<IncompleteReason> reasons;
    for (const auto& volume : info.volumes) {
        if (volume.status != JournalStatus::Ok) {
            IncompleteReason reason;
            reason.kind = IncompleteKind::Volume;
            reason.volume = volume.root;
            reason.status = volume.status;
            reasons.push_back(std::move(reason));
        }
    }
    for (const auto& diagnostic : info.registry.diagnostics) {
        IncompleteReason reason;
        reason.kind = IncompleteKind::Registry;
        reason.diagnostic = diagnostic;
        reasons.push_back(std::move(reason));
    }
    for (const auto& diagnostic : info.diagnostics) {
        if (diagnostic.code == TraceDiagnosticCode::SnapshotDamaged ||
            diagnostic.code == TraceDiagnosticCode::SavedValuesDamaged) {
            IncompleteReason reason;
            reason.kind = diagnostic.code == TraceDiagnosticCode::SnapshotDamaged
                              ? IncompleteKind::SnapshotDamaged
                              : IncompleteKind::SavedValuesDamaged;
            reasons.push_back(std::move(reason));
        }
    }
    return reasons;
}

std::string_view to_string(TraceState state) {
    switch (state) {
        case TraceState::Starting:
            return "starting";
        case TraceState::Open:
            return "open";
        case TraceState::Stopping:
            return "stopping";
        case TraceState::Complete:
            return "complete";
        case TraceState::Incomplete:
            return "incomplete";
    }
    return "unknown";
}

std::string_view to_string(TraceMode mode) {
    switch (mode) {
        case TraceMode::Manual:
            return "manual";
        case TraceMode::Launch:
            return "launch";
    }
    return "unknown";
}

std::string_view to_string(StopReason reason) {
    switch (reason) {
        case StopReason::None:
            return "none";
        case StopReason::Manual:
            return "manual";
        case StopReason::InstallerDone:
            return "installer-done";
        case StopReason::Stopped:
            return "stopped";
        case StopReason::TimedOut:
            return "timed-out";
        case StopReason::RebootPending:
            return "reboot-pending";
    }
    return "unknown";
}

std::string_view to_string(TraceDiagnosticCode code) {
    switch (code) {
        case TraceDiagnosticCode::CheckpointDamaged:
            return "checkpoint-damaged";
        case TraceDiagnosticCode::SnapshotDamaged:
            return "snapshot-damaged";
        case TraceDiagnosticCode::SavedValuesDamaged:
            return "saved-values-damaged";
        case TraceDiagnosticCode::UserNotRemapped:
            return "user-not-remapped";
    }
    return "unknown";
}

std::string_view to_string(LinkMatch match) {
    switch (match) {
        case LinkMatch::CreatedKey:
            return "created-key";
        case LinkMatch::ChangedKey:
            return "changed-key";
        case LinkMatch::ProductCode:
            return "product-code";
        case LinkMatch::Manual:
            return "manual";
    }
    return "unknown";
}

Json to_json(const TraceLink& link) {
    Json json;
    put_text(json, "program_id", link.program_id);
    put_text(json, "name", link.name);
    put_text(json, "publisher", link.publisher);
    put_text(json, "version", link.version);
    json["uninstall_key"] = link.uninstall_key ? key_json(*link.uninstall_key) : Json(nullptr);
    put_text(json, "product_code", link.product_code);
    put_text(json, "install_location", link.install_location);
    json["matched_by"] = to_string(link.matched_by);
    json["primary"] = link.primary;
    return json;
}

bool from_json(const Json& json, TraceLink& link) {
    TraceLink parsed;
    const auto id = json.is_object() ? get_text(json, "program_id") : std::nullopt;
    if (!id || !get_enum(json, "matched_by", kLinkMatches, parsed.matched_by) ||
        !get_bool(json, "primary", parsed.primary)) {
        return false;
    }
    if (const auto key = json.find("uninstall_key"); key != json.end() && !key->is_null()) {
        if (!key_from(*key, parsed.uninstall_key.emplace())) {
            return false;
        }
    }
    parsed.program_id = *id;
    parsed.name = get_text(json, "name").value_or(L"");
    parsed.publisher = get_text(json, "publisher").value_or(L"");
    parsed.version = get_text(json, "version").value_or(L"");
    parsed.product_code = get_text(json, "product_code").value_or(L"");
    parsed.install_location = get_text(json, "install_location").value_or(L"");
    link = std::move(parsed);
    return true;
}

Json to_json(const TraceChange& change) {
    Json json;
    json["change"] = to_string(change.change);
    json["location"] = changes::to_json(change.location);
    if (!change.old_path.empty()) {
        put_text(json, "old_path", change.old_path);
    }
    if (change.contents != 0 || (change.change == ChangeKind::Created && is_container(change.location))) {
        json["contents"] = change.contents;
    }
    if (change.inside) {
        Json& inside = json["inside"] = Json::array();
        for (const auto& item : *change.inside) {
            Json entry;
            put_text(entry, "path", item.path);
            if (item.value) {
                put_text(entry, "value", *item.value);
            }
            if (item.container) {
                entry["container"] = true;
            }
            inside.push_back(std::move(entry));
        }
    }
    if (change.replaced) {
        json["replaced"] = true;
    }
    json["noise"] = to_string(change.noise);
    return json;
}

// A created container's inventory ("inside"); absent in traces that kept only the count.
static bool inside_from_json(const Json& json, std::optional<std::vector<leftovers::ContainedItem>>& inside) {
    const auto found = json.find("inside");
    if (found == json.end() || found->is_null()) {
        return true;
    }
    if (!found->is_array()) {
        return false;
    }
    std::vector<leftovers::ContainedItem> items;
    items.reserve(found->size());
    for (const auto& entry : *found) {
        leftovers::ContainedItem item;
        auto path = entry.is_object() ? get_text(entry, "path") : std::nullopt;
        if (!path || !get_bool(entry, "container", item.container)) {
            return false;
        }
        item.path = std::move(*path);
        item.value = get_text(entry, "value");
        if (item.value && item.container) {
            return false;
        }
        items.push_back(std::move(item));
    }
    inside = std::move(items);
    return true;
}

bool from_json(const Json& json, TraceChange& change) {
    TraceChange parsed;
    const auto location = json.is_object() ? json.find("location") : json.end();
    if (!json.is_object() || !get_enum(json, "change", kChanges, parsed.change) || location == json.end() ||
        !changes::from_json(*location, parsed.location) || !get_number(json, "contents", parsed.contents) ||
        !get_bool(json, "replaced", parsed.replaced) || !get_enum(json, "noise", kNoise, parsed.noise) ||
        !inside_from_json(json, parsed.inside)) {
        return false;
    }
    parsed.old_path = get_text(json, "old_path").value_or(L"");
    change = std::move(parsed);
    return true;
}

Json to_json(const TraceInfo& info) {
    Json json;
    json["schema_version"] = kTraceSchemaVersion;
    put_text(json, "id", info.id);
    json["state"] = to_string(info.state);
    const auto reasons = incomplete_reasons(info);
    json["complete"] = info.state == TraceState::Complete;
    json["incomplete_reasons"] = Json::array();
    for (const auto& reason : reasons) {
        json["incomplete_reasons"].push_back(to_json(reason));
    }
    json["mode"] = to_string(info.mode);
    put_text(json, "description", info.description);
    put_text(json, "command", info.command);
    json["started"] = platform::iso_time(info.started);
    json["started_unix"] = platform::unix_seconds(info.started);
    json["stopped"] = time_json(info.stopped);
    json["stopped_unix"] = unix_json(info.stopped);
    json["trace_start"] = info.trace_start;
    json["stop_reason"] =
        info.stop_reason == StopReason::None ? Json(nullptr) : Json(to_string(info.stop_reason));
    json["volumes"] = Json::array();
    for (const auto& volume : info.volumes) {
        json["volumes"].push_back(volume_json(volume));
    }
    json["registry"] = registry_json(info.registry);
    if (info.watch) {
        json["watch"] = watch_json(*info.watch);
    }
    json["counts"] = {{"changes", info.counts.changes}, {"noise", info.counts.noise}};
    json["diagnostics"] = Json::array();
    for (const auto& diagnostic : info.diagnostics) {
        Json item;
        item["code"] = to_string(diagnostic.code);
        if (!diagnostic.subject.empty()) {
            put_text(item, "subject", diagnostic.subject);
        }
        json["diagnostics"].push_back(std::move(item));
    }
    json["links"] = Json::array();
    for (const auto& link : info.links) {
        json["links"].push_back(to_json(link));
    }
    json["unresolved_since"] = time_json(info.unresolved_since);
    json["unresolved_since_unix"] = unix_json(info.unresolved_since);
    json["imported"] = info.imported ? import_json(*info.imported) : Json(nullptr);
    return json;
}

bool from_json(const Json& json, TraceInfo& info) {
    if (!json.is_object() || json.value("schema_version", 0) != kTraceSchemaVersion) {
        return false;
    }
    TraceInfo parsed;
    const auto id = get_text(json, "id");
    std::int64_t started = 0;
    if (!id || !get_enum(json, "state", kStates, parsed.state) ||
        !get_enum(json, "mode", kModes, parsed.mode) || !get_number(json, "started_unix", started, false) ||
        !get_number(json, "trace_start", parsed.trace_start)) {
        return false;
    }
    parsed.id = *id;
    parsed.description = get_text(json, "description").value_or(L"");
    parsed.command = get_text(json, "command").value_or(L"");
    parsed.started = platform::from_unix(started);
    if (const auto stopped = json.find("stopped_unix"); stopped != json.end() && !stopped->is_null()) {
        if (!stopped->is_number_integer()) {
            return false;
        }
        parsed.stopped = platform::from_unix(stopped->get<std::int64_t>());
    }
    if (const auto reason = json.find("stop_reason");
        reason != json.end() && !reason->is_null() &&
        !get_enum(json, "stop_reason", kStopReasons, parsed.stop_reason)) {
        return false;
    }
    if (const Json* volumes = get_array(json, "volumes")) {
        for (const auto& item : *volumes) {
            if (!volume_from(item, parsed.volumes.emplace_back())) {
                return false;
            }
        }
    }
    if (const auto registry = json.find("registry");
        registry != json.end() && !registry_from(*registry, parsed.registry)) {
        return false;
    }
    if (const auto watch = json.find("watch"); watch != json.end() && !watch->is_null()) {
        if (!watch_from(*watch, parsed.watch.emplace())) {
            return false;
        }
    }
    if (const auto counts = json.find("counts"); counts != json.end()) {
        if (!counts->is_object() || !get_number(*counts, "changes", parsed.counts.changes) ||
            !get_number(*counts, "noise", parsed.counts.noise)) {
            return false;
        }
    }
    if (const Json* diagnostics = get_array(json, "diagnostics")) {
        for (const auto& item : *diagnostics) {
            TraceDiagnostic diagnostic;
            if (!item.is_object() || !get_enum(item, "code", kDiagnostics, diagnostic.code)) {
                return false;
            }
            diagnostic.subject = get_text(item, "subject").value_or(L"");
            parsed.diagnostics.push_back(std::move(diagnostic));
        }
    }
    // 5b; a trace.json from 5a has none of these.
    if (const Json* links = get_array(json, "links")) {
        for (const auto& item : *links) {
            if (!from_json(item, parsed.links.emplace_back())) {
                return false;
            }
        }
    }
    if (!time_from(json, "unresolved_since", parsed.unresolved_since)) {
        return false;
    }
    if (const auto imported = json.find("imported"); imported != json.end() && !imported->is_null()) {
        if (!import_from(*imported, parsed.imported.emplace())) {
            return false;
        }
    }
    info = std::move(parsed);
    return true;
}

Json to_json(const IncompleteReason& reason) {
    Json json;
    switch (reason.kind) {
        case IncompleteKind::Volume:
            json["volume"] = platform::to_utf8(reason.volume);
            json["status"] = to_string(reason.status);
            break;
        case IncompleteKind::Registry:
            json["registry"] = to_string(reason.diagnostic.code);
            json["key"] = platform::to_utf8(platform::display_path(reason.diagnostic.key));
            json["error"] = reason.diagnostic.error;
            break;
        case IncompleteKind::SnapshotDamaged:
            json["registry"] = to_string(TraceDiagnosticCode::SnapshotDamaged);
            break;
        case IncompleteKind::SavedValuesDamaged:
            json["registry"] = to_string(TraceDiagnosticCode::SavedValuesDamaged);
            break;
    }
    return json;
}

Json to_json(const TraceResult& result) {
    Json json;
    json["changes"] = Json::array();
    for (const auto& change : result.changes) {
        json["changes"].push_back(to_json(change));
    }
    json["known_values"] = Json::array();
    for (const auto& value : result.known_values) {
        json["known_values"].push_back(to_json(value));
    }
    return json;
}

bool from_json(const Json& json, TraceResult& result) {
    const Json* changes = json.is_object() ? get_array(json, "changes") : nullptr;
    const Json* known_values = json.is_object() ? get_array(json, "known_values") : nullptr;
    if (!changes || !known_values) {
        return false;
    }
    TraceResult parsed;
    for (const auto& item : *changes) {
        if (!from_json(item, parsed.changes.emplace_back())) {
            return false;
        }
    }
    for (const auto& item : *known_values) {
        if (!from_json(item, parsed.known_values.emplace_back())) {
            return false;
        }
    }
    result = std::move(parsed);
    return true;
}

}  // namespace avel::tracker
