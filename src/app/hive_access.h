#pragma once

#include <QString>

#include <functional>
#include <mutex>
#include <shared_mutex>

#include "core/inventory/inventory.h"

namespace avel::app {

// One lock for users' registry hives across avel.exe's background work.
//
// A signed-out user's hive can be loaded privately (RegLoadAppKey) only once per process, and
// leftovers::LiveEnvironment keeps it loaded while it lives; so do an inventory scan, briefly, and
// a trace while it stops. A ChangeSet that writes to that hive meanwhile fails on it as in use.
// Each window serialises its own work, so this is about work from different windows overlapping:
//   - every scope that loads hives (a LiveEnvironment, an inventory scan, a trace's stop) holds
//     shared access, no longer than the hives stay loaded;
//   - every ChangeSet execute, restore and recovery holds exclusive access while it runs.
//
// Taken on worker threads only, never on the GUI thread: a scan can take minutes. Lock order:
// this lock first, then the ChangeStore's (ChangeStore::lock); never this one while holding the
// store's. Neither access may be taken again on a thread that holds either: a thread that lists
// and then changes lets go of the listing's shared access first (it must free the hives anyway).
[[nodiscard]] std::shared_lock<std::shared_mutex> sharedHiveAccess();
[[nodiscard]] std::unique_lock<std::shared_mutex> exclusiveHiveAccess();

// Exclusive access as above. When something holds the lock, `waiting(true)` is called on this
// thread before waiting for it, and `waiting(false)` once it is granted; when it is free at once,
// neither.
[[nodiscard]] std::unique_lock<std::shared_mutex> exclusiveHiveAccess(
    const std::function<void(bool)>& waiting);

// What a change shows while it waits for exclusive access.
QString hiveWaitText();

// inventory::scan_inventory with shared access: it loads signed-out users' hives, one at a time.
// Not on a thread that holds the lock already.
inventory::Inventory scanInventory(const inventory::ScanOptions& options = {});

}  // namespace avel::app
