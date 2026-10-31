#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <memory>
#include <string>
#include <vector>

#include "core/force/force.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/tracker/trace_store.h"
#include "core/uninstall/uninstall_plan.h"
#include "leftover_model.h"

class QWidget;

namespace avel::app {

class ChangeStoreAccess;
class TraceStoreAccess;

// Whether Avel Force is the way to remove a program the normal uninstall can't: no uninstaller is
// registered, it is missing or isn't something Avel can run, or Windows Installer has lost the
// package.
bool forceCanHelp(uninstall::PlanError error);

// Avel Force in the GUI. It never runs a program's uninstaller:
//   1. on a worker thread, it reads the installed programs afresh and scans for everything that
//      belongs to the programs (or the folder, or the install trace): their folder, Uninstall key,
//      Windows Installer registration and leftovers, and, for a program installed with Avel
//      Trace, what its trace saw the installer create and its edits to Path and SharedDLLs
//      (reverting by the trace, as avel-cli force does);
//   2. the leftover dialog shows it for review ("Forced removal of X"), with "Revert by the
//      install trace" to leave the trace out;
//   3. what runs from the programs' folders is listed, and ended once the user agrees;
//   4. the checked items go as one change set, which the Undo Center can put back; then the
//      trace store's retention runs.
// The processes are only ended after the review, so nothing is closed for a removal the user
// then cancels. Emits finished() at the end, whatever happened, then deletes itself.
class ForceFlow : public QObject {
    Q_OBJECT

public:
    struct Request {
        std::vector<std::wstring> programIds;  // Installed programs
        // Or a folder no Uninstall entry describes, and what its other leftovers are named after
        // (the folder's name when empty).
        std::wstring folder;
        std::wstring name;
        std::wstring publisher;
        // Or an install trace, by its id (avel-cli force --trace <id>): what it recorded is
        // reverted, also when its program's entry is gone or the trace was imported. Its program,
        // while installed, is the target; otherwise what its link last recorded stands in.
        std::wstring traceId;
        // Programs installed with Avel Trace are reverted by their trace (off: --no-trace).
        bool useTraces = true;
    };

    // `stores` and `traces` must outlive the flow; `window` parents its dialogs.
    ForceFlow(Request request, ChangeStoreAccess* stores, TraceStoreAccess* traces, QWidget* window);
    ~ForceFlow() override;

    void start();

Q_SIGNALS:
    // A line for the status bar while the flow works in the background; empty clears it.
    void progress(const QString& message);
    // `changedAnything`: something was removed, so the program list needs refreshing.
    void finished(bool changedAnything);

private:
    // What the first step found.
    struct Scan {
        std::shared_ptr<const inventory::Inventory> inventory;
        std::vector<leftovers::ScanTarget> targets;  // As built, for the deep search to start from
        std::shared_ptr<const leftovers::ScanResult> scan;
        std::vector<LeftoverModel::Program> programs;
        force::FolderError folderError = force::FolderError::None;
        leftovers::ProtectionReason protection = leftovers::ProtectionReason::None;
        // A trace request whose trace can't be used: why (None also when the store is missing).
        bool traceFailed = false;
        tracker::TraceError traceError = tracker::TraceError::None;
    };

    // `traces` is null when the trace store can't be used.
    static Scan scan(const Request& request, std::shared_ptr<const tracker::TraceStore> traces);
    void onScanned(std::shared_ptr<Scan> scan);
    void onReviewed(const changes::ChangePlan& plan, bool allowIrreversible);
    void onProcessesListed(std::vector<platform::ProcessInfo> processes);
    void remove();
    void finish(bool changedAnything);
    QString folderErrorText(const Scan& scan) const;
    QString traceErrorText(const Scan& scan) const;
    void showError(const QString& text, bool changedAnything);

    Request request_;
    ChangeStoreAccess* stores_ = nullptr;
    TraceStoreAccess* traces_ = nullptr;
    QWidget* window_ = nullptr;
    std::shared_ptr<Scan> scan_;
    QString subject_;
    changes::ChangePlan plan_;
    bool allowIrreversible_ = false;
    // Declared last so it's destroyed first: its destructor waits for a running step.
    QThreadPool pool_;
};

}  // namespace avel::app
