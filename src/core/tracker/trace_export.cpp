#include "core/tracker/trace_export.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/platform/paths.h"
#include "core/platform/record_folders.h"
#include "core/platform/text.h"
#include "core/tracker/linking.h"
#include "core/version.h"

namespace avel::tracker {

namespace {

using changes::Json;
using platform::RegistryAddress;

void add_user(std::vector<TraceUser>& users, const std::wstring& sid,
              std::span<const platform::UserProfile> profiles) {
    if (sid.empty() || std::any_of(users.begin(), users.end(),
                                   [&](const TraceUser& user) { return platform::iequals(user.sid, sid); })) {
        return;
    }
    TraceUser user{sid, {}};
    for (const auto& profile : profiles) {
        if (platform::iequals(profile.sid, sid)) {
            user.profile = platform::normalize_path(profile.profile_path);
        }
    }
    users.push_back(std::move(user));
}

// The owner of the profile folder `path` is in, if any.
const platform::UserProfile* profile_of(std::wstring_view path,
                                        std::span<const platform::UserProfile> profiles) {
    for (const auto& profile : profiles) {
        if (!profile.profile_path.empty() && platform::path_is_under(path, profile.profile_path)) {
            return &profile;
        }
    }
    return nullptr;
}

bool is_environment(KnownSpot spot) {
    return spot == KnownSpot::MachineEnvironment || spot == KnownSpot::UserEnvironment;
}

// Moves one user's items to another.
class Remapper {
public:
    Remapper(const TraceUser& from, const TraceUser& to)
        : from_sid_(from.sid),
          to_sid_(to.sid),
          from_profile_(from.profile.empty() ? std::wstring() : platform::normalize_path(from.profile)),
          to_profile_(to.profile.empty() ? std::wstring() : platform::normalize_path(to.profile)) {}

    void key(RegistryAddress& address) const {
        if (address.hive == platform::Hive::User && platform::iequals(address.sid, from_sid_)) {
            address.sid = to_sid_;
        }
    }

    void sid(std::wstring& sid) const {
        if (platform::iequals(sid, from_sid_)) {
            sid = to_sid_;
        }
    }

    // A path in the old profile folder, moved to the new one.
    void path(std::wstring& path) const {
        if (from_profile_.empty() || to_profile_.empty() || !platform::istarts_with(path, from_profile_) ||
            (path.size() != from_profile_.size() && path[from_profile_.size()] != L'\\')) {
            return;
        }
        path = to_profile_ + path.substr(from_profile_.size());
    }

    // Every mention of the old SID or profile folder in a text: an id ("reg:HKU\<sid>:Name"), a
    // PATH entry, a command line.
    void text(std::wstring& text) const {
        replace(text, from_sid_, to_sid_);
        replace(text, from_profile_, to_profile_);
    }

    void value(platform::RawValue& value) const {
        if (value.type != REG_SZ && value.type != REG_EXPAND_SZ && value.type != REG_MULTI_SZ) {
            return;
        }
        const std::size_t count = value.data.size() / sizeof(wchar_t);
        std::wstring text(count, L'\0');
        if (count != 0) {
            std::memcpy(text.data(), value.data.data(), count * sizeof(wchar_t));
        }
        const std::wstring before = text;
        replace(text, from_profile_, to_profile_);
        if (text == before) {
            return;
        }
        // An odd trailing byte stays as it was.
        std::vector<std::byte> data(text.size() * sizeof(wchar_t) + value.data.size() % sizeof(wchar_t));
        std::memcpy(data.data(), text.data(), text.size() * sizeof(wchar_t));
        if (value.data.size() % sizeof(wchar_t) != 0) {
            data.back() = value.data.back();
        }
        value.data = std::move(data);
    }

    void location(leftovers::Location& location) const {
        if (auto* file = std::get_if<leftovers::FileLocation>(&location)) {
            path(file->path);
        } else if (auto* registry_key = std::get_if<leftovers::RegistryKeyLocation>(&location)) {
            key(registry_key->key);
        } else if (auto* registry_value = std::get_if<leftovers::RegistryValueLocation>(&location)) {
            key(registry_value->key);
        } else if (auto* package = std::get_if<leftovers::PackageLocation>(&location)) {
            sid(package->user_sid);
        }
    }

private:
    // Replaces `what` where it stands as a whole SID or path: followed by the end, a separator, or
    // a character that ends a path in a list or a command line.
    static void replace(std::wstring& text, const std::wstring& what, const std::wstring& with) {
        if (what.empty() || with.empty()) {
            return;
        }
        std::size_t at = 0;
        while (at + what.size() <= text.size()) {
            const std::wstring_view rest = std::wstring_view(text).substr(at);
            if (!platform::istarts_with(rest, what)) {
                ++at;
                continue;
            }
            const std::size_t end = at + what.size();
            const bool boundary = end == text.size() || std::wstring_view(L"\\/;\",:\0", 7).find(text[end]) !=
                                                            std::wstring_view::npos;
            if (!boundary) {
                ++at;
                continue;
            }
            text.replace(at, what.size(), with);
            at += with.size();
        }
    }

    std::wstring from_sid_;
    std::wstring to_sid_;
    std::wstring from_profile_;
    std::wstring to_profile_;
};

}  // namespace

std::vector<TraceUser> trace_users(const TraceResult& result,
                                   std::span<const platform::UserProfile> profiles) {
    std::vector<TraceUser> users;
    for (const auto& change : result.changes) {
        if (change.noise != NoiseReason::None) {
            continue;
        }
        // Registry items in a user's hive, and packages registered for a user.
        if (const std::wstring owner = leftovers::owner_sid(change.location); !owner.empty()) {
            add_user(users, owner, profiles);
        }
        const auto in_profile = [&](const std::wstring& path) {
            if (const auto* profile = profile_of(path, profiles)) {
                add_user(users, profile->sid, profiles);
            }
        };
        if (const auto* file = std::get_if<leftovers::FileLocation>(&change.location)) {
            in_profile(file->path);
        }
        if (!change.old_path.empty()) {
            in_profile(change.old_path);
        }
    }
    for (const auto& value : result.known_values) {
        if (value.key.hive == platform::Hive::User) {
            add_user(users, value.key.sid, profiles);
        }
    }
    return users;
}

std::optional<TraceExport> read_export(const TraceStore& store, const std::wstring& id,
                                       std::span<const platform::UserProfile> profiles, TraceError& error) {
    auto info = store.read(id);
    if (!info) {
        error = TraceError::NotFound;
        return std::nullopt;
    }
    if (info->state != TraceState::Complete && info->state != TraceState::Incomplete) {
        error = TraceError::NotFinished;
        return std::nullopt;
    }
    auto result = store.read_result(id);
    if (!result) {
        error = TraceError::Damaged;
        return std::nullopt;
    }
    TraceExport trace;
    const auto text = changes::read_file(store.folder_of(id) + L"\\" + std::wstring(kSavedValuesFile));
    if (!text || !from_json(Json::parse(*text, nullptr, false), trace.saved_values)) {
        // Only a trace that already found them damaged goes without them.
        if (std::none_of(info->diagnostics.begin(), info->diagnostics.end(),
                         [](const TraceDiagnostic& diagnostic) {
                             return diagnostic.code == TraceDiagnosticCode::SavedValuesDamaged;
                         })) {
            error = TraceError::Damaged;
            return std::nullopt;
        }
        trace.saved_values = {};
    }
    trace.users = trace_users(*result, profiles);
    trace.info = std::move(*info);
    trace.result = std::move(*result);
    error = TraceError::None;
    return trace;
}

std::size_t environment_values(const TraceExport& trace) {
    std::size_t count = 0;
    for (const auto& key : trace.saved_values.keys) {
        if (is_environment(key.spot)) {
            count += key.values.size();
        }
    }
    for (const auto& change : trace.result.known_values) {
        if (is_environment(change.spot)) {
            ++count;
        }
    }
    return count;
}

Json to_json(const TraceExport& trace, std::chrono::system_clock::time_point exported) {
    Json json;
    json["format"] = kExportFormat;
    json["schema_version"] = kExportSchemaVersion;
    json["avel_version"] = kVersionString;
    json["exported"] = platform::iso_time(exported);
    json["users"] = Json::array();
    for (const auto& user : trace.users) {
        Json item;
        changes::put_text(item, "sid", user.sid);
        changes::put_text(item, "profile", user.profile);
        json["users"].push_back(std::move(item));
    }
    json["trace"] = to_json(trace.info);
    json["changes"] = to_json(trace.result);
    json["saved_values"] = to_json(trace.saved_values);
    return json;
}

std::string_view to_string(ImportError error) {
    switch (error) {
        case ImportError::None:
            return "none";
        case ImportError::Damaged:
            return "damaged";
        case ImportError::OtherFormat:
            return "other-format";
        case ImportError::OtherVersion:
            return "other-version";
        case ImportError::NotFinished:
            return "not-finished";
    }
    return "unknown";
}

ImportError parse_export(std::string_view text, TraceExport& trace, int& version) {
    version = 0;
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        return ImportError::Damaged;
    }
    const auto format = json.is_object() ? json.find("format") : json.end();
    if (format == json.end() || !format->is_string() ||
        format->get_ref<const std::string&>() != kExportFormat) {
        return ImportError::OtherFormat;
    }
    const auto schema = json.find("schema_version");
    if (schema == json.end() || !schema->is_number_integer()) {
        return ImportError::Damaged;
    }
    version = schema->get<int>();
    if (version != kExportSchemaVersion) {
        return ImportError::OtherVersion;
    }
    TraceExport parsed;
    const auto info = json.find("trace");
    const auto changes = json.find("changes");
    const auto saved = json.find("saved_values");
    if (info == json.end() || changes == json.end() || saved == json.end() ||
        !from_json(*info, parsed.info) || !from_json(*changes, parsed.result) ||
        !from_json(*saved, parsed.saved_values)) {
        return ImportError::Damaged;
    }
    if (parsed.info.state != TraceState::Complete && parsed.info.state != TraceState::Incomplete) {
        return ImportError::NotFinished;
    }
    if (const auto users = json.find("users"); users != json.end()) {
        if (!users->is_array()) {
            return ImportError::Damaged;
        }
        for (const auto& item : *users) {
            const auto sid = item.is_object() ? changes::get_text(item, "sid") : std::nullopt;
            if (!sid || sid->empty()) {
                return ImportError::Damaged;
            }
            parsed.users.push_back({*sid, changes::get_text(item, "profile").value_or(L"")});
        }
    } else {
        // Without the list, the users are known by their SIDs only.
        parsed.users = trace_users(parsed.result, {});
    }
    trace = std::move(parsed);
    return ImportError::None;
}

void remap_user(TraceExport& trace, const TraceUser& from, const TraceUser& to) {
    const Remapper remap(from, to);
    for (auto& change : trace.result.changes) {
        remap.location(change.location);
        remap.path(change.old_path);
    }
    for (auto& change : trace.result.known_values) {
        remap.key(change.key);
        for (auto* value : {&change.before, &change.after}) {
            if (*value) {
                remap.value(**value);
            }
        }
        if (change.spot == KnownSpot::SharedDlls) {
            // Named by the DLL's path.
            remap.path(change.name);
            for (auto* value : {&change.before, &change.after}) {
                if (*value) {
                    remap.path((*value)->name);
                }
            }
        }
    }
    for (auto& key : trace.saved_values.keys) {
        remap.key(key.key);
        for (auto& value : key.values) {
            remap.value(value);
            if (key.spot == KnownSpot::SharedDlls) {
                remap.path(value.name);
            }
        }
    }
    for (auto& link : trace.info.links) {
        remap.text(link.program_id);
        if (link.uninstall_key) {
            remap.key(*link.uninstall_key);
        }
        remap.path(link.install_location);
    }
    for (auto& diagnostic : trace.info.registry.diagnostics) {
        remap.key(diagnostic.key);
    }
    for (auto& user : trace.users) {
        if (platform::iequals(user.sid, from.sid)) {
            user = to;
        }
    }
}

ImportOptions live_import_options() {
    ImportOptions options;
    options.user.sid = platform::current_user_sid();
    for (const auto& profile : platform::enumerate_user_profiles()) {
        if (platform::iequals(profile.sid, options.user.sid)) {
            options.user.profile = platform::normalize_path(profile.profile_path);
        }
    }
    return options;
}

std::optional<TraceInfo> import_trace(TraceStore& store, TraceExport trace,
                                      const inventory::Inventory& inventory, const ImportOptions& options,
                                      TraceError& error) {
    TraceInfo& info = trace.info;
    TraceImport imported;
    imported.time = store.options().clock();
    imported.origin_id = info.id;
    // What an earlier import found applies to that machine.
    std::erase_if(info.diagnostics, [](const TraceDiagnostic& diagnostic) {
        return diagnostic.code == TraceDiagnosticCode::UserNotRemapped;
    });
    const bool single = trace.users.size() == 1 && !trace.users[0].profile.empty();
    if (single && !options.user.sid.empty() && !options.user.profile.empty()) {
        const TraceUser from = trace.users[0];
        if (!platform::iequals(from.sid, options.user.sid) ||
            !platform::paths_equal(from.profile, options.user.profile)) {
            remap_user(trace, from, options.user);
            imported.from_sid = from.sid;
            imported.from_profile = from.profile;
            imported.to_sid = options.user.sid;
            imported.to_profile = options.user.profile;
        }
    } else {
        for (const auto& user : trace.users) {
            info.diagnostics.push_back({TraceDiagnosticCode::UserNotRemapped, user.sid});
        }
    }
    info.imported = std::move(imported);
    relink(info, trace.result, inventory);
    if (!store.add_finished(info, trace.result, trace.saved_values, error)) {
        return std::nullopt;
    }
    return info;
}

}  // namespace avel::tracker
