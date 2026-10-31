#include "trace_store_access.h"

#include "core/inventory/inventory.h"
#include "core/platform/elevation.h"
#include "core/tracker/linking.h"
#include "leftover_text.h"
#include "program_text.h"

namespace avel::app {

TraceStoreAccess::TraceStoreAccess(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
    if (!platform::is_process_elevated()) {
        reason_ = tr("Tracing installations and using their traces needs administrator rights.");
        return;
    }
    auto options = tracker::default_trace_store_options();
    const QString root = toQString(options.root);
    platform::FolderStatus status{};
    auto store = tracker::TraceStore::open(std::move(options), status);
    if (!store) {
        reason_ = tr("Avel can't trust its trace folder %1: %2. Nothing can be traced until that is fixed.")
                      .arg(root, LeftoverText::folderStatusText(status));
        return;
    }
    store_ = std::move(store);
}

TraceStoreAccess::~TraceStoreAccess() { pool_.waitForDone(); }

void TraceStoreAccess::startMaintenance() {
    if (!store_) {
        return;
    }
    pool_.start([this, store = store_] {
        inventory::ScanOptions scan;
        scan.appx = false;  // Avel doesn't trace packaged apps
        const auto inventory = inventory::scan_inventory(scan);
        // Without an inventory every linked trace would look uninstalled: then only tidy.
        const std::size_t deleted =
            inventory.programs.empty() ? store->purge() : store->purge([&inventory](tracker::TraceInfo& info) {
                return tracker::refresh_links(info, inventory);
            });
        QMetaObject::invokeMethod(
            this,
            [this, deleted] {
                Q_EMIT maintenanceFinished(static_cast<int>(deleted));
                if (deleted > 0) {
                    Q_EMIT tracesChanged();
                }
            },
            Qt::QueuedConnection);
    });
}

}  // namespace avel::app
