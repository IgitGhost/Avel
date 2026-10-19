#include "inventory_loader.h"

namespace avel::app {

InventoryLoader::InventoryLoader(QObject* parent) : QObject(parent) { pool_.setMaxThreadCount(1); }

InventoryLoader::~InventoryLoader() { pool_.waitForDone(); }

void InventoryLoader::start() {
    if (running_) {
        return;
    }
    running_ = true;
    Q_EMIT started();
    pool_.start([this] {
        // scan_inventory runs every source on its own worker thread with the COM apartment it
        // needs, so this pool thread's apartment doesn't matter.
        auto inventory = std::make_shared<const inventory::Inventory>(inventory::scan_inventory());
        QMetaObject::invokeMethod(
            this,
            [this, inventory = std::move(inventory)] {
                running_ = false;
                Q_EMIT finished(inventory);
            },
            Qt::QueuedConnection);
    });
}

}  // namespace avel::app
