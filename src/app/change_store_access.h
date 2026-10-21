#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <memory>

#include "core/changes/store.h"

namespace avel::app {

// The GUI's way to the change store (%ProgramData%\Avel\Changes): opened once at startup, shared
// by the removal, undo and Undo Center dialogs.
class ChangeStoreAccess : public QObject {
    Q_OBJECT

public:
    explicit ChangeStoreAccess(QObject* parent = nullptr);
    ~ChangeStoreAccess() override;

    // Null when Avel can't remove or restore anything; unavailableReason() says why.
    std::shared_ptr<changes::ChangeStore> store() const { return store_; }
    QString unavailableReason() const { return reason_; }

    // On a worker thread: settles sets an interrupted run left unfinished (a crash, a power cut)
    // and deletes sets older than 30 days, as every avel-cli run does.
    void startMaintenance();

Q_SIGNALS:
    // `settled`: the ids of the interrupted sets it settled.
    void maintenanceFinished(const QStringList& settled);

private:
    std::shared_ptr<changes::ChangeStore> store_;
    QString reason_;
    // Declared last so it's destroyed first: its destructor waits for the worker.
    QThreadPool pool_;
};

}  // namespace avel::app
