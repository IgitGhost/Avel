#include "windows_apps_window.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QTextBrowser>
#include <QToolBar>
#include <QVBoxLayout>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <variant>

#include "change_store_access.h"
#include "core/platform/text.h"
#include "core/tools/windows_apps_changes.h"
#include "hive_access.h"
#include "leftover_text.h"
#include "menu_copy.h"
#include "program_text.h"
#include "undo_center_dialog.h"
#include "windows_apps_model.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::AppProtection;
using tools::WindowsApp;

namespace {

// Settings keys for the window layout.
constexpr auto kGeometryKey = "windowsApps/geometry"_L1;
constexpr auto kSplitterKey = "windowsApps/splitter"_L1;
constexpr auto kHeaderKey = "windowsApps/header"_L1;

// The user filter's special entries; never a SID.
constexpr auto kAllUsers = "*"_L1;
constexpr auto kNewUsers = "+"_L1;

// The user filter's entry: kAllUsers, kNewUsers or a SID.
WindowsAppsFilterModel::User userFilter(const QString& entry) {
    using Kind = WindowsAppsFilterModel::User::Kind;
    if (entry == kAllUsers) {
        return {};
    }
    if (entry == kNewUsers) {
        return {Kind::NewUsers, {}};
    }
    return {Kind::Sid, entry.toStdWString()};
}

// A change waits this long for another Avel process changing the system.
constexpr auto kLockTimeout = std::chrono::seconds(30);

// The confirmation lists this many apps, then "... and n more".
constexpr int kMaxAppsShown = 12;

// Done, or left to go with the item that contains it.
bool finished(const changes::ItemStatus& status) {
    return status.state == changes::ItemState::Done ||
           (status.state == changes::ItemState::Skipped && status.reason == changes::Reason::Contained);
}

QString escaped(const std::wstring& text) { return toQString(text).toHtmlEscaped(); }

QString code(const std::wstring& text) { return u"<code>"_s + escaped(text) + u"</code>"_s; }

QString gray(const QString& text) {
    return u"<span style=\"color: gray\">"_s + text.toHtmlEscaped() + u"</span>"_s;
}

// The full name to show and copy: the current user's package, else the newest.
std::wstring fullNameOf(const WindowsApp& app) {
    if (!app.current_user_full_name.empty()) {
        return app.current_user_full_name;
    }
    return app.packages.empty() ? std::wstring() : app.packages.front().full_name;
}

const WindowsApp* findApp(const tools::WindowsAppsList& list, const std::wstring& id) {
    const auto found = std::find_if(list.apps.begin(), list.apps.end(),
                                    [&](const WindowsApp& app) { return platform::iequals(app.id, id); });
    return found != list.apps.end() ? &*found : nullptr;
}

tools::WindowsAppsPlan plan(WindowsAppsAction action, const tools::WindowsAppsList& list,
                            std::span<const std::wstring> ids, bool irreversibleConfirmed) {
    switch (action) {
        case WindowsAppsAction::Deprovision:
            return tools::plan_deprovision(list, ids);
        case WindowsAppsAction::RemoveForEveryone:
            return tools::plan_remove_for_everyone(list, ids, irreversibleConfirmed);
        case WindowsAppsAction::RemoveForMe:
            break;
    }
    return tools::plan_remove_for_me(list, ids);
}

}  // namespace

WindowsAppsWindow::WindowsAppsWindow(ChangeStoreAccess* stores, QWidget* parent)
    : QMainWindow(parent), stores_(stores) {
    setObjectName(u"windowsAppsWindow"_s);
    pool_.setMaxThreadCount(1);

    model_ = new WindowsAppsTableModel(this);
    filter_ = new WindowsAppsFilterModel(this);
    filter_->setSourceAppsModel(model_);

    createActions();
    createToolBar();
    createCentralArea();
    createStatusBar();
    retranslateUi();
    restoreLayout();

    connect(filter_, &QAbstractItemModel::rowsInserted, this, &WindowsAppsWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, &WindowsAppsWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::modelReset, this, &WindowsAppsWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::layoutChanged, this, &WindowsAppsWindow::updateCounts);
    connect(model_, &WindowsAppsTableModel::checksChanged, this, [this] {
        updateCounts();
        onSelectionChanged();
    });

    refresh();
}

WindowsAppsWindow::~WindowsAppsWindow() = default;

void WindowsAppsWindow::createActions() {
    const auto make = [this](QStyle::StandardPixmap icon, const QString& name, auto slot) {
        auto* action = icon == QStyle::SP_CustomBase
                           ? new QAction(this)
                           : new QAction(style()->standardIcon(icon), QString(), this);
        action->setObjectName(name);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);  // Its shortcut works anywhere in the window
        return action;
    };
    refreshAction_ = make(QStyle::SP_BrowserReload, u"windowsAppsRefreshAction"_s, [this] { refresh(); });
    refreshAction_->setShortcut(QKeySequence::Refresh);
    checkOftenRemovedAction_ =
        make(QStyle::SP_DialogApplyButton, u"checkOftenRemovedAction"_s, [this] { checkOftenRemoved(); });
    uncheckAllAction_ = make(QStyle::SP_CustomBase, u"uncheckAllAction"_s, [this] { model_->uncheckAll(); });
    removeForMeAction_ =
        make(QStyle::SP_TrashIcon, u"removeForMeAction"_s, [this] { apply(WindowsAppsAction::RemoveForMe); });
    deprovisionAction_ = make(QStyle::SP_CustomBase, u"deprovisionAction"_s,
                              [this] { apply(WindowsAppsAction::Deprovision); });
    removeForEveryoneAction_ = make(QStyle::SP_MessageBoxWarning, u"removeForEveryoneAction"_s,
                                    [this] { confirmRemoveForEveryone(); });
    undoAction_ = make(QStyle::SP_ArrowBack, u"windowsAppsUndoAction"_s, [this] { undoLast(); });
    undoAction_->setShortcut(QKeySequence::Undo);
    undoCenterAction_ =
        make(QStyle::SP_CustomBase, u"windowsAppsUndoCenterAction"_s, [this] { openUndoCenter(); });
    openFolderAction_ =
        make(QStyle::SP_DirOpenIcon, u"windowsAppsOpenFolderAction"_s, [this] { openInstallFolder(); });
    copyFullNameAction_ =
        make(QStyle::SP_CustomBase, u"windowsAppsCopyFullNameAction"_s, [this] { copyFullName(); });
    copyFamilyNameAction_ =
        make(QStyle::SP_CustomBase, u"windowsAppsCopyFamilyNameAction"_s, [this] { copyFamilyName(); });
    findAction_ = make(QStyle::SP_CustomBase, u"windowsAppsFindAction"_s, [this] {
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->selectAll();
    });
    findAction_->setShortcut(QKeySequence::Find);
}

void WindowsAppsWindow::createToolBar() {
    toolBar_ = addToolBar(QString());
    toolBar_->setObjectName(u"windowsAppsToolBar"_s);
    toolBar_->setMovable(false);
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar_->addAction(refreshAction_);
    toolBar_->addSeparator();
    toolBar_->addAction(checkOftenRemovedAction_);
    toolBar_->addAction(uncheckAllAction_);
    toolBar_->addSeparator();
    for (QAction* action : {removeForMeAction_, deprovisionAction_, removeForEveryoneAction_}) {
        toolBar_->addAction(action);
    }
    toolBar_->addSeparator();
    toolBar_->addAction(undoAction_);
    toolBar_->addAction(undoCenterAction_);
    // The buttons are found by name in UI Automation: "removeForMeAction" -> "removeForMeButton".
    for (QAction* action : toolBar_->actions()) {
        if (QWidget* button = toolBar_->widgetForAction(action);
            button != nullptr && !action->isSeparator()) {
            button->setObjectName(action->objectName().replace(u"Action"_s, u"Button"_s));
        }
    }

    addToolBarBreak();
    filterBar_ = addToolBar(QString());
    filterBar_->setObjectName(u"windowsAppsFilterBar"_s);
    filterBar_->setMovable(false);

    searchEdit_ = new QLineEdit(filterBar_);
    searchEdit_->setObjectName(u"windowsAppsSearch"_s);
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(300);
    connect(searchEdit_, &QLineEdit::textChanged, filter_, &WindowsAppsFilterModel::setSearchText);
    filterBar_->addWidget(searchEdit_);

    using Category = WindowsAppsFilterModel::Category;
    categoryCombo_ = new QComboBox(filterBar_);
    categoryCombo_->setObjectName(u"windowsAppsCategoryFilter"_s);
    categoryCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (const Category category : {Category::All, Category::Removable, Category::OftenRemoved,
                                    Category::SemiCritical, Category::Protected}) {
        categoryCombo_->addItem(QString(), static_cast<int>(category));
    }
    connect(categoryCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        filter_->setCategory(static_cast<Category>(categoryCombo_->itemData(index).toInt()));
        updateCounts();
    });
    filterBar_->addWidget(categoryCombo_);

    userCombo_ = new QComboBox(filterBar_);
    userCombo_->setObjectName(u"windowsAppsUserFilter"_s);
    userCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    userCombo_->addItem(QString(), QString(kAllUsers));
    userCombo_->addItem(QString(), QString(kNewUsers));
    connect(userCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        filter_->setUser(userFilter(userCombo_->itemData(index).toString()));
        updateCounts();
    });
    filterBar_->addWidget(userCombo_);
}

void WindowsAppsWindow::createCentralArea() {
    banner_ = new QLabel(this);
    banner_->setObjectName(u"windowsAppsBanner"_s);
    banner_->setWordWrap(true);
    banner_->setStyleSheet(u"QLabel { background-color: #fff4ce; color: #3b2300; padding: 6px; }"_s);
    banner_->hide();

    table_ = new QTableView(this);
    table_->setObjectName(u"windowsAppsTable"_s);
    table_->setModel(filter_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(WindowsAppsTableModel::NameColumn, Qt::AscendingOrder);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    auto* header = table_->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionsMovable(true);
    header->setStretchLastSection(true);  // The category
    header->resizeSection(WindowsAppsTableModel::CheckColumn, 30);
    header->resizeSection(WindowsAppsTableModel::NameColumn, 280);
    header->resizeSection(WindowsAppsTableModel::PublisherColumn, 220);
    header->resizeSection(WindowsAppsTableModel::VersionColumn, 140);
    header->resizeSection(WindowsAppsTableModel::InstalledForColumn, 200);

    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &WindowsAppsWindow::onSelectionChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &WindowsAppsWindow::onSelectionChanged);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, &WindowsAppsWindow::showTableMenu);

    details_ = new QTextBrowser(this);
    details_->setObjectName(u"windowsAppsDetails"_s);
    details_->setOpenLinks(false);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(u"windowsAppsSplitter"_s);
    splitter_->addWidget(table_);
    splitter_->addWidget(details_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({440, 300});

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(banner_);
    layout->addWidget(splitter_, 1);
    setCentralWidget(central);
}

void WindowsAppsWindow::createStatusBar() {
    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(u"windowsAppsResult"_s);
    workLabel_ = new QLabel(this);
    workLabel_->setObjectName(u"windowsAppsProgress"_s);
    warningLabel_ = new QLabel(this);
    warningLabel_->setObjectName(u"windowsAppsWarnings"_s);
    warningLabel_->hide();
    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(u"windowsAppsCount"_s);
    statusBar()->addWidget(workLabel_);
    statusBar()->addWidget(resultLabel_, 1);
    statusBar()->addPermanentWidget(warningLabel_);
    statusBar()->addPermanentWidget(countLabel_);
}

void WindowsAppsWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

void WindowsAppsWindow::closeEvent(QCloseEvent* event) {
    if (work_ == Work::Changing || work_ == Work::Undoing) {
        showResult(tr("Avel is changing apps. Close the window once that is done."));
        event->ignore();
        return;
    }
    saveLayout();
    QMainWindow::closeEvent(event);
}

void WindowsAppsWindow::retranslateUi() {
    setWindowTitle(tr("Windows apps"));
    refreshAction_->setText(tr("&Refresh"));
    refreshAction_->setToolTip(tr("List the apps again (F5)"));
    checkOftenRemovedAction_->setText(tr("Check &often removed"));
    checkOftenRemovedAction_->setToolTip(
        tr("Check the apps people often remove: Windows' promotions and extras such as Clipchamp, the "
           "Bing apps and Solitaire. Nothing changes until you choose an action."));
    uncheckAllAction_->setText(tr("U&ncheck all"));
    removeForMeAction_->setText(WindowsAppsText::actionName(WindowsAppsAction::RemoveForMe));
    removeForMeAction_->setToolTip(
        tr("Remove the checked apps (or, when none is checked, the selected ones) for you only. Undo "
           "installs them for you again; your data in them is lost."));
    deprovisionAction_->setText(WindowsAppsText::actionName(WindowsAppsAction::Deprovision));
    deprovisionAction_->setToolTip(
        tr("Stop installing the checked apps (or, when none is checked, the selected ones) for new user "
           "accounts. Those who have them keep them. Can be undone."));
    removeForEveryoneAction_->setText(WindowsAppsText::actionName(WindowsAppsAction::RemoveForEveryone) +
                                      u"…"_s);
    removeForEveryoneAction_->setToolTip(
        tr("Remove the checked apps (or, when none is checked, the selected ones) for every user and "
           "delete them from this PC. Can't be undone; Avel asks first."));
    undoAction_->setText(tr("&Undo last change"));
    undoCenterAction_->setText(tr("Undo &Center…"));
    undoCenterAction_->setToolTip(tr("Everything Avel changed in the last 30 days, to put back"));
    openFolderAction_->setText(tr("&Open install folder"));
    copyFullNameAction_->setText(tr("Copy &full name"));
    copyFullNameAction_->setToolTip(tr("Copy the package's full name, with its version and architecture"));
    copyFamilyNameAction_->setText(tr("Copy f&amily name"));
    findAction_->setText(tr("Find"));
    toolBar_->setWindowTitle(tr("Toolbar"));
    filterBar_->setWindowTitle(tr("Filters"));
    searchEdit_->setPlaceholderText(tr("Search by name, package or publisher (Ctrl+F)"));
    searchEdit_->setAccessibleName(tr("Search"));

    using Category = WindowsAppsFilterModel::Category;
    categoryCombo_->setAccessibleName(tr("Category"));
    for (int i = 0; i < categoryCombo_->count(); ++i) {
        QString text;
        switch (static_cast<Category>(categoryCombo_->itemData(i).toInt())) {
            case Category::All:
                text = tr("All apps");
                break;
            case Category::Removable:
                text = tr("Can be removed");
                break;
            case Category::OftenRemoved:
                text = WindowsAppsText::categoryText(tools::AppCategory::OftenRemoved);
                break;
            case Category::SemiCritical:
                text = WindowsAppsText::categoryText(tools::AppCategory::SemiCritical);
                break;
            case Category::Protected:
                text = tr("Protected");
                break;
        }
        categoryCombo_->setItemText(i, text);
    }
    userCombo_->setAccessibleName(tr("Installed for"));
    userCombo_->setItemText(0, tr("All users"));
    userCombo_->setItemText(1, tr("New users"));
    populateUserFilter();  // The current user's "(you)"

    model_->retranslate();
    updateActions();
    updateCounts();
    updateDiagnostics();
    renderDetails();
    if (work_ == Work::Listing) {
        workLabel_->setText(tr("Listing apps…"));
    }
}

void WindowsAppsWindow::restoreLayout() {
    const QSettings settings;
    restoreGeometry(settings.value(kGeometryKey).toByteArray());
    if (!settings.contains(kGeometryKey)) {
        resize(1200, 780);
    }
    splitter_->restoreState(settings.value(kSplitterKey).toByteArray());
    table_->horizontalHeader()->restoreState(settings.value(kHeaderKey).toByteArray());
}

void WindowsAppsWindow::saveLayout() const {
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kSplitterKey, splitter_->saveState());
    settings.setValue(kHeaderKey, table_->horizontalHeader()->saveState());
}

// --- Listing -----------------------------------------------------------------------------------

void WindowsAppsWindow::refresh() {
    if (work_ != Work::None) {
        return;
    }
    work_ = Work::Listing;
    workLabel_->setText(tr("Listing apps…"));
    updateActions();
    // The listing asks the package manager only and loads no user's hive, so it takes no hive
    // access (hive_access.h). The pool runs one task at a time: a change of this window never
    // overlaps it.
    pool_.start([this] {
        auto listing = std::make_shared<WindowsAppsListing>();
        const auto started = std::chrono::steady_clock::now();
        listing->list = tools::list_windows_apps(tools::live_windows_apps_environment());
        listing->elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        QMetaObject::invokeMethod(
            this,
            [this, listing = std::shared_ptr<const WindowsAppsListing>(std::move(listing))] {
                onListed(listing);
            },
            Qt::QueuedConnection);
    });
}

void WindowsAppsWindow::onListed(std::shared_ptr<const WindowsAppsListing> listing) {
    work_ = Work::None;
    workLabel_->clear();

    // Keep the selection across a refresh; the model keeps the checks.
    std::vector<std::wstring> selected;
    for (const auto* app : selectedApps()) {
        selected.push_back(app->id);
    }
    const auto* current = currentApp();
    const std::wstring currentId = current != nullptr ? current->id : std::wstring();

    model_->setListing(std::move(listing));
    populateUserFilter();
    selectIds(selected, currentId);
    updateCounts();
    updateDiagnostics();
    onSelectionChanged();  // The model reset cleared the selection without a selectionChanged signal
}

void WindowsAppsWindow::populateUserFilter() {
    const QString chosen = userCombo_->currentData().toString();
    std::map<QString, QString> users;  // By name, then SID
    if (const auto* listing = model_->listing()) {
        for (const auto& app : listing->list.apps) {
            for (const auto& user : app.users) {
                const QString name = toQString(user.account_name.empty() ? user.sid : user.account_name);
                users.emplace(user.current ? tr("%1 (you)").arg(name) : name, toQString(user.sid));
            }
        }
    }
    const QSignalBlocker blocker(userCombo_);
    while (userCombo_->count() > 2) {
        userCombo_->removeItem(2);
    }
    for (const auto& [name, sid] : users) {
        userCombo_->addItem(name, sid);
    }
    const int index = userCombo_->findData(chosen);
    userCombo_->setCurrentIndex(std::max(index, 0));
    filter_->setUser(userFilter(userCombo_->currentData().toString()));
}

void WindowsAppsWindow::selectIds(const std::vector<std::wstring>& ids, const std::wstring& current) {
    QItemSelection selection;
    QModelIndex currentIndex;
    for (const auto& id : ids) {
        const QModelIndex index = filter_->mapFromSource(model_->index(model_->rowOf(id), 0));
        if (!index.isValid()) {
            continue;
        }
        selection.select(index, index);
        if (platform::iequals(id, current)) {
            currentIndex = index;
        }
    }
    if (selection.isEmpty()) {
        return;
    }
    if (!currentIndex.isValid()) {
        currentIndex = selection.indexes().front();
    }
    table_->selectionModel()->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
    table_->selectionModel()->select(selection,
                                     QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table_->scrollTo(currentIndex);
}

std::vector<const WindowsApp*> WindowsAppsWindow::selectedApps() const {
    std::vector<const WindowsApp*> apps;
    QModelIndexList rows = table_->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const QModelIndex& row : rows) {
        if (const auto* app = model_->appAt(filter_->mapToSource(row).row())) {
            apps.push_back(app);
        }
    }
    return apps;
}

const WindowsApp* WindowsAppsWindow::currentApp() const {
    const QItemSelectionModel* selection = table_->selectionModel();
    QModelIndex index = table_->currentIndex();
    if (!index.isValid() || !selection->isRowSelected(index.row())) {
        const QModelIndexList rows = selection->selectedRows();
        index = rows.isEmpty() ? QModelIndex() : rows.front();
    }
    return index.isValid() ? model_->appAt(filter_->mapToSource(index).row()) : nullptr;
}

std::vector<const WindowsApp*> WindowsAppsWindow::targetApps() const {
    auto checked = model_->checkedApps();
    return checked.empty() ? selectedApps() : checked;
}

void WindowsAppsWindow::onSelectionChanged() {
    renderDetails();
    updateActions();
}

void WindowsAppsWindow::checkOftenRemoved() {
    const int added = model_->checkOftenRemoved();
    showResult(added > 0
                   ? tr("%n often removed app(s) checked. Choose an action to remove them.", nullptr, added)
                   : tr("No other often removed app to check."));
}

void WindowsAppsWindow::updateActions() {
    const auto* listing = model_->listing();
    const bool complete = listing != nullptr && listing->list.all_users;
    const auto targets = targetApps();
    const bool idle = work_ == Work::None;
    const bool canChange = idle && complete && stores_->store() != nullptr;

    refreshAction_->setEnabled(idle);
    checkOftenRemovedAction_->setEnabled(complete);
    uncheckAllAction_->setEnabled(!model_->checkedApps().empty());
    removeForMeAction_->setEnabled(
        canChange && WindowsAppsTableModel::fitting(targets, WindowsAppsAction::RemoveForMe) > 0);
    deprovisionAction_->setEnabled(
        canChange && WindowsAppsTableModel::fitting(targets, WindowsAppsAction::Deprovision) > 0);
    removeForEveryoneAction_->setEnabled(
        canChange && WindowsAppsTableModel::fitting(targets, WindowsAppsAction::RemoveForEveryone) > 0);
    undoAction_->setEnabled(idle && stores_->store() != nullptr && !undoStack_.empty());
    undoAction_->setToolTip(
        undoStack_.empty()
            ? tr("Undo the last change made in this window (Ctrl+Z)")
            : tr("Undo “%1” (Ctrl+Z)").arg(toQString(undoStack_.back().change->plan.description)));
    undoCenterAction_->setEnabled(idle);
    if (stores_->store() == nullptr) {
        for (QAction* action : {removeForMeAction_, deprovisionAction_, removeForEveryoneAction_}) {
            action->setToolTip(stores_->unavailableReason());
        }
    }

    const auto* app = currentApp();
    openFolderAction_->setEnabled(app != nullptr && !app->install_location.empty() &&
                                  QFileInfo(toQString(app->install_location)).isDir());
    copyFullNameAction_->setEnabled(app != nullptr);
    copyFamilyNameAction_->setEnabled(app != nullptr);
}

void WindowsAppsWindow::updateCounts() {
    const auto* listing = model_->listing();
    if (listing == nullptr) {
        countLabel_->clear();
        return;
    }
    const int total = model_->rowCount();
    const int shown = filter_->rowCount();
    const auto protectedCount = static_cast<int>(
        std::count_if(listing->list.apps.begin(), listing->list.apps.end(),
                      [](const WindowsApp& app) { return !WindowsAppsTableModel::checkable(app); }));
    QString text = shown == total ? tr("%n app(s)", nullptr, total)
                                  : tr("Showing %1 of %n app(s)", nullptr, total).arg(shown);
    text += u", "_s + tr("%n protected", nullptr, protectedCount);
    text += u", "_s + tr("%n checked", nullptr, static_cast<int>(model_->checkedApps().size()));
    countLabel_->setText(text);
}

void WindowsAppsWindow::updateDiagnostics() {
    const auto* listing = model_->listing();
    // The banner: nothing can be changed from this list.
    if (listing != nullptr && !listing->list.all_users) {
        banner_->setText(u"⚠ "_s + WindowsAppsText::incompleteListingText());
        banner_->show();
    } else {
        banner_->hide();
    }
    QStringList lines;
    if (stores_->store() == nullptr) {
        lines << stores_->unavailableReason();
    }
    if (listing != nullptr && listing->list.error != 0) {
        lines << WindowsAppsText::listingErrorText(listing->list.error);
    }
    if (lines.isEmpty()) {
        warningLabel_->hide();
        return;
    }
    warningLabel_->setText(u"⚠ "_s + tr("%n warning(s)", nullptr, static_cast<int>(lines.size())));
    warningLabel_->setToolTip(lines.join(u'\n'));
    warningLabel_->show();
}

void WindowsAppsWindow::renderDetails() {
    const auto* app = currentApp();
    const auto* listing = model_->listing();
    if (app == nullptr || listing == nullptr) {
        details_->setHtml(u"<p style=\"color: gray\">"_s +
                          (listing == nullptr ? tr("Listing apps…") : tr("Select an app to see its details."))
                              .toHtmlEscaped() +
                          u"</p>"_s);
        return;
    }
    const WindowsApp& a = *app;

    QString rows;
    const auto row = [&rows](const QString& label, const QString& valueHtml) {
        if (!valueHtml.isEmpty()) {
            rows += u"<tr><td style=\"color: gray; padding-right: 12px; white-space: nowrap\">"_s +
                    label.toHtmlEscaped() + u"</td><td>"_s + valueHtml + u"</td></tr>"_s;
        }
    };

    // Every version with who has it; usually one.
    QStringList packages;
    for (const auto& package : a.packages) {
        packages << code(package.full_name);
    }
    row(a.packages.size() > 1 ? tr("Full names") : tr("Full name"), packages.join(u"<br>"_s));
    row(tr("Family"), code(a.family_name));
    row(tr("Publisher"), escaped(a.publisher));
    row(tr("Version"), escaped(a.version));
    row(tr("Install folder"), a.install_location.empty() ? QString() : code(a.install_location));
    QStringList users;
    for (const auto& user : a.users) {
        const QString name = toQString(user.account_name.empty() ? user.sid : user.account_name);
        users << (user.current ? tr("%1 (you)").arg(name) : name).toHtmlEscaped();
    }
    row(tr("Installed for"), users.isEmpty() ? gray(tr("No user")) : users.join(u"<br>"_s));
    if (listing->list.all_users) {
        row(tr("New users"), a.provisioned ? tr("Installed for every new user").toHtmlEscaped() + u"<br>"_s +
                                                 code(a.provisioned_full_name)
                                           : tr("Not installed for new users").toHtmlEscaped());
    } else {
        row(tr("New users"), gray(tr("Not known without administrator rights")));
    }
    QString category = WindowsAppsText::categoryText(a.category).toHtmlEscaped();
    if (a.warning != tools::AppWarning::None) {
        category += u"<br>"_s + gray(WindowsAppsText::warningText(a.warning));
    }
    row(tr("Category"), category);
    row(tr("Signature"), WindowsAppsText::signatureText(a.signature_kind).toHtmlEscaped());
    row(tr("Id"), code(a.id));

    // What each action does for this app, whether it can be undone, and why it isn't offered.
    QString actions;
    if (a.protection == AppProtection::None) {
        const std::vector<std::wstring> ids{a.id};
        for (const auto action : {WindowsAppsAction::RemoveForMe, WindowsAppsAction::Deprovision,
                                  WindowsAppsAction::RemoveForEveryone}) {
            const auto planned = plan(action, listing->list, ids, true);
            QString availability;
            if (planned.issues.empty()) {
                availability =
                    u"<span style=\"color: #107c10\">"_s + tr("Available").toHtmlEscaped() + u"</span>"_s;
            } else {
                availability =
                    gray(tr("Not available: %1")
                             .arg(WindowsAppsText::issueText(planned.issues.front().code, action)));
            }
            actions += u"<p><b>"_s + WindowsAppsText::actionName(action).toHtmlEscaped() + u"</b> — "_s +
                       availability + u"<br>"_s + WindowsAppsText::actionDetails(action).toHtmlEscaped() +
                       u"</p>"_s;
        }
    }

    QStringList notes;
    if (a.protection != AppProtection::None) {
        notes << WindowsAppsText::protectionText(a);
    } else if (a.warning != tools::AppWarning::None) {
        notes << tr("Removing it breaks something: %1").arg(WindowsAppsText::warningText(a.warning));
    }
    QString notesHtml;
    for (const auto& note : notes) {
        notesHtml += u"<p style=\"color: #b36b00\">"_s + note.toHtmlEscaped() + u"</p>"_s;
    }
    QString header = u"<h3>"_s + WindowsAppsText::nameText(a).toHtmlEscaped() + u"</h3>"_s;
    const auto checked = model_->checkedApps().size();
    if (checked > 0) {
        header += u"<p>"_s +
                  gray(tr("%n app(s) checked; each action applies to the ones it fits. Details of the "
                          "selected one:",
                          nullptr, static_cast<int>(checked))) +
                  u"</p>"_s;
    } else if (const auto selected = selectedApps().size(); selected > 1) {
        header += u"<p>"_s +
                  gray(tr("%n app(s) selected; each action applies to the ones it fits. Details of this one:",
                          nullptr, static_cast<int>(selected))) +
                  u"</p>"_s;
    }
    details_->setHtml(header + notesHtml + u"<table>"_s + rows + u"</table>"_s + actions);
}

// --- Changes -----------------------------------------------------------------------------------

std::shared_ptr<WindowsAppsWindow::Change> WindowsAppsWindow::planChange(WindowsAppsAction action) const {
    const auto* listing = model_->listing();
    const auto targets = targetApps();
    if (listing == nullptr || targets.empty()) {
        return nullptr;
    }
    std::vector<std::wstring> ids;
    for (const auto* app : targets) {
        ids.push_back(app->id);
    }
    const auto nameOf = [&](const std::wstring& id) {
        const auto* app = findApp(listing->list, id);
        return app != nullptr ? WindowsAppsText::nameText(*app) : toQString(id);
    };
    // Remove for everyone is planned confirmed: it runs only once its confirmation is given.
    auto planned = plan(action, listing->list, ids, true);

    auto change = std::make_shared<Change>();
    change->action = action;
    for (const auto& id : planned.planned) {
        Change::Planned app{nameOf(id), {}, {}};
        if (const auto* found = findApp(listing->list, id)) {
            app.family = found->family_name;
            for (const auto& package : found->packages) {
                app.fullNames.push_back(package.full_name);
            }
        }
        change->planned.push_back(std::move(app));
    }
    for (const auto& issue : planned.issues) {
        change->skipped << tr("%1: %2").arg(nameOf(issue.id), WindowsAppsText::issueText(issue.code, action));
    }
    change->plan = std::move(planned.plan);
    change->plan.description = WindowsAppsText::planDescription(
                                   action, static_cast<int>(change->planned.size()),
                                   change->planned.size() == 1 ? change->planned.front().name : QString())
                                   .toStdWString();
    return change;
}

void WindowsAppsWindow::apply(WindowsAppsAction action) {
    if (work_ != Work::None || stores_->store() == nullptr) {
        return;
    }
    auto change = planChange(action);
    if (!change) {
        return;
    }
    if (change->plan.items.empty()) {
        showResult(
            tr("Nothing to change: %n app(s) skipped.", nullptr, static_cast<int>(change->skipped.size())),
            change->skipped);
        return;
    }
    startChange(std::move(change));
}

void WindowsAppsWindow::confirmRemoveForEveryone() {
    if (work_ != Work::None || stores_->store() == nullptr) {
        return;
    }
    auto change = planChange(WindowsAppsAction::RemoveForEveryone);
    if (!change) {
        return;
    }
    const int count = static_cast<int>(change->planned.size());
    if (count == 0) {
        showResult(
            tr("Nothing to remove: %n app(s) skipped.", nullptr, static_cast<int>(change->skipped.size())),
            change->skipped);
        return;
    }
    QStringList lines;
    for (int i = 0; i < count && i < kMaxAppsShown; ++i) {
        const auto& app = change->planned[static_cast<std::size_t>(i)];
        lines << u"• "_s + tr("%1 (%2)").arg(app.name, toQString(app.family));
    }
    if (count > kMaxAppsShown) {
        lines << tr("… and %n more", nullptr, count - kMaxAppsShown);
    }
    QString informative =
        lines.join(u'\n') + u"\n\n"_s +
        tr("They are removed for every user of this PC and deleted from it. This can't be undone:") + u'\n' +
        u"• "_s + tr("every user's data in them is lost: their settings and the files they keep") + u'\n' +
        u"• "_s + tr("Avel can't back them up: neither Undo nor the Undo Center can bring them back") +
        u'\n' + u"• "_s + tr("they come back only when installed again from the Microsoft Store or winget");
    if (!change->skipped.isEmpty()) {
        informative += u"\n\n"_s +
                       tr("%n app(s) will be skipped:", nullptr, static_cast<int>(change->skipped.size())) +
                       u'\n' + change->skipped.join(u'\n');
    }

    auto* box = new QMessageBox(QMessageBox::Warning, tr("Remove for everyone"),
                                count == 1 ? tr("Remove %1 for every user?").arg(change->planned.front().name)
                                           : tr("Remove %n app(s) for every user?", nullptr, count),
                                QMessageBox::NoButton, this);
    box->setObjectName(u"removeForEveryoneConfirmation"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextFormat(Qt::PlainText);
    box->setInformativeText(informative);
    auto* understand = new QCheckBox(tr("I understand this can't be undone"), box);
    understand->setObjectName(u"removeForEveryoneUnderstandCheck"_s);
    box->setCheckBox(understand);
    QPushButton* remove = box->addButton(tr("Remove for everyone"), QMessageBox::DestructiveRole);
    remove->setObjectName(u"removeForEveryoneConfirmButton"_s);
    remove->setEnabled(false);
    connect(understand, &QCheckBox::toggled, remove, &QPushButton::setEnabled);
    box->addButton(QMessageBox::Cancel)->setObjectName(u"removeForEveryoneCancelButton"_s);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::buttonClicked, this,
            [this, remove, understand,
             change = std::shared_ptr<const Change>(std::move(change))](QAbstractButton* clicked) {
                if (clicked == remove && understand->isChecked() && work_ == Work::None &&
                    stores_->store() != nullptr) {
                    startChange(change);
                }
            });
    box->open();
}

void WindowsAppsWindow::startChange(std::shared_ptr<const Change> change) {
    work_ = Work::Changing;
    resultLabel_->clear();
    resultLabel_->setToolTip(QString());
    workLabel_->setText(tr("Applying: %1…").arg(toQString(change->plan.description)));
    updateActions();
    pool_.start([this, store = stores_->store(), change] {
        bool busy = false;
        changes::ExecuteResult result;
        // Every ChangeSet holds exclusive hive access while it runs (hive_access.h).
        const auto hives = exclusiveHiveAccess([this](bool waiting) {
            QMetaObject::invokeMethod(this, [this, waiting] { onHiveWait(waiting); }, Qt::QueuedConnection);
        });
        if (const auto guard = store->lock(kLockTimeout)) {
            changes::ExecuteOptions options;
            // Only Remove for everyone plans irreversible items, once its confirmation was given.
            options.allow_irreversible = change->action == WindowsAppsAction::RemoveForEveryone;
            result = changes::execute(*store, change->plan, options);
        } else {
            busy = true;
        }
        QMetaObject::invokeMethod(
            this, [this, change, busy, result] { onChangeFinished(change, busy, result); },
            Qt::QueuedConnection);
    });
}

void WindowsAppsWindow::onHiveWait(bool waiting) {
    if (waiting) {
        workText_ = workLabel_->text();
        workLabel_->setText(hiveWaitText());
    } else {
        workLabel_->setText(workText_);
    }
}

void WindowsAppsWindow::onChangeFinished(const std::shared_ptr<const Change>& change, bool busy,
                                         const changes::ExecuteResult& result) {
    work_ = Work::None;
    workLabel_->clear();
    if (busy) {
        showResult(
            tr("Another Avel process is changing the system. Nothing was changed; try again when it's done."),
            {}, true);
        updateActions();
        return;
    }
    if (result.set_id.empty()) {
        showResult(tr("Avel couldn't write the backup of this change, so nothing was changed."), {}, true);
        updateActions();
        return;
    }
    const bool undoable = change->action != WindowsAppsAction::RemoveForEveryone;
    if (undoable) {
        undoStack_.push_back({result.set_id, change});
    }

    // Each item to its app: the planner adds an app's items one after another.
    const auto belongsTo = [](const leftovers::Location& location, const Change::Planned& app) {
        if (const auto* user = std::get_if<leftovers::PackageUserLocation>(&location)) {
            return platform::iequals(user->family_name, app.family);
        }
        if (const auto* provisioning = std::get_if<leftovers::PackageProvisioningLocation>(&location)) {
            return platform::iequals(provisioning->family_name, app.family);
        }
        if (const auto* package = std::get_if<leftovers::PackageLocation>(&location)) {
            return std::any_of(app.fullNames.begin(), app.fullNames.end(), [&](const std::wstring& name) {
                return platform::iequals(name, package->full_name);
            });
        }
        return false;
    };
    int changed = 0;
    bool anyDone = false;
    QStringList details;
    for (const auto& app : change->planned) {
        const changes::ItemStatus* problem = nullptr;
        const leftovers::Location* where = nullptr;
        for (std::size_t i = 0; i < result.items.size() && i < change->plan.items.size(); ++i) {
            const auto& location = change->plan.items[i].location;
            if (!belongsTo(location, app)) {
                continue;
            }
            const auto& status = result.items[i].status;
            anyDone = anyDone || status.state == changes::ItemState::Done ||
                      status.state == changes::ItemState::Partial;
            if (!finished(status) && problem == nullptr) {
                problem = &status;
                where = &location;
            }
        }
        if (problem == nullptr) {
            ++changed;
        } else {
            details << tr("%1: %2").arg(app.name, LeftoverText::statusText(*problem, *where));
        }
    }
    const int failed = static_cast<int>(details.size());
    const int skipped = static_cast<int>(change->skipped.size());
    QString text;
    switch (change->action) {
        case WindowsAppsAction::RemoveForMe:
            text = tr("%n app(s) removed for you.", nullptr, changed);
            break;
        case WindowsAppsAction::Deprovision:
            text = tr("%n app(s) will no longer be installed for new users.", nullptr, changed);
            break;
        case WindowsAppsAction::RemoveForEveryone:
            text = tr("%n app(s) removed for everyone.", nullptr, changed);
            break;
    }
    if (failed > 0) {
        text += u' ' + tr("%n couldn't be changed.", nullptr, failed);
    }
    if (skipped > 0) {
        text += u' ' + tr("%n skipped.", nullptr, skipped);
    }
    if (undoable && (changed > 0 || failed > 0)) {
        text += u' ' + tr("Undo with Ctrl+Z or in the Undo Center.");
    }
    showResult(text, details + change->skipped, failed > 0);
    if (anyDone) {
        Q_EMIT appsChanged();
    }
    refresh();
}

void WindowsAppsWindow::undoLast() {
    if (work_ != Work::None || undoStack_.empty() || stores_->store() == nullptr) {
        return;
    }
    const Done last = undoStack_.back();
    work_ = Work::Undoing;
    resultLabel_->clear();
    resultLabel_->setToolTip(QString());
    workLabel_->setText(tr("Undoing: %1…").arg(toQString(last.change->plan.description)));
    updateActions();
    pool_.start([this, store = stores_->store(), last] {
        bool busy = false;
        changes::RestoreResult result;
        const auto hives = exclusiveHiveAccess([this](bool waiting) {
            QMetaObject::invokeMethod(this, [this, waiting] { onHiveWait(waiting); }, Qt::QueuedConnection);
        });
        if (const auto guard = store->lock(kLockTimeout)) {
            result = changes::restore(*store, last.setId);
        } else {
            busy = true;
        }
        QMetaObject::invokeMethod(
            this, [this, last, busy, result] { onUndoFinished(last.change, last.setId, busy, result); },
            Qt::QueuedConnection);
    });
}

void WindowsAppsWindow::onUndoFinished(const std::shared_ptr<const Change>& change, const std::wstring& setId,
                                       bool busy, const changes::RestoreResult& result) {
    work_ = Work::None;
    workLabel_->clear();
    if (busy) {
        showResult(
            tr("Another Avel process is changing the system. Nothing was changed; try again when it's done."),
            {}, true);
        updateActions();
        return;
    }
    std::erase_if(undoStack_, [&](const Done& done) { return done.setId == setId; });
    const QString description = toQString(change->plan.description);
    if (!result.found) {
        showResult(
            tr("“%1” can't be undone here: its backup is missing or in use by another Avel process. The "
               "Undo Center shows what can still be put back.")
                .arg(description),
            {}, true);
        updateActions();
        return;
    }
    int restored = 0;
    QStringList details;
    for (const auto& [index, item] : result.items) {
        if (item.status.state == changes::ItemState::Restored) {
            ++restored;
        } else if (index < change->plan.items.size()) {
            const auto& location = change->plan.items[index].location;
            details << tr("%1: %2").arg(LeftoverText::locationText(location),
                                        LeftoverText::statusText(item.status, location));
        }
    }
    QString text = result.items.empty() ? tr("Nothing left to undo in “%1”.").arg(description)
                                        : tr("Undone: %1.").arg(description);
    if (!details.isEmpty()) {
        text += u' ' + tr("%n item(s) couldn't be put back; the Undo Center can try again.", nullptr,
                          static_cast<int>(details.size()));
    }
    showResult(text, details, !details.isEmpty());
    if (restored > 0) {
        Q_EMIT appsChanged();
    }
    refresh();
}

void WindowsAppsWindow::openUndoCenter() {
    if (work_ != Work::None) {
        return;
    }
    auto* center = new UndoCenterDialog(stores_->store(), stores_->unavailableReason(), this);
    center->setAttribute(Qt::WA_DeleteOnClose);
    connect(center, &QDialog::finished, this, [this, center] {
        if (center->changedAnything()) {
            Q_EMIT appsChanged();
            refresh();
        }
    });
    center->open();
}

void WindowsAppsWindow::showResult(const QString& text, const QStringList& details, bool problem) {
    resultLabel_->setText(problem ? u"⚠ "_s + text : text);
    resultLabel_->setToolTip(details.join(u'\n'));
}

// --- Context menu ------------------------------------------------------------------------------

void WindowsAppsWindow::showTableMenu(const QPoint& position) {
    if (!table_->indexAt(position).isValid()) {
        return;
    }
    QMenu menu(this);
    addCopy(menu, removeForMeAction_);
    addCopy(menu, deprovisionAction_);
    addCopy(menu, removeForEveryoneAction_);
    menu.addSeparator();
    addCopy(menu, copyFullNameAction_);
    addCopy(menu, copyFamilyNameAction_);
    addCopy(menu, openFolderAction_);
    menu.exec(table_->viewport()->mapToGlobal(position));
}

void WindowsAppsWindow::openInstallFolder() {
    const auto* app = currentApp();
    if (app == nullptr || app->install_location.empty()) {
        return;
    }
    const std::wstring folder = QDir::toNativeSeparators(toQString(app->install_location)).toStdWString();
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void WindowsAppsWindow::copyFullName() {
    QStringList names;
    for (const auto* app : selectedApps()) {
        if (const std::wstring name = fullNameOf(*app); !name.empty()) {
            names << toQString(name);
        }
    }
    if (!names.isEmpty()) {
        QGuiApplication::clipboard()->setText(names.join(u"\r\n"_s));
    }
}

void WindowsAppsWindow::copyFamilyName() {
    QStringList names;
    for (const auto* app : selectedApps()) {
        names << toQString(app->family_name);
    }
    if (!names.isEmpty()) {
        QGuiApplication::clipboard()->setText(names.join(u"\r\n"_s));
    }
}

}  // namespace avel::app
