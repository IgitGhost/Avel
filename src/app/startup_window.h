#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/executor.h"
#include "core/platform/registry_address.h"
#include "core/tools/startup.h"
#include "core/tools/startup_changes.h"

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTableView;
class QTextBrowser;
class QToolBar;

namespace avel::inventory {
struct Inventory;
}

namespace avel::app {

class ChangeStoreAccess;
class StartupFilterModel;
class StartupTableModel;
struct StartupListing;

// Avel Tools' Startup manager (Tools > Startup...): everything that starts with Windows, for the
// machine and every user, in one sortable table with the selected entry's details below it.
// Disable and Enable apply at once, Delete asks first; each action is one ChangeSet for all the
// selected entries it fits, so the Undo Center (and Ctrl+Z here) can put it back. A window of its
// own, not a dialog: the main window stays usable while it is open.
class StartupWindow : public QMainWindow {
    Q_OBJECT

public:
    // `stores` must outlive the window. `inventory` names the owner programs; when it is null the
    // listing scans the installed programs itself.
    StartupWindow(ChangeStoreAccess* stores, std::shared_ptr<const inventory::Inventory> inventory,
                  QWidget* parent = nullptr);
    ~StartupWindow() override;

    // The main window's latest inventory, for the next listing.
    void setInventory(std::shared_ptr<const inventory::Inventory> inventory);
    // Lists the entries again (F5), unless a listing or a change is under way.
    void refresh();

Q_SIGNALS:
    // "Go to program": show the owner program in the main window.
    void programRequested(const QString& programId);
    // The Undo Center opened from here put something back: the installed programs may differ.
    void changesRestored();

protected:
    void changeEvent(QEvent* event) override;
    // Refused while a change runs: it is backed up and recorded step by step, and finishes first.
    void closeEvent(QCloseEvent* event) override;

private:
    // A ChangeSet this window runs, and what the result is told against.
    struct Change {
        tools::StartupAction action = tools::StartupAction::Disable;
        tools::ServiceDisable serviceDisable = tools::ServiceDisable::Manual;
        changes::ChangePlan plan;
        struct Planned {
            QString name;
            std::size_t items = 0;  // Its plan items, in order
        };
        std::vector<Planned> planned;
        QStringList skipped;  // "Name: why"
    };
    enum class Work { None, Listing, Changing, Undoing };

    void createActions();
    void createToolBar();
    void createCentralArea();
    void createStatusBar();
    void retranslateUi();
    void restoreLayout();
    void saveLayout() const;

    void onListed(std::shared_ptr<const StartupListing> listing);
    void populateUserFilter();
    void onSelectionChanged();
    void updateActions();
    void updateCounts();
    void updateDiagnostics();
    void renderDetails();
    std::vector<const tools::StartupEntry*> selectedEntries() const;
    // The current row when it is selected, otherwise the first selected one.
    const tools::StartupEntry* currentEntry() const;
    void selectIds(const std::vector<std::wstring>& ids, const std::wstring& current);

    std::shared_ptr<Change> planChange(tools::StartupAction action,
                                       tools::ServiceDisable serviceDisable) const;
    void apply(tools::StartupAction action, tools::ServiceDisable serviceDisable);
    void confirmDelete();
    void startChange(std::shared_ptr<const Change> change);
    void onChangeFinished(const std::shared_ptr<const Change>& change, bool busy,
                          const changes::ExecuteResult& result);
    void undoLast();
    void onUndoFinished(const std::shared_ptr<const Change>& change, const std::wstring& setId, bool busy,
                        const changes::RestoreResult& result);
    // While a change waits for a scan elsewhere in Avel to let go of users' hives.
    void onHiveWait(bool waiting);
    void openUndoCenter();
    // The status bar's result line; `details` go to its tooltip.
    void showResult(const QString& text, const QStringList& details = {}, bool problem = false);

    void showTableMenu(const QPoint& position);
    void openFileLocation();
    void copyCommand();
    void goToProgram();
    void openInRegistryEditor();
    static QString fileToShow(const tools::StartupEntry& entry);
    static std::optional<platform::RegistryAddress> registryKeyOf(const tools::StartupEntry& entry);

    ChangeStoreAccess* stores_;
    std::shared_ptr<const inventory::Inventory> inventory_;
    tools::SignerFunction signer_;  // Its cache keeps the signatures across refreshes
    Work work_ = Work::None;
    // The sets this window made, newest last, for "Undo last change".
    struct Done {
        std::wstring setId;
        std::shared_ptr<const Change> change;
    };
    std::vector<Done> undoStack_;

    StartupTableModel* model_ = nullptr;
    StartupFilterModel* filter_ = nullptr;

    QAction* refreshAction_ = nullptr;
    QAction* enableAction_ = nullptr;
    QAction* disableAction_ = nullptr;
    QAction* disableCompletelyAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* undoCenterAction_ = nullptr;
    QAction* openLocationAction_ = nullptr;
    QAction* copyCommandAction_ = nullptr;
    QAction* goToProgramAction_ = nullptr;
    QAction* openRegistryAction_ = nullptr;
    QAction* openTaskSchedulerAction_ = nullptr;
    QAction* openServicesAction_ = nullptr;
    QAction* findAction_ = nullptr;

    QToolBar* toolBar_ = nullptr;
    QToolBar* filterBar_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* sourceCombo_ = nullptr;
    QComboBox* userCombo_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTableView* table_ = nullptr;
    QTextBrowser* details_ = nullptr;

    QLabel* resultLabel_ = nullptr;
    QLabel* workLabel_ = nullptr;
    QString workText_;  // The work label's text, shown again once the wait for the hives is over
    QLabel* warningLabel_ = nullptr;
    QLabel* countLabel_ = nullptr;

    // Declared last so it's destroyed first: its destructor waits for a running listing or change.
    QThreadPool pool_;
};

}  // namespace avel::app
