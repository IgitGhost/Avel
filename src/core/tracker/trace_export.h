// Moving a trace to another machine (docs/trace.md, "Export and import"): `trace export` writes one
// JSON file with the trace, its links, its changes and its saved known values; `trace import`
// adds it to the store as an imported trace and links it again to what this machine has installed.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/changes/serialization.h"
#include "core/inventory/inventory.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/known_values.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_store.h"

namespace avel::tracker {

inline constexpr std::string_view kExportFormat = "avel-trace";
inline constexpr int kExportSchemaVersion = 1;

// A user whose items a trace holds.
struct TraceUser {
    std::wstring sid;
    std::wstring profile;  // Their profile folder where the trace was recorded; empty if unknown
};

// A trace as an export file carries it.
struct TraceExport {
    TraceInfo info;
    TraceResult result;
    KnownValues saved_values;  // saved_values.json: the known spots' values the installer changed
    std::vector<TraceUser> users;
};

// The users a trace's changes (noise left out) and known values belong to: the SIDs of its
// registry items, packages and known values, and the owners of the profile folders its files are
// in. `profiles` gives the profile folders.
std::vector<TraceUser> trace_users(const TraceResult& result,
                                   std::span<const platform::UserProfile> profiles);

// A finished trace as it is in the store, with its users (from `profiles`). Fails with NotFound,
// NotFinished, or Damaged when its files can't be read.
std::optional<TraceExport> read_export(const TraceStore& store, const std::wstring& id,
                                       std::span<const platform::UserProfile> profiles, TraceError& error);

// How many environment values (the machine's and users' Environment) the export holds, saved or
// changed: they say something about the machine, so `trace export` warns about them.
std::size_t environment_values(const TraceExport& trace);

// The export file: {"format": "avel-trace", "schema_version": 1, "avel_version", "exported",
// "users": [{"sid", "profile"}], "trace", "changes", "saved_values"}, the last three exactly as the
// store keeps them.
changes::Json to_json(const TraceExport& trace, std::chrono::system_clock::time_point exported);

enum class ImportError : std::uint8_t {
    None,
    Damaged,       // Not JSON, or a part is missing or can't be read
    OtherFormat,   // JSON, but not an Avel trace export
    OtherVersion,  // An Avel trace export of another schema version (see `version`)
    NotFinished,   // The trace in it was never finished
};

std::string_view to_string(ImportError error);

// Reads an export file. `version` is the file's schema version, when it says one.
ImportError parse_export(std::string_view text, TraceExport& trace, int& version);

// Moves the trace's items of user `from` to user `to`: registry items and known values from
// their SID to the other's, files and string values from their profile folder to the other's.
void remap_user(TraceExport& trace, const TraceUser& from, const TraceUser& to);

struct ImportOptions {
    TraceUser user;  // Who imports: per-user items are moved to them
};

// This process's user and their profile folder.
ImportOptions live_import_options();

// Adds an exported trace to the store as imported. When it holds a single user (with a known
// profile folder), their items are moved to `options.user`; otherwise they stay as they are, and
// each user gets a UserNotRemapped diagnostic. Its links are resolved again against `inventory`
// (relink). Returns the trace as stored, or nullopt with the reason (TraceStore::add_finished).
std::optional<TraceInfo> import_trace(TraceStore& store, TraceExport trace,
                                      const inventory::Inventory& inventory, const ImportOptions& options,
                                      TraceError& error);

}  // namespace avel::tracker
