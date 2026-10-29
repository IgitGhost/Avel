// A trace as the store keeps it: trace.json (TraceInfo: its state and a summary of how it went)
// and changes.json (TraceResult: what it found). The JSON field names are the ones
// `avel-cli trace --json` prints (docs/trace.md); everything the store needs to read a trace back
// is kept exactly, names that UTF-8 can't carry included.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/changes/serialization.h"
#include "core/tracker/installer_watch.h"
#include "core/tracker/journal_trace.h"
#include "core/tracker/registry_snapshot.h"
#include "core/tracker/trace.h"

namespace avel::tracker {

inline constexpr int kTraceSchemaVersion = 1;

enum class TraceState : std::uint8_t {
    Starting,    // Being set up: journal positions, known values, registry snapshot
    Open,        // Recording; survives restarts
    Stopping,    // Its journals were read to the end and checkpointed; the comparison is under way
    Complete,    // Finished, everything compared
    Incomplete,  // Finished, but a volume or part of the registry couldn't be compared
};

enum class TraceMode : std::uint8_t {
    Manual,  // Started and stopped by hand
    Launch,  // Install and trace: Avel launched the installer and stopped by itself
};

enum class StopReason : std::uint8_t {
    None,           // Not stopped yet
    Manual,         // Stopped by hand
    InstallerDone,  // The installation was over
    Stopped,        // The user stopped Install and trace early (the installer may still run)
    TimedOut,       // Install and trace gave up waiting
    RebootPending,  // The installer asked for a restart; the trace stays open until it's stopped
};

struct RegistrySummary {
    std::uint64_t keys = 0;  // In the snapshot
    std::uint64_t values = 0;
    std::uint64_t snapshot_ms = 0;
    std::uint64_t diff_ms = 0;
    // While open, what the snapshot couldn't read; once stopped, what the comparison couldn't
    // (which repeats the former as KeyUnreadableAtStart). Each makes the trace incomplete.
    std::vector<SnapshotDiagnostic> diagnostics;
};

struct TraceCounts {
    std::uint64_t changes = 0;  // Not noise
    std::uint64_t noise = 0;
};

enum class TraceDiagnosticCode : std::uint8_t {
    CheckpointDamaged,   // A volume's checkpoint was missing or damaged: its changes are lost (the volume
                         // counts as unreadable)
    SnapshotDamaged,     // registry.snapshot was missing or damaged: the registry wasn't compared
    SavedValuesDamaged,  // saved_values.json was missing or damaged: the known values weren't compared
    // Imported: the trace holds several users' items (or a user whose profile is unknown), so they
    // were left under the user they were recorded for
    UserNotRemapped,
};

struct TraceDiagnostic {
    TraceDiagnosticCode code = TraceDiagnosticCode::CheckpointDamaged;
    std::wstring subject;  // CheckpointDamaged: the volume's root; UserNotRemapped: the user's SID
};

// How a trace was linked to a program (linking.h).
enum class LinkMatch : std::uint8_t {
    CreatedKey,   // The trace created its Uninstall key, or a key that holds it
    ChangedKey,   // Its Uninstall key existed, and the trace changed its values (an upgrade)
    ProductCode,  // The trace registered its Windows Installer product
    Manual,       // Linked by hand (`trace link`)
};

// A program the trace installed, as it was when it was last found. Enough is kept to find it again
// after an upgrade (another version, product code or even Uninstall key) and to name it once it's
// gone.
struct TraceLink {
    std::wstring program_id;  // inventory::Program::id
    std::wstring name;        // Its display name
    std::wstring publisher;
    std::wstring version;
    std::optional<platform::RegistryAddress> uninstall_key;
    std::wstring product_code;      // "{GUID}" of a Windows Installer product
    std::wstring install_location;  // Normalized
    LinkMatch matched_by = LinkMatch::CreatedKey;
    bool primary = false;  // The program the trace is for: the first link, preferably an entry Settings lists
};

// A trace brought in from another machine (`trace import`).
struct TraceImport {
    std::chrono::system_clock::time_point time{};
    std::wstring origin_id;  // Its id where it was recorded
    // The user its per-user items were moved from and to; empty when nothing was remapped.
    std::wstring from_sid;
    std::wstring from_profile;
    std::wstring to_sid;
    std::wstring to_profile;
};

// trace.json.
struct TraceInfo {
    std::wstring id;  // Also its folder's name
    TraceState state = TraceState::Starting;
    TraceMode mode = TraceMode::Manual;
    std::wstring description;
    std::wstring command;  // Launch: the installer and its arguments
    std::chrono::system_clock::time_point started{};
    std::optional<std::chrono::system_clock::time_point> stopped;
    std::int64_t trace_start = 0;  // FILETIME (UTC) when the journals' positions were taken
    StopReason stop_reason = StopReason::None;
    // As they were at the start, and once stopped, at the end. While open, the volume checkpoints
    // are what is current.
    std::vector<VolumeTrace> volumes;
    RegistrySummary registry;
    std::optional<WatchResult> watch;  // Install and trace
    TraceCounts counts;
    std::vector<TraceDiagnostic> diagnostics;
    // The programs it installed, the primary one first (linking.h); empty until it's linked. A
    // trace.json from 5a has none.
    std::vector<TraceLink> links;
    // Retention: when the links were first found to lead to no installed program; cleared once
    // they do again (an undo that restores the program restores the link).
    std::optional<std::chrono::system_clock::time_point> unresolved_since;
    std::optional<TraceImport> imported;
};

// Why a stopped trace is incomplete: a volume whose journal couldn't be followed to the end, or a
// part of the registry that wasn't compared.
enum class IncompleteKind : std::uint8_t {
    Volume,              // `volume` and its `status`
    Registry,            // `diagnostic`
    SnapshotDamaged,     // The registry wasn't compared at all
    SavedValuesDamaged,  // The known values weren't compared
};

struct IncompleteReason {
    IncompleteKind kind = IncompleteKind::Volume;
    std::wstring volume;
    JournalStatus status = JournalStatus::Ok;
    SnapshotDiagnostic diagnostic;
};

std::vector<IncompleteReason> incomplete_reasons(const TraceInfo& info);

// changes.json: the changes (noise included, with its reason) and the known values that changed.
struct TraceResult {
    std::vector<TraceChange> changes;
    std::vector<KnownValueChange> known_values;
};

std::string_view to_string(TraceState state);
std::string_view to_string(TraceMode mode);
std::string_view to_string(StopReason reason);  // None is "none"; the JSON writes null for it
std::string_view to_string(TraceDiagnosticCode code);
std::string_view to_string(LinkMatch match);

// {"program_id", "name", "publisher", "version", "uninstall_key" (a registry-key location or
// null), "product_code", "install_location", "matched_by", "primary"}, exact.
changes::Json to_json(const TraceLink& link);
bool from_json(const changes::Json& json, TraceLink& link);

// {"change", "location" (changes::to_json, exact), "old_path", "contents", "replaced", "noise"}.
changes::Json to_json(const TraceChange& change);
bool from_json(const changes::Json& json, TraceChange& change);

// The `trace` object of docs/trace.md, plus what reading it back needs (schema_version, the
// volumes' journal positions, ...). "complete" and "incomplete_reasons" are derived, not read.
changes::Json to_json(const TraceInfo& info);
bool from_json(const changes::Json& json, TraceInfo& info);

// {"volume", "status"}, {"registry": <code>, "key", "error"}, {"registry": "snapshot-damaged"} or
// {"registry": "saved-values-damaged"}.
changes::Json to_json(const IncompleteReason& reason);

// {"changes": [...], "known_values": [...]}.
changes::Json to_json(const TraceResult& result);
bool from_json(const changes::Json& json, TraceResult& result);

}  // namespace avel::tracker
