#include "cli/leftover_output.h"

#include <format>
#include <string>
#include <type_traits>

#include "cli/console.h"
#include "core/changes/serialization.h"
#include "core/leftovers/value_edits.h"
#include "core/platform/text.h"

namespace avel::cli {

namespace {

using leftovers::Leftover;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

// A value edit's value: {"type", "data" (hex of the exact bytes)}, plus "text" for a string and
// "count" for a SharedDLLs count; null when absent.
Json edit_value_to_json(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return nullptr;
    }
    Json json;
    json["type"] = value->type;
    json["data"] = changes::to_hex(value->data);
    if (const auto text = leftovers::string_value_text(*value)) {
        json["text"] = to_utf8(*text);
    } else if (const auto count = leftovers::shared_dll_count(*value)) {
        json["count"] = *count;
    }
    return json;
}

// "Path: remove C:\Contoso\bin", "C:\Windows\x.dll: count 2 -> 1", ...
std::wstring describe_edit(const leftovers::ValueEditLocation& edit) {
    using leftovers::ValueEditKind;
    std::wstring text = platform::display_path(edit.key) + L" : " +
                        (edit.value_name.empty() ? L"(default)" : edit.value_name);
    switch (edit.kind) {
        case ValueEditKind::RemovePathEntries: {
            text += L" (remove ";
            for (std::size_t i = 0; i < edit.entries.size(); ++i) {
                text += (i == 0 ? L"" : L"; ") + edit.entries[i];
            }
            return text + L")";
        }
        case ValueEditKind::DecrementSharedDll:
        case ValueEditKind::DeleteValue: {
            const auto before = edit.expected ? leftovers::shared_dll_count(*edit.expected) : std::nullopt;
            const auto after = edit.desired ? leftovers::shared_dll_count(*edit.desired) : std::nullopt;
            if (before && after) {
                return text + std::format(L" (count {} -> {})", *before, *after);
            }
            return text + L" (delete)";
        }
        case ValueEditKind::RestoreValue:
            return text + L" (restore the value before the install)";
    }
    return text;
}

}  // namespace

std::string_view kind_of(const leftovers::Location& location) {
    return std::visit(
        [](const auto& item) -> std::string_view {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::FileLocation>) {
                return item.directory ? "folder" : "file";
            } else if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                return "registry-key";
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                return "registry-value";
            } else if constexpr (std::is_same_v<T, leftovers::ServiceLocation>) {
                return item.driver ? "driver" : "service";
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                return item.folder ? "task-folder" : "task";
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                return "package";
            } else if constexpr (std::is_same_v<T, leftovers::FirewallRuleLocation>) {
                return "firewall-rule";
            } else {
                static_assert(std::is_same_v<T, leftovers::ValueEditLocation>);
                return "value-edit";
            }
        },
        location);
}

std::wstring describe_location(const leftovers::Location& location) {
    return std::visit(
        [](const auto& item) -> std::wstring {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::FileLocation>) {
                return item.path;
            } else if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                return platform::display_path(item.key);
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                return platform::display_path(item.key) + L" : " + (item.value_name.empty() ? L"(default)" : item.value_name);
            } else if constexpr (std::is_same_v<T, leftovers::ServiceLocation>) {
                return item.name;
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                return item.path;
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                return item.full_name;
            } else if constexpr (std::is_same_v<T, leftovers::FirewallRuleLocation>) {
                return item.rule_id;
            } else {
                static_assert(std::is_same_v<T, leftovers::ValueEditLocation>);
                return describe_edit(item);
            }
        },
        location);
}

Json location_to_json(const leftovers::Location& location) {
    Json json;
    json["kind"] = kind_of(location);
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::FileLocation>) {
                json["path"] = to_utf8(item.path);
            } else if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                json["key"] = to_utf8(platform::display_path(item.key));
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                json["key"] = to_utf8(platform::display_path(item.key));
                json["value"] = to_utf8(item.value_name);
            } else if constexpr (std::is_same_v<T, leftovers::ServiceLocation>) {
                json["service"] = to_utf8(item.name);
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                json["task"] = to_utf8(item.path);
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                json["package_full_name"] = to_utf8(item.full_name);
                json["user_sid"] = to_utf8(item.user_sid);
            } else if constexpr (std::is_same_v<T, leftovers::FirewallRuleLocation>) {
                json["rule_id"] = to_utf8(item.rule_id);
            } else {
                static_assert(std::is_same_v<T, leftovers::ValueEditLocation>);
                json["key"] = to_utf8(platform::display_path(item.key));
                json["value"] = to_utf8(item.value_name);
                json["edit"] = leftovers::to_string(item.kind);
                json["expected"] = edit_value_to_json(item.expected);
                json["desired"] = edit_value_to_json(item.desired);
                Json entries = Json::array();
                for (const auto& entry : item.entries) {
                    entries.push_back(to_utf8(entry));
                }
                json["entries"] = std::move(entries);
                json["delta"] = item.delta;
            }
        },
        location);
    return json;
}

namespace {

std::string evidence_summary(const Leftover& item) {
    std::string text;
    for (const auto& record : item.records) {
        if (!text.empty()) {
            text += ", ";
        }
        text += leftovers::to_string(record.evidence);
        if (record.evidence == leftovers::Evidence::Depth) {
            text += std::format("({})", record.level);
        } else if (record.reference != leftovers::ReferenceKind::None) {
            text += std::format("({})", leftovers::to_string(record.reference));
        }
    }
    return text;
}

}  // namespace

Json leftovers_to_json(const leftovers::ScanResult& result, leftovers::Confidence min_level) {
    Json items = Json::array();
    for (std::size_t i = 0; i < result.leftovers.size(); ++i) {
        const auto& item = result.leftovers[i];
        const auto level = leftovers::level(item.records);
        if (level < min_level) {
            continue;
        }
        Json json;
        json["index"] = i;
        json["category"] = leftovers::to_string(item.category);
        json["location"] = location_to_json(item.location);
        json["confidence"] = leftovers::to_string(level);
        json["score"] = leftovers::score(item.records);
        json["inside"] = item.inside ? Json(*item.inside) : Json(nullptr);
        json["found_by"] = leftovers::to_string(item.found_by);
        Json programs = Json::array();
        for (const auto& id : item.owner_ids) {
            programs.push_back(to_utf8(id));
        }
        json["programs"] = std::move(programs);
        Json evidence = Json::array();
        for (const auto& record : item.records) {
            Json part{{"kind", leftovers::to_string(record.evidence)}, {"points", leftovers::points(record)}};
            if (record.reference != leftovers::ReferenceKind::None) {
                part["reference"] = leftovers::to_string(record.reference);
            }
            if (!record.detail.empty()) {
                part["detail"] = to_utf8(record.detail);
            }
            evidence.push_back(std::move(part));
        }
        json["evidence"] = std::move(evidence);
        items.push_back(std::move(json));
    }
    Json diagnostics = Json::array();
    for (const auto& diagnostic : result.diagnostics) {
        diagnostics.push_back({{"code", leftovers::to_string(diagnostic.code)},
                               {"subject", to_utf8(diagnostic.subject)},
                               {"error", diagnostic.error}});
    }
    Json json;
    json["leftovers"] = std::move(items);
    json["protected_count"] = result.protected_count;
    json["elapsed_ms"] = result.elapsed.count();
    Json timings = Json::array();
    for (const auto& timing : result.timings) {
        timings.push_back({{"stage", timing.stage}, {"elapsed_ms", timing.elapsed.count()}});
    }
    json["timings"] = std::move(timings);
    json["diagnostics"] = std::move(diagnostics);
    json["traces"] = traces_to_json(result);
    return json;
}

Json traces_to_json(const leftovers::ScanResult& result) {
    Json traces = Json::array();
    for (const auto& use : result.traces) {
        Json values = Json::array();
        for (const auto& value : use.values) {
            values.push_back({{"key", to_utf8(platform::display_path(value.key))},
                              {"name", to_utf8(value.name)},
                              {"basis", leftovers::to_string(value.basis)}});
        }
        traces.push_back({{"program_id", to_utf8(use.program_id)},
                          {"trace_id", to_utf8(use.trace_id)},
                          {"items", use.items},
                          {"tied", use.tied},
                          {"value_edits", use.value_edits},
                          {"gone", use.gone},
                          {"shown_only", use.shown_only},
                          {"protected", use.protected_count},
                          {"split", use.split},
                          {"split_into", use.split_into},
                          {"values", std::move(values)}});
    }
    return traces;
}

void print_leftovers(const leftovers::ScanResult& result, leftovers::Confidence min_level) {
    std::size_t shown = 0;
    for (const auto& item : result.leftovers) {
        const auto level = leftovers::level(item.records);
        if (level < min_level) {
            continue;
        }
        ++shown;
        int depth = 0;
        for (auto parent = item.inside; parent; parent = result.leftovers[*parent].inside) {
            ++depth;
        }
        print(std::format("  {:<12} {:>3}  {:<14} {}{}\n      {}\n", leftovers::to_string(level),
                          leftovers::score(item.records), kind_of(item.location), std::string(depth * 2, ' '),
                          to_utf8(describe_location(item.location)), evidence_summary(item)));
    }
    const std::size_t hidden = result.leftovers.size() - shown;
    print(std::format("\n{} leftover{}", shown, shown == 1 ? "" : "s"));
    if (hidden > 0) {
        print(std::format(" ({} less likely one{} hidden; see --all-levels)", hidden, hidden == 1 ? "" : "s"));
    }
    print(std::format(", {} protected match{} skipped, {} ms\n", result.protected_count,
                      result.protected_count == 1 ? "" : "es", result.elapsed.count()));
    const auto plural = [](std::size_t count) { return count == 1 ? "" : "s"; };
    for (const auto& use : result.traces) {
        std::string line =
            std::format("From install trace {}: {} item{} ({} tied to the program), {} value edit{}",
                        to_utf8(use.trace_id), use.items, plural(use.items), use.tied, use.value_edits,
                        plural(use.value_edits));
        if (use.gone > 0) {
            line += std::format("; {} already gone", use.gone);
        }
        if (use.protected_count > 0) {
            line += std::format("; {} protected", use.protected_count);
        }
        if (use.shown_only > 0) {
            line += std::format("; {} change{} to what existed before, never offered", use.shown_only,
                                plural(use.shown_only));
        }
        if (use.split > 0) {
            line += std::format("; {} folder{} or key{} holding more than it saw, offered as the {} item{} "
                                "it saw in them",
                                use.split, plural(use.split), plural(use.split), use.split_into,
                                plural(use.split_into));
        }
        print(line + "\n");
    }
    for (const auto& diagnostic : result.diagnostics) {
        print_error(std::format("note: {} {}\n", leftovers::to_string(diagnostic.code), to_utf8(diagnostic.subject)));
    }
}

}  // namespace avel::cli
