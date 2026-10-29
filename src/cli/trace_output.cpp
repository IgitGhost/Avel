#include "cli/trace_output.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>

#include "cli/console.h"
#include "cli/leftover_output.h"
#include "core/changes/serialization.h"
#include "core/platform/record_folders.h"
#include "core/platform/registry_address.h"
#include "core/platform/text.h"
#include "core/tracker/trace_store.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using Json = nlohmann::ordered_json;
using platform::to_utf8;
using tracker::ChangeKind;
using tracker::NoiseReason;
using tracker::StopReason;
using tracker::TraceInfo;
using tracker::TraceState;

bool is_finished(TraceState state) {
    return state == TraceState::Complete || state == TraceState::Incomplete;
}

bool is_container(const leftovers::Location& location) {
    const auto* file = std::get_if<leftovers::FileLocation>(&location);
    return (file && file->directory) || std::holds_alternative<leftovers::RegistryKeyLocation>(location);
}

std::string plural(std::uint64_t count) { return count == 1 ? "" : "s"; }

// A string value's text: REG_SZ and REG_EXPAND_SZ as they are, REG_MULTI_SZ's strings joined with
// '\n'; trailing NULs dropped. nullopt for the other types.
std::optional<std::wstring> value_text(const platform::RawValue& value) {
    if (value.type != REG_SZ && value.type != REG_EXPAND_SZ && value.type != REG_MULTI_SZ) {
        return std::nullopt;
    }
    std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
    if (!text.empty()) {
        std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
    }
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    if (value.type == REG_MULTI_SZ) {
        std::replace(text.begin(), text.end(), L'\0', L'\n');
    }
    return text;
}

// {"type", "data" (hex of the exact bytes)}, or null when the value didn't exist.
Json value_to_json(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return nullptr;
    }
    Json json;
    json["type"] = value->type;
    json["data"] = changes::to_hex(value->data);
    return json;
}

// A value on one line, for people: strings as text (a REG_MULTI_SZ's separated by " | "), numbers
// in decimal and hex, anything else as its first bytes in hex.
std::string describe_value(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return "(none)";
    }
    if (auto text = value_text(*value)) {
        std::string line;
        std::size_t begin = 0;
        for (std::size_t end = text->find(L'\n'); end != std::wstring::npos; end = text->find(L'\n', begin)) {
            line += to_utf8(std::wstring_view(*text).substr(begin, end - begin)) + " | ";
            begin = end + 1;
        }
        return line + to_utf8(std::wstring_view(*text).substr(begin));
    }
    if ((value->type == REG_DWORD && value->data.size() == 4) ||
        (value->type == REG_QWORD && value->data.size() == 8)) {
        std::uint64_t number = 0;
        std::memcpy(&number, value->data.data(), value->data.size());
        return std::format("{} (0x{:x})", number, number);
    }
    constexpr std::size_t kShown = 32;
    return std::format("type {}, {} bytes: {}{}", value->type, value->data.size(),
                       changes::to_hex(value->data.data(), std::min(value->data.size(), kShown)),
                       value->data.size() > kShown ? "..." : "");
}

// The tying of change `index`, if it was tied at all (noise isn't).
const tracker::TiedItem* tie_of(const std::vector<tracker::TiedItem>* tied, std::size_t index) {
    if (!tied) {
        return nullptr;
    }
    const auto found =
        std::lower_bound(tied->begin(), tied->end(), index,
                         [](const tracker::TiedItem& item, std::size_t i) { return item.change < i; });
    return found != tied->end() && found->change == index ? &*found : nullptr;
}

Json link_to_json(const tracker::TraceLink& link) {
    Json json;
    json["program_id"] = to_utf8(link.program_id);
    json["name"] = to_utf8(link.name);
    json["publisher"] = to_utf8(link.publisher);
    json["version"] = to_utf8(link.version);
    json["uninstall_key"] =
        link.uninstall_key ? Json(to_utf8(platform::display_path(*link.uninstall_key))) : Json(nullptr);
    json["product_code"] = to_utf8(link.product_code);
    json["install_location"] = to_utf8(link.install_location);
    json["matched_by"] = tracker::to_string(link.matched_by);
    json["primary"] = link.primary;
    return json;
}

// "7-Zip 24.08 (x64) (24.08, Igor Pavlov) [created-key]".
std::string describe_link(const tracker::TraceLink& link) {
    std::string text = link.name.empty() ? to_utf8(link.program_id) : to_utf8(link.name);
    std::string details = to_utf8(link.version);
    if (!link.publisher.empty()) {
        details += (details.empty() ? "" : ", ") + to_utf8(link.publisher);
    }
    if (!details.empty()) {
        text += " (" + details + ")";
    }
    return text + std::format(" [{}]", tracker::to_string(link.matched_by));
}

Json change_to_json(const tracker::TraceChange& change, const tracker::TiedItem* tie) {
    Json json;
    json["change"] = tracker::to_string(change.change);
    json["location"] = location_to_json(change.location);
    if (!change.old_path.empty()) {
        json["old_path"] = to_utf8(change.old_path);
    }
    if (change.contents != 0 || (change.change == ChangeKind::Created && is_container(change.location))) {
        json["contents"] = change.contents;
    }
    if (change.replaced) {
        json["replaced"] = true;
    }
    json["noise"] = tracker::to_string(change.noise);
    if (tie) {
        json["tied"] = tie->tied;
        json["tie"] = tracker::to_string(tie->reason);
    }
    return json;
}

Json known_value_to_json(const tracker::KnownValueChange& change) {
    Json json;
    json["spot"] = tracker::to_string(change.spot);
    json["key"] = to_utf8(platform::display_path(change.key));
    json["name"] = to_utf8(change.name);
    json["before"] = value_to_json(change.before);
    json["after"] = value_to_json(change.after);
    for (const auto& [field, value] :
         {std::pair{"before_text", &change.before}, std::pair{"after_text", &change.after}}) {
        if (*value) {
            if (const auto text = value_text(**value)) {
                json[field] = to_utf8(*text);
            }
        }
    }
    return json;
}

std::string duration_text(std::chrono::system_clock::duration duration) {
    const auto seconds =
        std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(duration).count());
    if (seconds < 60) {
        return std::format("{} s", seconds);
    }
    if (seconds < 3600) {
        return std::format("{} min {} s", seconds / 60, seconds % 60);
    }
    return std::format("{} h {} min", seconds / 3600, seconds % 3600 / 60);
}

std::string describe_reason(const tracker::IncompleteReason& reason) {
    switch (reason.kind) {
        case tracker::IncompleteKind::Volume:
            return std::format("volume {}: {}", to_utf8(reason.volume), tracker::to_string(reason.status));
        case tracker::IncompleteKind::Registry: {
            std::string text = std::format("registry: {} {}", tracker::to_string(reason.diagnostic.code),
                                           to_utf8(platform::display_path(reason.diagnostic.key)));
            if (reason.diagnostic.error != 0) {
                text += std::format(" (error {})", reason.diagnostic.error);
            }
            return text;
        }
        case tracker::IncompleteKind::SnapshotDamaged:
            return "registry: the snapshot taken at the start is damaged, so the registry wasn't compared";
        case tracker::IncompleteKind::SavedValuesDamaged:
            return "known values: the values saved at the start are damaged, so they weren't compared";
    }
    return "unknown";
}

}  // namespace

Json trace_to_json(const TraceInfo& info) {
    Json json;
    json["id"] = to_utf8(info.id);
    json["state"] = tracker::to_string(info.state);
    json["complete"] = info.state == TraceState::Complete;
    json["incomplete_reasons"] = Json::array();
    for (const auto& reason : tracker::incomplete_reasons(info)) {
        json["incomplete_reasons"].push_back(tracker::to_json(reason));
    }
    json["mode"] = tracker::to_string(info.mode);
    json["description"] = to_utf8(info.description);
    json["command"] = to_utf8(info.command);
    json["started"] = platform::iso_time(info.started);
    json["stopped"] = info.stopped ? Json(platform::iso_time(*info.stopped)) : Json(nullptr);
    json["stop_reason"] =
        info.stop_reason == StopReason::None ? Json(nullptr) : Json(tracker::to_string(info.stop_reason));
    json["volumes"] = Json::array();
    for (const auto& volume : info.volumes) {
        Json item;
        item["root"] = to_utf8(volume.root);
        item["status"] = tracker::to_string(volume.status);
        item["created_journal"] = volume.created_journal;
        json["volumes"].push_back(std::move(item));
    }
    json["registry"] = {{"keys", info.registry.keys},
                        {"values", info.registry.values},
                        {"snapshot_ms", info.registry.snapshot_ms},
                        {"diff_ms", info.registry.diff_ms}};
    if (info.watch) {
        Json watch;
        watch["end"] = tracker::to_string(info.watch->end);
        watch["exit_code"] = info.watch->exit_code;
        watch["reboot_requested"] = info.watch->reboot_requested;
        watch["left_running"] = Json::array();
        for (const auto& process : info.watch->left_running) {
            Json item;
            item["pid"] = process.pid;
            item["image"] = to_utf8(process.image);
            watch["left_running"].push_back(std::move(item));
        }
        json["watch"] = std::move(watch);
    }
    json["counts"] = {{"changes", info.counts.changes}, {"noise", info.counts.noise}};
    json["links"] = Json::array();
    for (const auto& link : info.links) {
        json["links"].push_back(link_to_json(link));
    }
    json["unresolved_since"] =
        info.unresolved_since ? Json(platform::iso_time(*info.unresolved_since)) : Json(nullptr);
    if (info.imported) {
        const auto& imported = *info.imported;
        Json item;
        item["time"] = platform::iso_time(imported.time);
        item["origin_id"] = to_utf8(imported.origin_id);
        item["remapped"] = imported.from_sid.empty() ? Json(nullptr)
                                                     : Json{{"from_sid", to_utf8(imported.from_sid)},
                                                            {"from_profile", to_utf8(imported.from_profile)},
                                                            {"to_sid", to_utf8(imported.to_sid)},
                                                            {"to_profile", to_utf8(imported.to_profile)}};
        item["not_remapped"] = Json::array();
        for (const auto& diagnostic : info.diagnostics) {
            if (diagnostic.code == tracker::TraceDiagnosticCode::UserNotRemapped) {
                item["not_remapped"].push_back(to_utf8(diagnostic.subject));
            }
        }
        json["imported"] = std::move(item);
    } else {
        json["imported"] = nullptr;
    }
    return json;
}

Json trace_root_to_json(const TraceInfo& info) {
    Json root;
    root["avel_version"] = kVersionString;
    root["trace"] = trace_to_json(info);
    return root;
}

Json trace_result_to_json(const TraceInfo& info, const tracker::TraceResult& result, bool all,
                          const std::vector<tracker::TiedItem>* tied) {
    Json root = trace_root_to_json(info);
    root["changes"] = Json::array();
    for (std::size_t i = 0; i < result.changes.size(); ++i) {
        const auto& change = result.changes[i];
        if (all || change.noise == NoiseReason::None) {
            root["changes"].push_back(change_to_json(change, tie_of(tied, i)));
        }
    }
    root["known_values"] = Json::array();
    for (const auto& value : result.known_values) {
        root["known_values"].push_back(known_value_to_json(value));
    }
    return root;
}

void print_trace_header(const TraceInfo& info) {
    std::string text = std::format("Trace {}: {}", to_utf8(info.id), tracker::to_string(info.state));
    if (info.stop_reason != StopReason::None) {
        text += std::format(" ({})", tracker::to_string(info.stop_reason));
    }
    text += '\n';
    if (!info.description.empty()) {
        text += std::format("  Description:  {}\n", to_utf8(info.description));
    }
    text += std::format("  Mode:         {}\n", tracker::to_string(info.mode));
    if (!info.command.empty()) {
        text += std::format("  Command:      {}\n", to_utf8(info.command));
    }
    text += "  Started:      " + platform::iso_time(info.started);
    if (info.stopped) {
        text += std::format(", stopped {} after {}", platform::iso_time(*info.stopped),
                            duration_text(*info.stopped - info.started));
    }
    text += "\n  Volumes:      ";
    if (info.volumes.empty()) {
        text += "none";
    }
    for (std::size_t i = 0; i < info.volumes.size(); ++i) {
        const auto& volume = info.volumes[i];
        text += std::format("{}{} {}{}", i == 0 ? "" : ", ", to_utf8(volume.root),
                            tracker::to_string(volume.status),
                            volume.created_journal ? " (journal created)" : "");
    }
    text += std::format("\n  Registry:     {} keys, {} values; snapshot {} ms", info.registry.keys,
                        info.registry.values, info.registry.snapshot_ms);
    if (is_finished(info.state)) {
        text += std::format(", comparison {} ms", info.registry.diff_ms);
    }
    if (const auto unreadable = info.registry.diagnostics.size(); unreadable != 0) {
        text += std::format("; {} part{} couldn't be read", unreadable, plural(unreadable));
    }
    text += '\n';
    if (info.watch) {
        const auto& watch = *info.watch;
        text += std::format("  Installer:    {}", tracker::to_string(watch.end));
        if (watch.end == tracker::WatchEnd::InstallerDone || watch.reboot_requested) {
            text += std::format(", exit code {}", watch.exit_code);
        }
        if (watch.reboot_requested) {
            text += ", asked for a restart";
        }
        text += '\n';
        if (!watch.left_running.empty()) {
            text += "  Left running:\n";
            for (const auto& process : watch.left_running) {
                text +=
                    std::format("    {:>6}  {}\n", process.pid,
                                process.image.empty() ? std::string("(unknown)") : to_utf8(process.image));
            }
        }
    }
    if (info.imported) {
        const auto& imported = *info.imported;
        text += std::format("  Imported:     {}, recorded as {}", platform::iso_time(imported.time),
                            to_utf8(imported.origin_id));
        if (!imported.from_sid.empty()) {
            text += std::format("; {} ({}) moved to {} ({})", to_utf8(imported.from_sid),
                                to_utf8(imported.from_profile), to_utf8(imported.to_sid),
                                to_utf8(imported.to_profile));
        }
        text += '\n';
        for (const auto& diagnostic : info.diagnostics) {
            if (diagnostic.code == tracker::TraceDiagnosticCode::UserNotRemapped) {
                text += std::format("                user {}'s items left as they were\n",
                                    to_utf8(diagnostic.subject));
            }
        }
    }
    if (is_finished(info.state)) {
        if (info.links.empty()) {
            text += "  Linked to:    nothing (`avel-cli trace link` links it by hand)\n";
        }
        for (std::size_t i = 0; i < info.links.size(); ++i) {
            text += std::format("  {:<14}{}\n", i == 0 ? "Linked to:" : "", describe_link(info.links[i]));
        }
        if (info.unresolved_since) {
            text +=
                std::format("  Uninstalled:  since {}; the trace is deleted {} days after\n",
                            platform::iso_time(*info.unresolved_since),
                            std::chrono::duration_cast<std::chrono::days>(tracker::kTraceRetention).count());
        }
    }
    print(text);
}

void print_trace_changes(const TraceInfo& info, const tracker::TraceResult& result, bool all,
                         const std::vector<tracker::TiedItem>* tied) {
    constexpr std::array kGroups{
        std::pair{ChangeKind::Created, "Created"}, std::pair{ChangeKind::Modified, "Modified"},
        std::pair{ChangeKind::Deleted, "Deleted"}, std::pair{ChangeKind::Renamed, "Renamed"}};
    std::string text;
    std::size_t tied_count = 0;
    for (const auto& [kind, heading] : kGroups) {
        std::string lines;
        std::size_t count = 0;
        for (std::size_t i = 0; i < result.changes.size(); ++i) {
            const auto& change = result.changes[i];
            if (change.change != kind || (!all && change.noise != NoiseReason::None)) {
                continue;
            }
            ++count;
            lines += std::format("  {:<15} {}", kind_of(change.location),
                                 to_utf8(describe_location(change.location)));
            if (!change.old_path.empty()) {
                lines += std::format(" (was {})", to_utf8(change.old_path));
            }
            if (change.contents != 0) {
                lines += std::format(" ({} inside)", change.contents);
            }
            if (change.replaced) {
                lines += " (replaced)";
            }
            if (change.noise != NoiseReason::None) {
                lines += std::format(" [noise: {}]", tracker::to_string(change.noise));
            }
            if (const auto* tie = tie_of(tied, i); tie && tie->tied) {
                lines += std::format(" [tied: {}]", tracker::to_string(tie->reason));
                ++tied_count;
            }
            lines += '\n';
        }
        if (count != 0) {
            text += std::format("\n{} ({}):\n{}", heading, count, lines);
        }
    }
    if (!result.known_values.empty()) {
        text += std::format("\nKnown values ({}):\n", result.known_values.size());
        for (const auto& value : result.known_values) {
            text += std::format("  {:<19} {} : {}\n      before: {}\n      after:  {}\n",
                                tracker::to_string(value.spot), to_utf8(platform::display_path(value.key)),
                                value.name.empty() ? std::string("(default)") : to_utf8(value.name),
                                describe_value(value.before), describe_value(value.after));
        }
    }
    text += std::format("\n{} change{}, {} set aside as noise", info.counts.changes,
                        plural(info.counts.changes), info.counts.noise);
    if (!all && info.counts.noise != 0) {
        text += std::format(" (shown by `avel-cli trace show {} --all`)", to_utf8(info.id));
    }
    text += '\n';
    if (tied) {
        text += std::format("{} tied to the program\n", tied_count);
    }
    print(text);
}

void print_trace_list(const std::vector<TraceInfo>& traces) {
    if (traces.empty()) {
        print("No traces.\n");
        return;
    }
    std::string text;
    for (const auto& info : traces) {
        const std::string counts = is_finished(info.state) ? std::format("{} change{}", info.counts.changes,
                                                                         plural(info.counts.changes))
                                                           : "";
        std::string line =
            std::format("{}  {:<10}  {:<6}  {}  {:>12}  {}", to_utf8(info.id), tracker::to_string(info.state),
                        tracker::to_string(info.mode), platform::iso_time(info.started), counts,
                        to_utf8(info.description.empty() ? info.command : info.description));
        line.erase(line.find_last_not_of(' ') + 1);
        if (!info.links.empty()) {
            line += " -> " + to_utf8(info.links.front().name.empty() ? info.links.front().program_id
                                                                     : info.links.front().name);
            if (info.unresolved_since) {
                line += " (uninstalled)";
            }
        }
        if (info.imported) {
            line += " (imported)";
        }
        text += line + '\n';
    }
    print(text);
}

void warn_if_incomplete(const TraceInfo& info) {
    if (info.state != TraceState::Incomplete) {
        return;
    }
    const auto reasons = tracker::incomplete_reasons(info);
    std::string text = "warning: the trace is incomplete:\n";
    constexpr std::size_t kShown = 10;
    for (std::size_t i = 0; i < reasons.size() && i < kShown; ++i) {
        text += "  " + describe_reason(reasons[i]) + '\n';
    }
    if (reasons.size() > kShown) {
        text += std::format("  ... and {} more\n", reasons.size() - kShown);
    }
    print_error(text);
}

}  // namespace avel::cli
