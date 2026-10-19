#include "cli/leftover_output.h"

#include <format>
#include <string>
#include <type_traits>

#include "cli/console.h"
#include "core/platform/text.h"

namespace avel::cli {

namespace {

using leftovers::Leftover;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

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
                return "service";
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                return "task";
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                return "package";
            } else {
                return "firewall-rule";
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
            } else {
                return item.rule_id;
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
            } else {
                json["rule_id"] = to_utf8(item.rule_id);
            }
        },
        location);
    return json;
}

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
    json["diagnostics"] = std::move(diagnostics);
    return json;
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
    for (const auto& diagnostic : result.diagnostics) {
        print_error(std::format("note: {} {}\n", leftovers::to_string(diagnostic.code), to_utf8(diagnostic.subject)));
    }
}

}  // namespace avel::cli
