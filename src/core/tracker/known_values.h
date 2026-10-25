// The known spots of a trace (see KnownSpot): keys whose values are saved byte for byte at its start
// and stop, so removal can take back exactly what the installer added to a shared value (a PATH
// entry, a SharedDLLs reference count) instead of deleting the value.
#pragma once

#include <windows.h>

#include <memory>
#include <span>
#include <vector>

#include "core/changes/serialization.h"
#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"
#include "core/platform/user_hives.h"
#include "core/tracker/registry_snapshot.h"
#include "core/tracker/trace.h"

namespace avel::tracker {

// A known spot's key: where it is reported, and how to open it.
struct KnownRoot {
    KnownSpot spot = KnownSpot::MachineEnvironment;
    platform::RegistryAddress key;
    HKEY parent = nullptr;  // Opened as parent\subpath: a predefined handle, or hive's key
    std::wstring subpath;
    std::shared_ptr<const platform::UserHive> hive;  // Keeps a privately loaded hive loaded
};

struct KnownKey {
    KnownSpot spot = KnownSpot::MachineEnvironment;
    platform::RegistryAddress key;
    std::vector<platform::RawValue> values;  // Sorted by name (ordinal, ignoring case)
    bool present = false;                    // false: the key didn't exist (no values)
};

// A key that couldn't be read is left out, so nothing is compared for it.
struct KnownValues {
    std::vector<KnownKey> keys;
};

// The known spots when a trace starts: the machine's environment, both SharedDLLs keys, and every
// signed-in user's environment.
std::vector<KnownRoot> live_known_roots();

// The same keys when it stops, for comparing with `started`; users who signed out in the meantime
// are read from their hive files, through the cache the registry diff shares.
std::vector<KnownRoot> live_known_roots(const KnownValues& started, UserHiveCache& hives);

KnownValues capture_known_values(std::span<const KnownRoot> roots);

// Every value added, changed or deleted in a key captured at both ends.
std::vector<KnownValueChange> diff_known_values(const KnownValues& before, const KnownValues& after);

// Exact: names that UTF-8 can't carry are kept as hex (put_text), data as hex.
changes::Json to_json(const KnownValues& values);
bool from_json(const changes::Json& json, KnownValues& values);
changes::Json to_json(const KnownValueChange& change);
bool from_json(const changes::Json& json, KnownValueChange& change);

}  // namespace avel::tracker
