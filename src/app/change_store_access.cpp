#include "change_store_access.h"

#include "core/changes/executor.h"
#include "core/platform/elevation.h"
#include "leftover_text.h"
#include "program_text.h"

namespace avel::app {

ChangeStoreAccess::ChangeStoreAccess(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
    if (!platform::is_process_elevated()) {
        reason_ = tr("Removing and restoring leftovers needs administrator rights.");
        return;
    }
    auto options = changes::default_store_options();
    const QString root = toQString(options.root);
    platform::FolderStatus status{};
    auto store = changes::ChangeStore::open(std::move(options), status);
    if (!store) {
        reason_ = tr("Avel can't trust its backup folder %1: %2. Nothing can be removed until that is fixed.")
                      .arg(root, LeftoverText::folderStatusText(status));
        return;
    }
    store_ = std::move(store);
}

ChangeStoreAccess::~ChangeStoreAccess() { pool_.waitForDone(); }

void ChangeStoreAccess::startMaintenance() {
    if (!store_) {
        return;
    }
    pool_.start([this, store = store_] {
        QStringList settled;
        // Another Avel process at work does this itself when it's done.
        if (const auto guard = store->lock(std::chrono::seconds(5))) {
            for (const auto& id : changes::recover(*store)) {
                settled << toQString(id);
            }
            store->purge();
        }
        QMetaObject::invokeMethod(
            this, [this, settled] { Q_EMIT maintenanceFinished(settled); }, Qt::QueuedConnection);
    });
}

}  // namespace avel::app
