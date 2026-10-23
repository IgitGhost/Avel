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
#include "core/uninstall/uninstall_plan.h"
#include "leftover_model.h"

class QWidget;

namespace avel::app {

class ChangeStoreAccess;

// Whether Avel Force is the way to remove a program the normal uninstall can't: no uninstaller is
// registered, it is missing or isn't something Avel can run, or Windows Installer has lost the
// package.
bool forceCanHelp(uninstall::PlanError error);

// Avel Force in the GUI. It never runs a program's uninstaller:
//   1. on a worker thread, it reads the installed programs afresh and scans for everything that
//      belongs to the programs (or the folder): their folder, Uninstall key, Windows Installer
//      registration and leftovers;
//   2. the leftover dialog shows it for review ("Forced removal of X");
//   3. what runs from the programs' folders is listed, and ended once the user agrees;
//   4. the checked items go as one change set, which the Undo Center can put back.
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
    };

    // `stores` must outlive the flow; `window` parents its dialogs.
    ForceFlow(Request request, ChangeStoreAccess* stores, QWidget* window);
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
    };

    static Scan scan(const Request& request);
    void onScanned(std::shared_ptr<Scan> scan);
    void onReviewed(const changes::ChangePlan& plan, bool allowIrreversible);
    void onProcessesListed(std::vector<platform::ProcessInfo> processes);
    void remove();
    void finish(bool changedAnything);
    QString folderErrorText(const Scan& scan) const;

    Request request_;
    ChangeStoreAccess* stores_ = nullptr;
    QWidget* window_ = nullptr;
    std::shared_ptr<Scan> scan_;
    QString subject_;
    changes::ChangePlan plan_;
    bool allowIrreversible_ = false;
    // Declared last so it's destroyed first: its destructor waits for a running step.
    QThreadPool pool_;
};

}  // namespace avel::app
