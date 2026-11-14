#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/executor.h"
#include "core/tools/browser_extensions.h"
#include "core/tools/browser_extensions_changes.h"

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTableView;
class QTextBrowser;
class QToolBar;
class QUrl;

namespace avel::inventory {
struct Inventory;
}

namespace avel::app {

class ChangeStoreAccess;
class ExtensionsFilterModel;
class ExtensionsTableModel;
struct ExtensionsListing;

// Avel Tools' browser extensions (Tools > Browser extensions...): every extension of every Chromium
// and Firefox profile of every user, in one sortable table with the selected entry's details below
// it. Remove takes out the policy entries and registrations that install the selected extensions
// (only those: ADR 0009), after a confirmation that lists them and every other profile the same
// sources install the extension in, as one ChangeSet; the browser drops the extension at its next
// start. Undo here (Ctrl+Z) or in the Undo Center. An extension nothing outside the browser installs
// is removed on the browser's own extensions page, which "Open extensions page" opens, unelevated.
// A window of its own, not a dialog: the main window stays usable while it is open. See docs/tools.md
// ("Browser extensions").
class ExtensionsWindow : public QMainWindow {
    Q_OBJECT

public:
    // `stores` must outlive the window. `inventory` names the programs that registered extensions;
    // when it is null the listing scans the installed programs itself.
    ExtensionsWindow(ChangeStoreAccess* stores, std::shared_ptr<const inventory::Inventory> inventory,
                     QWidget* parent = nullptr);
    ~ExtensionsWindow() override;

    // The main window's latest inventory, for the next listing.
    void setInventory(std::shared_ptr<const inventory::Inventory> inventory);
    // Lists the extensions again (F5), unless a listing or a change is under way.
    void refresh();

Q_SIGNALS:
    // The Undo Center opened from here put something back: the installed programs may differ.
    void changesRestored();

protected:
    void changeEvent(QEvent* event) override;
    // Refused while a change runs: it is backed up and recorded step by step, and finishes first.
    void closeEvent(QCloseEvent* event) override;

private:
    // A ChangeSet this window runs, and what the result is told against.
    struct Change {
        changes::ChangePlan plan;
        int extensions = 0;   // The extensions planned (an extension in several profiles counts once)
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

    void onListed(std::shared_ptr<const ExtensionsListing> listing);
    void populateFilters();
    void onSelectionChanged();
    void updateActions();
    void updateCounts();
    void updateDiagnostics();
    void renderDetails();
    std::vector<const tools::BrowserExtension*> selectedEntries() const;
    // The current row when it is selected, otherwise the first selected one.
    const tools::BrowserExtension* currentEntry() const;
    void selectIds(const std::vector<std::wstring>& ids, const std::wstring& current);

    void confirmRemove();
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

    // Why the entry's extensions page can't be opened from here; empty when it can.
    QString pageUnavailableReason(const tools::BrowserExtension& entry) const;
    // The policies.json that manages the entry, if one does.
    static QString policiesJsonOf(const tools::BrowserExtension& entry,
                                  const tools::BrowserExtensionsList& list);
    // The first source of the entry that has a registry key.
    static const tools::ExtensionSource* firstRegistrySource(const tools::BrowserExtension& entry,
                                                             const tools::BrowserExtensionsList& list);
    void showTableMenu(const QPoint& position);
    void openExtensionsPage();
    void openPoliciesJson();
    void openProfileFolder();
    void copyExtensionId();
    void openInRegistryEditor(const tools::ExtensionSource& source);
    void onDetailsLink(const QUrl& link);

    ChangeStoreAccess* stores_;
    std::shared_ptr<const inventory::Inventory> inventory_;
    Work work_ = Work::None;
    // The sets this window made, newest last, for "Undo last change".
    struct Done {
        std::wstring setId;
        std::shared_ptr<const Change> change;
    };
    std::vector<Done> undoStack_;

    ExtensionsTableModel* model_ = nullptr;
    ExtensionsFilterModel* filter_ = nullptr;

    QAction* refreshAction_ = nullptr;
    QAction* removeAction_ = nullptr;
    QAction* openPageAction_ = nullptr;
    QAction* openPoliciesJsonAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* undoCenterAction_ = nullptr;
    QAction* copyIdAction_ = nullptr;
    QAction* openRegistryAction_ = nullptr;
    QAction* openProfileAction_ = nullptr;
    QAction* findAction_ = nullptr;

    QToolBar* toolBar_ = nullptr;
    QToolBar* filterBar_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* browserCombo_ = nullptr;
    QComboBox* userCombo_ = nullptr;
    QCheckBox* removableCheck_ = nullptr;
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
