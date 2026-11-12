#include "hive_access.h"

#include <QCoreApplication>

namespace avel::app {

namespace {

std::shared_mutex& hiveMutex() {
    static std::shared_mutex mutex;
    return mutex;
}

}  // namespace

std::shared_lock<std::shared_mutex> sharedHiveAccess() { return std::shared_lock(hiveMutex()); }

std::unique_lock<std::shared_mutex> exclusiveHiveAccess() { return std::unique_lock(hiveMutex()); }

std::unique_lock<std::shared_mutex> exclusiveHiveAccess(const std::function<void(bool)>& waiting) {
    std::unique_lock lock(hiveMutex(), std::try_to_lock);
    if (!lock.owns_lock()) {
        waiting(true);
        lock.lock();
        waiting(false);
    }
    return lock;
}

QString hiveWaitText() {
    return QCoreApplication::translate("avel::app::HiveAccess",
                                       "Waiting for a scan elsewhere in Avel to finish…");
}

inventory::Inventory scanInventory(const inventory::ScanOptions& options) {
    const auto hives = sharedHiveAccess();
    return inventory::scan_inventory(options);
}

}  // namespace avel::app
