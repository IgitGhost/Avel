#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/changes/executor.h"
#include "core/tools/windows_apps.h"
#include "windows_apps_text.h"

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTableView;
class QTextBrowser;
class QToolBar;

namespace avel::app {

class ChangeStoreAccess;
class WindowsAppsFilterModel;
class WindowsAppsTableModel;
struct WindowsAppsListing;

// Avel Tools' Windows apps (Tools > Windows apps...): the packaged apps on this PC, one per package
// family, in one sortable table with check boxes and the selected app's details below it. Three
// actions work on the checked apps (or, when none is checked, the selected ones), each as one
// ChangeSet for all the apps it fits: "Remove for me" and "Don't install for new users" apply at
// once and can be undone here (Ctrl+Z) or in the Undo Center; "Remove for everyone" can't be undone
// and asks first. Protected apps are listed greyed, never changed. A window of its own, not a
// dialog: the main window stays usable while it is open. See docs/tools.md ("Windows apps").
class WindowsAppsWindow : public QMainWindow {
    Q_OBJECT

public:
    // `stores` must outlive the window.
    explicit WindowsAppsWindow(ChangeStoreAccess* stores, QWidget* parent = nullptr);
    ~WindowsAppsWindow() override;

    // Lists the apps again (F5), unless a listing or a change is under way.
    void refresh();

Q_SIGNALS:
    // Apps were removed or put back (here or from the Undo Center opened here): the installed
    // programs may differ.
    void appsChanged();

protected:
    void changeEvent(QEvent* event) override;
    // Refused while a change runs: it is backed up and recorded step by step, and finishes first.
    void closeEvent(QCloseEvent* event) override;

private:
    // A ChangeSet this window runs, and what the result is told against.
    struct Change {
        WindowsAppsAction action = WindowsAppsAction::RemoveForMe;
        changes::ChangePlan plan;
        struct Planned {
            QString name;
            std::wstring family;
            std::vector<std::wstring> fullNames;  // Every version of the family, for its items
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

    void onListed(std::shared_ptr<const WindowsAppsListing> listing);
    void populateUserFilter();
    void onSelectionChanged();
    void updateActions();
    void updateCounts();
    void updateDiagnostics();
    void renderDetails();
    std::vector<const tools::WindowsApp*> selectedApps() const;
    // The current row when it is selected, otherwise the first selected one.
    const tools::WindowsApp* currentApp() const;
    // What the actions work on: the checked apps, or the selected ones when none is checked.
    std::vector<const tools::WindowsApp*> targetApps() const;
    void selectIds(const std::vector<std::wstring>& ids, const std::wstring& current);
    void checkOftenRemoved();

    std::shared_ptr<Change> planChange(WindowsAppsAction action) const;
    void apply(WindowsAppsAction action);
    void confirmRemoveForEveryone();
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
    void openInstallFolder();
    void copyFullName();
    void copyFamilyName();

    ChangeStoreAccess* stores_;
    Work work_ = Work::None;
    // The undoable sets this window made, newest last, for "Undo last change".
    struct Done {
        std::wstring setId;
        std::shared_ptr<const Change> change;
    };
    std::vector<Done> undoStack_;

    WindowsAppsTableModel* model_ = nullptr;
    WindowsAppsFilterModel* filter_ = nullptr;

    QAction* refreshAction_ = nullptr;
    QAction* checkOftenRemovedAction_ = nullptr;
    QAction* uncheckAllAction_ = nullptr;
    QAction* removeForMeAction_ = nullptr;
    QAction* deprovisionAction_ = nullptr;
    QAction* removeForEveryoneAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* undoCenterAction_ = nullptr;
    QAction* openFolderAction_ = nullptr;
    QAction* copyFullNameAction_ = nullptr;
    QAction* copyFamilyNameAction_ = nullptr;
    QAction* findAction_ = nullptr;

    QToolBar* toolBar_ = nullptr;
    QToolBar* filterBar_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* categoryCombo_ = nullptr;
    QComboBox* userCombo_ = nullptr;
    QLabel* banner_ = nullptr;
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
