#include "core/tracker/known_values.h"

#include <wil/resource.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>

#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::tracker {

namespace {

using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

constexpr wchar_t kMachineEnvironment[] = LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)";
constexpr wchar_t kSharedDlls[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)";
constexpr wchar_t kSharedDlls32[] = LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)";

KnownRoot machine_root(KnownSpot spot, const std::wstring& path) {
    return {spot, {Hive::Machine, L"", path}, HKEY_LOCAL_MACHINE, path, nullptr};
}

void sort_values(std::vector<RawValue>& values) {
    std::sort(values.begin(), values.end(), [](const RawValue& a, const RawValue& b) {
        return compare_registry_names(a.name, b.name) < 0;
    });
}

std::optional<KnownSpot> spot_named(std::string_view text) {
    for (const KnownSpot spot :
         {KnownSpot::MachineEnvironment, KnownSpot::UserEnvironment, KnownSpot::SharedDlls}) {
        if (to_string(spot) == text) {
            return spot;
        }
    }
    return std::nullopt;
}

changes::Json value_json(const RawValue& value) {
    changes::Json json;
    changes::put_text(json, "name", value.name);
    json["type"] = value.type;
    json["data"] = changes::to_hex(value.data);
    return json;
}

bool value_from(const changes::Json& json, RawValue& value) {
    if (!json.is_object()) {
        return false;
    }
    const auto name = changes::get_text(json, "name");
    const auto type = json.find("type");
    const auto data = json.find("data");
    if (!name || type == json.end() || !type->is_number_unsigned() || data == json.end() ||
        !data->is_string()) {
        return false;
    }
    auto bytes = changes::from_hex(data->get_ref<const std::string&>());
    if (!bytes) {
        return false;
    }
    value = {*name, type->get<DWORD>(), std::move(*bytes)};
    return true;
}

void put_key(changes::Json& json, KnownSpot spot, const RegistryAddress& key) {
    json["spot"] = std::string(to_string(spot));
    json["key"] = changes::to_json(leftovers::Location(leftovers::RegistryKeyLocation{key}));
}

bool get_key(const changes::Json& json, KnownSpot& spot, RegistryAddress& key) {
    const auto spot_field = json.find("spot");
    const auto key_field = json.find("key");
    if (spot_field == json.end() || !spot_field->is_string() || key_field == json.end()) {
        return false;
    }
    const auto named = spot_named(spot_field->get_ref<const std::string&>());
    leftovers::Location location;
    if (!named || !changes::from_json(*key_field, location)) {
        return false;
    }
    const auto* registry_key = std::get_if<leftovers::RegistryKeyLocation>(&location);
    if (!registry_key) {
        return false;
    }
    spot = *named;
    key = registry_key->key;
    return true;
}

}  // namespace

std::vector<KnownRoot> live_known_roots() {
    std::vector<KnownRoot> roots{machine_root(KnownSpot::MachineEnvironment, kMachineEnvironment),
                                 machine_root(KnownSpot::SharedDlls, kSharedDlls),
                                 machine_root(KnownSpot::SharedDlls, kSharedDlls32)};
    for (const auto& profile : platform::enumerate_user_profiles()) {
        if (profile.hive_loaded) {
            roots.push_back({KnownSpot::UserEnvironment,
                             {Hive::User, profile.sid, L"Environment"},
                             HKEY_USERS,
                             profile.sid + LR"(\Environment)",
                             nullptr});
        }
    }
    return roots;
}

std::vector<KnownRoot> live_known_roots(const KnownValues& started, UserHiveCache& hives) {
    std::vector<KnownRoot> roots;
    for (const KnownKey& key : started.keys) {
        if (key.key.hive == Hive::Machine) {
            roots.push_back(machine_root(key.spot, key.key.path));
            continue;
        }
        if (auto hive = hives.open(key.key.sid, false)) {
            const HKEY parent = hive->key.get();
            roots.push_back({key.spot, key.key, parent, key.key.path, std::move(hive)});
        }
    }
    return roots;
}

KnownValues capture_known_values(std::span<const KnownRoot> roots) {
    KnownValues captured;
    // Two names for one key (a WOW6432Node key that both views share) are read once.
    std::set<std::wstring> seen;
    for (const KnownRoot& root : roots) {
        KnownKey key{root.spot, root.key, {}, false};
        HKEY handle = nullptr;
        const LSTATUS status = root.parent ? ::RegOpenKeyExW(root.parent, root.subpath.c_str(), 0,
                                                             KEY_QUERY_VALUE | KEY_WOW64_64KEY, &handle)
                                           : ERROR_INVALID_HANDLE;
        if (status == ERROR_FILE_NOT_FOUND) {
            captured.keys.push_back(std::move(key));
            continue;
        }
        if (status != ERROR_SUCCESS) {
            continue;
        }
        const wil::unique_hkey opened(handle);
        const std::wstring kernel_name = platform::to_lower(platform::kernel_name(opened.get()));
        if (!kernel_name.empty() && !seen.insert(kernel_name).second) {
            continue;
        }
        if (platform::read_raw_values(opened.get(), key.values) != ERROR_SUCCESS) {
            continue;
        }
        sort_values(key.values);
        key.present = true;
        captured.keys.push_back(std::move(key));
    }
    return captured;
}

std::vector<KnownValueChange> diff_known_values(const KnownValues& before, const KnownValues& after) {
    std::vector<KnownValueChange> changes;
    for (const KnownKey& old_key : before.keys) {
        const auto found = std::find_if(after.keys.begin(), after.keys.end(), [&](const KnownKey& candidate) {
            return candidate.spot == old_key.spot && platform::same_address(candidate.key, old_key.key);
        });
        if (found == after.keys.end()) {
            continue;
        }
        std::vector<RawValue> old_values = old_key.values;
        std::vector<RawValue> new_values = found->values;
        sort_values(old_values);
        sort_values(new_values);
        const auto add = [&](const std::wstring& name, const RawValue* old_value, const RawValue* new_value) {
            KnownValueChange change;
            change.spot = old_key.spot;
            change.key = old_key.key;
            change.name = name;
            if (old_value) {
                change.before = *old_value;
            }
            if (new_value) {
                change.after = *new_value;
            }
            changes.push_back(std::move(change));
        };
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < old_values.size() || j < new_values.size()) {
            const int order = i == old_values.size()   ? 1
                              : j == new_values.size() ? -1
                              : platform::iequals(old_values[i].name, new_values[j].name)
                                  ? 0
                                  : compare_registry_names(old_values[i].name, new_values[j].name);
            if (order < 0) {
                add(old_values[i].name, &old_values[i], nullptr);
                ++i;
            } else if (order > 0) {
                add(new_values[j].name, nullptr, &new_values[j]);
                ++j;
            } else {
                if (old_values[i].type != new_values[j].type || old_values[i].data != new_values[j].data) {
                    add(new_values[j].name, &old_values[i], &new_values[j]);
                }
                ++i;
                ++j;
            }
        }
    }
    return changes;
}

changes::Json to_json(const KnownValues& values) {
    changes::Json keys = changes::Json::array();
    for (const KnownKey& key : values.keys) {
        changes::Json item;
        put_key(item, key.spot, key.key);
        item["present"] = key.present;
        item["values"] = changes::Json::array();
        for (const RawValue& value : key.values) {
            item["values"].push_back(value_json(value));
        }
        keys.push_back(std::move(item));
    }
    changes::Json json;
    json["keys"] = std::move(keys);
    return json;
}

bool from_json(const changes::Json& json, KnownValues& values) {
    const auto keys = json.is_object() ? json.find("keys") : json.end();
    if (!json.is_object() || keys == json.end() || !keys->is_array()) {
        return false;
    }
    KnownValues parsed;
    for (const auto& item : *keys) {
        KnownKey key;
        if (!item.is_object() || !get_key(item, key.spot, key.key)) {
            return false;
        }
        const auto present = item.find("present");
        const auto list = item.find("values");
        if (present == item.end() || !present->is_boolean() || list == item.end() || !list->is_array()) {
            return false;
        }
        key.present = present->get<bool>();
        for (const auto& value : *list) {
            if (!value_from(value, key.values.emplace_back())) {
                return false;
            }
        }
        sort_values(key.values);
        parsed.keys.push_back(std::move(key));
    }
    values = std::move(parsed);
    return true;
}

changes::Json to_json(const KnownValueChange& change) {
    changes::Json json;
    put_key(json, change.spot, change.key);
    changes::put_text(json, "name", change.name);
    if (change.before) {
        json["before"] = value_json(*change.before);
    }
    if (change.after) {
        json["after"] = value_json(*change.after);
    }
    return json;
}

bool from_json(const changes::Json& json, KnownValueChange& change) {
    KnownValueChange parsed;
    if (!json.is_object() || !get_key(json, parsed.spot, parsed.key)) {
        return false;
    }
    const auto name = changes::get_text(json, "name");
    if (!name) {
        return false;
    }
    parsed.name = *name;
    for (const auto& [field, target] :
         {std::pair{"before", &parsed.before}, std::pair{"after", &parsed.after}}) {
        if (const auto found = json.find(field); found != json.end()) {
            RawValue value;
            if (!value_from(*found, value)) {
                return false;
            }
            *target = std::move(value);
        }
    }
    change = std::move(parsed);
    return true;
}

}  // namespace avel::tracker
