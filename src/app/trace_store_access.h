#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <memory>

#include "core/tracker/trace_store.h"

namespace avel::app {

// The GUI's way to the trace store (%ProgramData%\Avel\Traces): opened once at startup, shared
// by tracing, the trace review, the Traces dialog, and uninstalling and forcing by trace.
class TraceStoreAccess : public QObject {
    Q_OBJECT

public:
    explicit TraceStoreAccess(QObject* parent = nullptr);
    ~TraceStoreAccess() override;

    // Null when Avel can't trace or use traces; unavailableReason() says why.
    std::shared_ptr<tracker::TraceStore> store() const { return store_; }
    QString unavailableReason() const { return reason_; }

    // On a worker thread: removes traces that died while starting, tidies finished ones, and
    // applies retention with a fresh inventory (TraceStore::purge with refresh_links), as avel-cli
    // does after removing programs. Run it at startup and after programs were removed.
    void startMaintenance();

    // Whoever adds, changes or removes a trace calls this, so views that show traces refresh.
    void notifyChanged() { Q_EMIT tracesChanged(); }

Q_SIGNALS:
    // `deleted`: how many traces the purge deleted (tracesChanged() follows when there were any).
    void maintenanceFinished(int deleted);
    void tracesChanged();

private:
    std::shared_ptr<tracker::TraceStore> store_;
    QString reason_;
    // Declared last so it's destroyed first: its destructor waits for the worker.
    QThreadPool pool_;
};

}  // namespace avel::app
