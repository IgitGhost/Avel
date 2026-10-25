// Avel Trace's registry side (ADR 0002): when a trace starts, a snapshot of every key's
// LastWriteTime and a hash of each of its values, kept on disk so the trace survives a reboot;
// when it stops, the live registry compared with it. LastWriteTime prunes that comparison: a
// key's values are read again only where it changed. Children are still listed everywhere, since
// a parent's LastWriteTime isn't something to rely on.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/registry_address.h"
#include "core/platform/user_hives.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/trace.h"

namespace avel::tracker {

// A tree the snapshot covers: where its keys are reported, and how to open it. Below the root
// every key is opened as itself (REG_OPTION_OPEN_LINK): a registry link is recorded, not followed.
struct SnapshotRoot {
    platform::RegistryAddress address;  // e.g. Machine "SOFTWARE", or User <sid> "Software\Classes"
    HKEY parent = nullptr;              // Opened as parent\subpath: a predefined handle or hive's key
    std::wstring subpath;               // Empty for `parent` itself
    std::shared_ptr<const platform::UserHive> hive;  // Keeps a privately loaded hive loaded
};

class RegistrySnapshot;

// Users' hives opened once each for a trace's stop and shared by everything that reads them: a
// hive file can be loaded privately only once per process at a time, and both the registry diff
// and the known values read the hives of users who signed out during the trace.
class UserHiveCache {
public:
    // `sid`'s main hive (their HKEY_CURRENT_USER) or classes hive (their Software\Classes): the
    // live one while they are signed in, otherwise a private, read-only load. Null if it can't be
    // opened.
    std::shared_ptr<const platform::UserHive> open(const std::wstring& sid, bool classes);

private:
    std::optional<std::vector<platform::UserProfile>> profiles_;
    std::map<std::wstring, std::shared_ptr<const platform::UserHive>> hives_;
};

// What a trace snapshots when it starts: HKLM\SOFTWARE (with WOW6432Node and Classes),
// HKLM\SYSTEM\CurrentControlSet\Services, and for every signed-in user HKU\<sid>\Software and
// HKU\<sid>_Classes (reported as their Software\Classes).
std::vector<SnapshotRoot> live_snapshot_roots();

// The same roots when the trace stops, for comparing with `started`: users who signed out in the
// meantime are read from their hive files. A root that can't be opened is left out (the diff
// reports it).
std::vector<SnapshotRoot> live_snapshot_roots(const RegistrySnapshot& started, UserHiveCache& hives);

// The order the snapshot keeps names in: ordinal ignoring case, like the registry itself (0 for
// names it takes as the same name), then exactly, so the order is total.
int compare_registry_names(std::wstring_view a, std::wstring_view b);

// A key's flags in the snapshot.
constexpr std::uint32_t kKeyLink = 1;        // A registry link: recorded, never followed
constexpr std::uint32_t kKeyUnreadable = 2;  // Couldn't be opened or read: its subtree is unknown

struct SnapKey {
    std::uint32_t name;         // In the snapshot's name pool; a root's top key has an empty name
    std::uint32_t flags;        // kKeyLink, kKeyUnreadable
    std::int64_t lwt;           // LastWriteTime, in FILETIME units
    std::uint32_t first_child;  // Children are contiguous and sorted by name (ordinal, ignoring case)
    std::uint32_t child_count;
    std::uint32_t first_value;  // Values are contiguous and sorted by name, too
    std::uint32_t value_count;
};

struct SnapValue {
    std::uint32_t name;
    std::uint32_t type;
    std::uint64_t hash;  // platform::hash_raw_value of its exact bytes
};

// The registry as a trace found it at its start. Keys are laid out depth first, each key's
// children in one block after it, so the layout depends only on the registry's contents: taking
// it with one thread or eight gives the same bytes.
class RegistrySnapshot {
public:
    static constexpr std::uint32_t kNoKey = 0xFFFFFFFF;

    struct Root {
        platform::RegistryAddress address;
        std::uint32_t key = kNoKey;  // Its top key; kNoKey when the root couldn't be opened
    };

    const std::vector<Root>& roots() const { return roots_; }
    std::span<const SnapKey> keys() const { return keys_; }
    std::span<const SnapValue> values() const { return values_; }
    std::span<const SnapKey> children(const SnapKey& key) const {
        return std::span(keys_).subspan(key.first_child, key.child_count);
    }
    std::span<const SnapValue> values(const SnapKey& key) const {
        return std::span(values_).subspan(key.first_value, key.value_count);
    }
    // Names are UTF-16 exactly as the registry holds them, unpaired surrogates included.
    std::wstring_view name(std::uint32_t id) const {
        return {name_chars_.data() + name_offsets_[id], name_offsets_[id + 1] - name_offsets_[id]};
    }
    std::size_t name_count() const { return name_offsets_.size() - 1; }

private:
    friend struct SnapshotAccess;

    std::vector<Root> roots_;
    std::vector<SnapKey> keys_;
    std::vector<SnapValue> values_;
    std::vector<std::uint32_t> name_offsets_{0};  // Name i is name_chars_[offsets[i], offsets[i + 1])
    std::vector<wchar_t> name_chars_;
};

struct SnapshotOptions {
    unsigned threads = 0;  // Worker threads; 0 for one per hardware thread, at most 8
};

enum class SnapshotDiagnosticCode : std::uint8_t {
    RootUnavailable,       // A root couldn't be opened at the start or the stop: nothing in it was compared
    KeyUnreadable,         // A key couldn't be read: changes in it and below it are unknown
    KeyUnreadableAtStart,  // (diff) The snapshot couldn't read this key: changes in it and below are unknown
};

struct SnapshotDiagnostic {
    SnapshotDiagnosticCode code = SnapshotDiagnosticCode::KeyUnreadable;
    platform::RegistryAddress key;
    std::int32_t error = 0;  // Win32 error code, when there is one
};

std::string_view to_string(SnapshotDiagnosticCode code);

// Reads the trees below `roots`, skipping subtrees that only Windows churns (servicing, Defender,
// background activity). Keys that deny access are read with backup
// intent when SeBackupPrivilege can be enabled, and otherwise marked unreadable.
RegistrySnapshot take_registry_snapshot(std::span<const SnapshotRoot> roots, const SnapshotOptions& options,
                                        std::vector<SnapshotDiagnostic>& diagnostics);

// A binary file: "AVELRSN1", a version, the counts, and an FNV-1a checksum of everything after it,
// written and flushed to disk before returning. read_snapshot returns nullopt for a missing,
// damaged or inconsistent file.
bool write_snapshot(const RegistrySnapshot& snapshot, const std::wstring& path);
std::optional<RegistrySnapshot> read_snapshot(const std::wstring& path);

// What changed below the snapshot's roots, comparing each with the root of the same address in
// `roots`. A created key is one item, its `contents` the number of subkeys and values inside it,
// and a deleted key is one item too. Created services, scheduled tasks and firewall rules are
// named as such (see TraceChange); the SharedDLLs values are left to the known values.
std::vector<TraceChange> diff_registry(const RegistrySnapshot& snapshot, std::span<const SnapshotRoot> roots,
                                       const SnapshotOptions& options,
                                       std::vector<SnapshotDiagnostic>& diagnostics);

}  // namespace avel::tracker
