#include "core/tools/browser_extensions_changes.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <utility>

#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::tools {

namespace {

using Json = nlohmann::ordered_json;
using leftovers::Location;
using leftovers::ValueEditKind;
using leftovers::ValueEditLocation;
using platform::RawValue;

bool is_list(ExtensionSourceKind kind) {
    return kind == ExtensionSourceKind::ForceList || kind == ExtensionSourceKind::FirefoxInstall ||
           kind == ExtensionSourceKind::FirefoxLocked;
}

bool is_json_settings(const ExtensionSource& source) {
    return source.kind == ExtensionSourceKind::Settings && !source.settings_subkey;
}

bool same_value(const RawValue& a, const RawValue& b) { return a.type == b.type && a.data == b.data; }

std::optional<std::uint32_t> list_index(std::wstring_view name) {
    if (name.empty() || name.size() > 9 || name.front() == L'0' ||
        !std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::wcstoul(std::wstring(name).c_str(), nullptr, 10));
}

std::wstring key_identity(const platform::RegistryAddress& key) {
    return platform::to_lower(platform::display_path(key));
}

// What a source's removal changes, for the protection check before planning: the value or key it
// lives in, as the plan will name it.
Location probe(const ExtensionSource& source) {
    if (is_list(source.kind) || is_json_settings(source)) {
        return ValueEditLocation{source.key,
                                 source.value_name,
                                 ValueEditKind::RestoreValue,
                                 source.value,
                                 is_json_settings(source) ? source.value : std::optional<RawValue>(),
                                 {},
                                 0};
    }
    if (source.kind == ExtensionSourceKind::FirefoxRegistry) {
        return leftovers::RegistryValueLocation{source.key, source.value_name};
    }
    return leftovers::RegistryKeyLocation{source.key};
}

std::wstring text_of(const RawValue& value) {
    std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    if (value.type == REG_MULTI_SZ) {
        std::replace(text.begin(), text.end(), L'\0', L'\n');
    }
    return text;
}

// `text` stored as `type` was: a string with its terminator, or a one-string REG_MULTI_SZ.
RawValue string_value(const std::wstring& name, DWORD type, const std::wstring& text) {
    const std::size_t terminators = type == REG_MULTI_SZ ? 2 : 1;
    RawValue value{name, type, std::vector<std::byte>((text.size() + terminators) * sizeof(wchar_t))};
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

// The renumbered list: `removed` (value names) taken out of the run "1".."k" Chromium reads, the values
// after them moved down; removed values past a gap simply deleted. One edit per value that changes.
void plan_list(const std::vector<RawValue>& values, const std::set<std::wstring>& removed,
               const platform::RegistryAddress& key, std::vector<ValueEditLocation>& edits) {
    std::vector<const RawValue*> run;
    for (const auto& value : values) {
        if (list_index(value.name) != run.size() + 1) {
            break;
        }
        run.push_back(&value);
    }
    std::vector<const RawValue*> kept;
    for (const RawValue* value : run) {
        if (!removed.contains(platform::to_lower(value->name))) {
            kept.push_back(value);
        }
    }
    for (std::size_t i = 0; i < run.size(); ++i) {
        const std::wstring name = std::to_wstring(i + 1);
        std::optional<RawValue> desired;
        if (i < kept.size()) {
            desired = RawValue{name, kept[i]->type, kept[i]->data};
        }
        if (desired && same_value(*desired, *run[i])) {
            continue;
        }
        edits.push_back({key, name, ValueEditKind::RestoreValue, *run[i], std::move(desired), {}, 0});
    }
    for (std::size_t i = run.size(); i < values.size(); ++i) {
        if (removed.contains(platform::to_lower(values[i].name))) {
            edits.push_back(
                {key, values[i].name, ValueEditKind::RestoreValue, values[i], std::nullopt, {}, 0});
        }
    }
}

// The ExtensionSettings value without the removed ids (lowercase for Chromium); nullopt when nothing
// changes or the value can't be read as JSON. An empty result deletes the value.
std::optional<std::optional<RawValue>> plan_settings(const RawValue& value,
                                                     const std::set<std::wstring>& removed, bool chromium) {
    const Json json = Json::parse(platform::to_utf8(text_of(value)), nullptr, false, true);
    if (!json.is_object()) {
        return std::nullopt;
    }
    Json result = Json::object();
    bool changed = false;
    for (const auto& [name, settings] : json.items()) {
        const std::wstring key = platform::to_utf16(name);
        std::vector<std::wstring> kept;
        std::size_t start = 0;
        bool dropped = false;
        while (start <= key.size()) {
            std::size_t end = chromium ? key.find(L',', start) : std::wstring::npos;
            if (end == std::wstring::npos) {
                end = key.size();
            }
            const std::wstring id(platform::trim(std::wstring_view(key).substr(start, end - start)));
            if (!id.empty() && removed.contains(chromium ? platform::to_lower(id) : id)) {
                dropped = true;
            } else if (!id.empty()) {
                kept.push_back(id);
            }
            start = end + 1;
        }
        if (!dropped) {
            result[name] = settings;
            continue;
        }
        changed = true;
        if (!kept.empty()) {
            std::wstring joined;
            for (const auto& id : kept) {
                joined += (joined.empty() ? L"" : L",") + id;
            }
            result[platform::to_utf8(joined)] = settings;
        }
    }
    if (!changed) {
        return std::nullopt;
    }
    if (result.empty()) {
        return std::optional<std::optional<RawValue>>(std::in_place, std::nullopt);
    }
    const std::wstring text = platform::to_utf16(result.dump(-1, ' ', false, Json::error_handler_t::replace));
    return std::optional<std::optional<RawValue>>(std::in_place, string_value(value.name, value.type, text));
}

changes::PlannedItem planned(Location location, const std::wstring& program) {
    changes::PlannedItem item;
    item.category = leftovers::category_of(location);
    item.location = std::move(location);
    if (!program.empty()) {
        item.programs.push_back(program);
    }
    // No evidence was scored: the user chose the item (the executor doesn't read these).
    item.confidence = leftovers::Confidence::Unknown;
    item.score = 0;
    return item;
}

std::optional<ExtensionPlanIssueCode> issue_of(ExtensionRemoval removal) {
    switch (removal) {
        case ExtensionRemoval::Removable:
            return std::nullopt;
        case ExtensionRemoval::NotRemovable:
            return ExtensionPlanIssueCode::NotRemovable;
        case ExtensionRemoval::ManagedByPoliciesJson:
            return ExtensionPlanIssueCode::ManagedByPoliciesJson;
        case ExtensionRemoval::NotSupported:
            return ExtensionPlanIssueCode::NotSupported;
    }
    return ExtensionPlanIssueCode::NotRemovable;
}

std::wstring label(const BrowserExtension& entry) {
    return entry.name.empty() ? entry.extension_id : entry.name;
}

}  // namespace

ExtensionsPlan plan_remove_extensions(
    const BrowserExtensionsList& list, std::span<const std::wstring> ids,
    const std::function<leftovers::ProtectionReason(const leftovers::Location&)>& protection) {
    ExtensionsPlan result;
    std::vector<std::size_t> chosen;  // Entry indexes
    const auto issue = [&](const BrowserExtension& entry) -> std::optional<ExtensionPlanIssueCode> {
        if (const auto code = issue_of(entry.removal)) {
            return code;
        }
        if (protection) {
            for (const std::size_t s : entry.sources) {
                if (protection(probe(list.sources[s])) != leftovers::ProtectionReason::None) {
                    return ExtensionPlanIssueCode::Protected;
                }
            }
        }
        return std::nullopt;
    };
    for (const auto& id : ids) {
        std::vector<std::size_t> matches;
        for (std::size_t i = 0; i < list.extensions.size(); ++i) {
            if (platform::iequals(list.extensions[i].id, id)) {
                matches = {i};
                break;
            }
            if (!list.extensions[i].extension_id.empty() &&
                platform::iequals(list.extensions[i].extension_id, id)) {
                matches.push_back(i);
            }
        }
        if (matches.empty()) {
            result.issues.push_back({id, ExtensionPlanIssueCode::UnknownId});
            continue;
        }
        // An extension id takes its removable entries; it is refused only when none is.
        std::optional<ExtensionPlanIssueCode> first_issue;
        std::vector<std::size_t> removable;
        for (const std::size_t i : matches) {
            if (const auto code = issue(list.extensions[i])) {
                first_issue = first_issue ? first_issue : code;
            } else {
                removable.push_back(i);
            }
        }
        if (removable.empty()) {
            result.issues.push_back({id, *first_issue});
            continue;
        }
        for (const std::size_t i : removable) {
            if (std::find(chosen.begin(), chosen.end(), i) == chosen.end()) {
                chosen.push_back(i);
                result.planned.push_back(list.extensions[i].id);
            }
        }
    }

    // The sources, each once, and who they belong to.
    std::vector<std::size_t> sources;
    std::vector<std::wstring> owner(list.sources.size());
    for (const std::size_t i : chosen) {
        for (const std::size_t s : list.extensions[i].sources) {
            if (std::find(sources.begin(), sources.end(), s) == sources.end()) {
                sources.push_back(s);
                owner[s] = label(list.extensions[i]);
            }
        }
    }

    // Lists and JSON values: every removal from one value in one edit, in the order first met.
    std::vector<std::wstring> done_keys;
    std::vector<std::wstring> done_locations;
    for (const std::size_t s : sources) {
        const ExtensionSource& source = list.sources[s];
        const bool chromium = !source.browsers.empty() && is_chromium_family(source.browsers.front());
        if (is_list(source.kind) || is_json_settings(source)) {
            const std::wstring identity = key_identity(source.key) + L"\\\\" +
                                          platform::to_lower(source.value_name) +
                                          (is_list(source.kind) ? L"|list" : L"");
            const std::wstring group = is_list(source.kind) ? key_identity(source.key) : identity;
            if (std::find(done_keys.begin(), done_keys.end(), group) != done_keys.end()) {
                continue;
            }
            done_keys.push_back(group);
            std::set<std::wstring> removed;
            for (const std::size_t other : sources) {
                const ExtensionSource& peer = list.sources[other];
                if (peer.kind != source.kind || peer.settings_subkey != source.settings_subkey ||
                    !platform::same_address(peer.key, source.key)) {
                    continue;
                }
                if (is_list(source.kind)) {
                    removed.insert(platform::to_lower(peer.value_name));
                } else if (platform::iequals(peer.value_name, source.value_name)) {
                    removed.insert(chromium ? platform::to_lower(peer.extension_id) : peer.extension_id);
                }
            }
            if (is_list(source.kind)) {
                std::vector<ValueEditLocation> edits;
                plan_list(source.list, removed, source.key, edits);
                for (auto& edit : edits) {
                    result.plan.items.push_back(planned(std::move(edit), owner[s]));
                }
            } else if (source.value) {
                if (auto desired = plan_settings(*source.value, removed, chromium)) {
                    result.plan.items.push_back(planned(ValueEditLocation{source.key,
                                                                          source.value_name,
                                                                          ValueEditKind::RestoreValue,
                                                                          source.value,
                                                                          std::move(*desired),
                                                                          {},
                                                                          0},
                                                        owner[s]));
                }
            }
            continue;
        }
        Location location = probe(source);
        const std::wstring identity = leftovers::identity(location);
        if (std::find(done_locations.begin(), done_locations.end(), identity) != done_locations.end()) {
            continue;
        }
        done_locations.push_back(identity);
        result.plan.items.push_back(planned(std::move(location), owner[s]));
    }

    // Every entry that loses a source, the ones asked for first.
    for (const std::size_t i : chosen) {
        result.affected.push_back(list.extensions[i].id);
    }
    for (std::size_t i = 0; i < list.extensions.size(); ++i) {
        const auto& entry = list.extensions[i];
        if (std::find(chosen.begin(), chosen.end(), i) == chosen.end() &&
            std::any_of(entry.sources.begin(), entry.sources.end(), [&](std::size_t s) {
                return std::find(sources.begin(), sources.end(), s) != sources.end();
            })) {
            result.affected.push_back(entry.id);
        }
    }
    for (const auto& id : result.affected) {
        const auto found = std::find_if(list.extensions.begin(), list.extensions.end(),
                                        [&](const BrowserExtension& entry) { return entry.id == id; });
        if (found != list.extensions.end() && found->keeps_user_copy) {
            result.notes.push_back({id, ExtensionPlanNoteCode::KeepsUserCopy});
        }
    }
    return result;
}

std::string_view to_string(ExtensionPlanNoteCode code) {
    switch (code) {
        case ExtensionPlanNoteCode::KeepsUserCopy:
            return "keeps-user-copy";
    }
    return "unknown";
}

std::string_view to_string(ExtensionPlanIssueCode code) {
    switch (code) {
        case ExtensionPlanIssueCode::UnknownId:
            return "unknown-id";
        case ExtensionPlanIssueCode::NotRemovable:
            return "not-removable";
        case ExtensionPlanIssueCode::ManagedByPoliciesJson:
            return "managed-by-policies-json";
        case ExtensionPlanIssueCode::NotSupported:
            return "not-supported";
        case ExtensionPlanIssueCode::Protected:
            return "protected";
    }
    return "unknown";
}

}  // namespace avel::tools
