#pragma once

#include <QObject>
#include <QThreadPool>

#include <memory>

#include "core/inventory/inventory.h"

namespace avel::app {

// Runs the inventory scan off the GUI thread and hands the result back on it.
class InventoryLoader : public QObject {
    Q_OBJECT

public:
    explicit InventoryLoader(QObject* parent = nullptr);
    ~InventoryLoader() override;

    bool isRunning() const { return running_; }
    // Starts a scan unless one is already running.
    void start();

Q_SIGNALS:
    void started();
    void finished(std::shared_ptr<const avel::inventory::Inventory> inventory);

private:
    bool running_ = false;
    // Declared last so it's destroyed first: its destructor waits for a running scan while the
    // rest of this object is still alive.
    QThreadPool pool_;
};

}  // namespace avel::app
