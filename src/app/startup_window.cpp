#include "startup_window.h"

#include <QAction>
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

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <type_traits>
#include <variant>

#include "change_store_access.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/environment.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "hive_access.h"
#include "leftover_text.h"
#include "menu_copy.h"
#include "program_text.h"
#include "startup_model.h"
#include "startup_text.h"
#include "undo_center_dialog.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::StartupAction;
using tools::StartupEntry;
using tools::StartupKind;
using tools::StartupProtection;
using tools::StartupState;

namespace {

// Settings keys for the window layout.
constexpr auto kGeometryKey = "startup/geometry"_L1;
constexpr auto kSplitterKey = "startup/splitter"_L1;
constexpr auto kHeaderKey = "startup/header"_L1;

// The user filter's "every user"; never a SID.
constexpr auto kAllUsers = "*"_L1;

// Reading the services Avel disabled waits this long for another Avel process changing the system.
constexpr auto kReadTimeout = std::chrono::seconds(10);
// A change waits this long for it.
constexpr auto kLockTimeout = std::chrono::seconds(30);

// Delete's confirmation lists this many items, then "... and n more".
constexpr int kMaxItemsShown = 12;

// The Run and Startup folder entries Task Manager keeps a state for, tasks and services: the ones
// Disable and Enable work on (the others are tools::StartupPlanIssueCode::NotSupported).
bool hasState(const StartupEntry& entry) {
    switch (entry.kind) {
        case StartupKind::Run:
        case StartupKind::StartupFolder:
            return entry.approval.has_value();
        case StartupKind::Task:
        case StartupKind::Service:
            return true;
        case StartupKind::RunOnce:
        case StartupKind::RunOnceEx:
            break;
    }
    return false;
}

// Done, or left to go with the key or folder that contains it.
bool finished(const changes::ItemStatus& status) {
    return status.state == changes::ItemState::Done ||
           (status.state == changes::ItemState::Skipped && status.reason == changes::Reason::Contained);
}

QString itemText(const leftovers::Location& location) {
    return LeftoverText::kindLabel(location, leftovers::category_of(location)) + u": "_s +
           LeftoverText::locationText(location);
}

QString escaped(const std::wstring& text) { return toQString(text).toHtmlEscaped(); }

QString code(const std::wstring& text) { return u"<code>"_s + escaped(text) + u"</code>"_s; }

QString gray(const QString& text) {
    return u"<span style=\"color: gray\">"_s + text.toHtmlEscaped() + u"</span>"_s;
}

// The key Registry Editor opens, as its address bar writes it, or empty when the key's hive isn't
// loaded where Registry Editor sees it (a signed-out user's).
QString regeditPath(const platform::RegistryAddress& key) {
    if (key.hive == platform::Hive::Machine) {
        return u"HKEY_LOCAL_MACHINE\\"_s + toQString(key.path);
    }
    if (platform::iequals(key.sid, platform::current_user_sid())) {
        return u"HKEY_CURRENT_USER\\"_s + toQString(key.path);
    }
    HKEY hive = nullptr;
    if (::RegOpenKeyExW(HKEY_USERS, key.sid.c_str(), 0, KEY_READ, &hive) != ERROR_SUCCESS) {
        return {};
    }
    ::RegCloseKey(hive);
    return u"HKEY_USERS\\"_s + toQString(key.sid) + u'\\' + toQString(key.path);
}

// Registry Editor opens at the key it last showed, kept in its LastKey value. It writes the path
// under its tree's root, whose name ("Computer") is localized; the name it wrote last is reused.
void openRegedit(const QString& path) {
    constexpr wchar_t kRegeditKey[] = LR"(Software\Microsoft\Windows\CurrentVersion\Applets\Regedit)";
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegeditKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                          nullptr, &key, nullptr) == ERROR_SUCCESS) {
        QString root;
        wchar_t last[1024] = {};
        DWORD size = sizeof(last) - sizeof(wchar_t);
        if (::RegGetValueW(key, nullptr, L"LastKey", RRF_RT_REG_SZ, nullptr, last, &size) == ERROR_SUCCESS) {
            const QString previous = QString::fromWCharArray(last);
            const QString first = previous.section(u'\\', 0, 0);
            if (!first.isEmpty() && !first.startsWith(u"HKEY_"_s, Qt::CaseInsensitive)) {
                root = first + u'\\';
            }
        }
        const std::wstring value = (root + path).toStdWString();
        ::RegSetValueExW(key, L"LastKey", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        ::RegCloseKey(key);
    }
    // -m: a new window even when Registry Editor is open already, so it reads LastKey.
    ::ShellExecuteW(nullptr, L"open", L"regedit.exe", L"-m", nullptr, SW_SHOWNORMAL);
}

void openConsole(const wchar_t* console) {
    ::ShellExecuteW(nullptr, L"open", console, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

StartupWindow::StartupWindow(ChangeStoreAccess* stores, std::shared_ptr<const inventory::Inventory> inventory,
                             QWidget* parent)
    : QMainWindow(parent), stores_(stores), inventory_(std::move(inventory)), signer_(tools::live_signer()) {
    setObjectName(u"startupWindow"_s);
    pool_.setMaxThreadCount(1);

    model_ = new StartupTableModel(this);
    filter_ = new StartupFilterModel(this);
    filter_->setSourceStartupModel(model_);

    createActions();
    createToolBar();
    createCentralArea();
    createStatusBar();
    retranslateUi();
    restoreLayout();

    connect(filter_, &QAbstractItemModel::rowsInserted, this, &StartupWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, &StartupWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::modelReset, this, &StartupWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::layoutChanged, this, &StartupWindow::updateCounts);

    refresh();
}

StartupWindow::~StartupWindow() = default;

void StartupWindow::setInventory(std::shared_ptr<const inventory::Inventory> inventory) {
    inventory_ = std::move(inventory);
}

void StartupWindow::createActions() {
    const auto make = [this](QStyle::StandardPixmap icon, const QString& name, auto slot) {
        auto* action = icon == QStyle::SP_CustomBase
                           ? new QAction(this)
                           : new QAction(style()->standardIcon(icon), QString(), this);
        action->setObjectName(name);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);  // Its shortcut works anywhere in the window
        return action;
    };
    refreshAction_ = make(QStyle::SP_BrowserReload, u"startupRefreshAction"_s, [this] { refresh(); });
    refreshAction_->setShortcut(QKeySequence::Refresh);
    enableAction_ = make(QStyle::SP_MediaPlay, u"startupEnableAction"_s,
                         [this] { apply(StartupAction::Enable, tools::ServiceDisable::Manual); });
    disableAction_ = make(QStyle::SP_MediaPause, u"startupDisableAction"_s,
                          [this] { apply(StartupAction::Disable, tools::ServiceDisable::Manual); });
    disableCompletelyAction_ = make(QStyle::SP_CustomBase, u"startupDisableCompletelyAction"_s, [this] {
        apply(StartupAction::Disable, tools::ServiceDisable::Disabled);
    });
    deleteAction_ = make(QStyle::SP_TrashIcon, u"startupDeleteAction"_s, [this] { confirmDelete(); });
    deleteAction_->setShortcut(QKeySequence::Delete);
    undoAction_ = make(QStyle::SP_ArrowBack, u"startupUndoAction"_s, [this] { undoLast(); });
    undoAction_->setShortcut(QKeySequence::Undo);
    undoCenterAction_ =
        make(QStyle::SP_CustomBase, u"startupUndoCenterAction"_s, [this] { openUndoCenter(); });
    openLocationAction_ =
        make(QStyle::SP_DirOpenIcon, u"startupOpenLocationAction"_s, [this] { openFileLocation(); });
    copyCommandAction_ =
        make(QStyle::SP_CustomBase, u"startupCopyCommandAction"_s, [this] { copyCommand(); });
    goToProgramAction_ =
        make(QStyle::SP_CustomBase, u"startupGoToProgramAction"_s, [this] { goToProgram(); });
    openRegistryAction_ =
        make(QStyle::SP_CustomBase, u"startupOpenRegistryAction"_s, [this] { openInRegistryEditor(); });
    openTaskSchedulerAction_ = make(QStyle::SP_CustomBase, u"startupOpenTaskSchedulerAction"_s,
                                    [] { openConsole(L"taskschd.msc"); });
    openServicesAction_ =
        make(QStyle::SP_CustomBase, u"startupOpenServicesAction"_s, [] { openConsole(L"services.msc"); });
    findAction_ = make(QStyle::SP_CustomBase, u"startupFindAction"_s, [this] {
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->selectAll();
    });
    findAction_->setShortcut(QKeySequence::Find);
}

void StartupWindow::createToolBar() {
    toolBar_ = addToolBar(QString());
    toolBar_->setObjectName(u"startupToolBar"_s);
    toolBar_->setMovable(false);
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (QAction* action : {refreshAction_, enableAction_, disableAction_, deleteAction_}) {
        toolBar_->addAction(action);
    }
    toolBar_->addSeparator();
    toolBar_->addAction(undoAction_);
    toolBar_->addAction(undoCenterAction_);
    // The buttons are found by name in UI Automation: "startupEnableAction" -> "startupEnableButton".
    for (QAction* action : toolBar_->actions()) {
        if (QWidget* button = toolBar_->widgetForAction(action);
            button != nullptr && !action->isSeparator()) {
            button->setObjectName(action->objectName().replace(u"Action"_s, u"Button"_s));
        }
    }

    addToolBarBreak();
    filterBar_ = addToolBar(QString());
    filterBar_->setObjectName(u"startupFilterBar"_s);
    filterBar_->setMovable(false);

    searchEdit_ = new QLineEdit(filterBar_);
    searchEdit_->setObjectName(u"startupSearch"_s);
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(300);
    connect(searchEdit_, &QLineEdit::textChanged, filter_, &StartupFilterModel::setSearchText);
    filterBar_->addWidget(searchEdit_);

    sourceCombo_ = new QComboBox(filterBar_);
    sourceCombo_->setObjectName(u"startupSourceFilter"_s);
    sourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    sourceCombo_->addItem(QString(), -1);
    for (const StartupKind kind : {StartupKind::Run, StartupKind::RunOnce, StartupKind::RunOnceEx,
                                   StartupKind::StartupFolder, StartupKind::Task, StartupKind::Service}) {
        sourceCombo_->addItem(QString(), static_cast<int>(kind));
    }
    connect(sourceCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int kind = sourceCombo_->itemData(index).toInt();
        filter_->setKind(kind < 0 ? std::nullopt : std::optional(static_cast<StartupKind>(kind)));
        updateCounts();
    });
    filterBar_->addWidget(sourceCombo_);

    userCombo_ = new QComboBox(filterBar_);
    userCombo_->setObjectName(u"startupUserFilter"_s);
    userCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    userCombo_->addItem(QString(), QString(kAllUsers));
    userCombo_->addItem(QString(), QString());  // The machine's entries
    connect(userCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString sid = userCombo_->itemData(index).toString();
        filter_->setUser(sid == kAllUsers ? std::nullopt : std::optional(sid.toStdWString()));
        updateCounts();
    });
    filterBar_->addWidget(userCombo_);
}

void StartupWindow::createCentralArea() {
    table_ = new QTableView(this);
    table_->setObjectName(u"startupTable"_s);
    table_->setModel(filter_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(StartupTableModel::NameColumn, Qt::AscendingOrder);
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
    header->setStretchLastSection(true);  // The command
    header->resizeSection(StartupTableModel::NameColumn, 240);
    header->resizeSection(StartupTableModel::StateColumn, 130);
    header->resizeSection(StartupTableModel::SourceColumn, 150);
    header->resizeSection(StartupTableModel::UserColumn, 170);
    header->resizeSection(StartupTableModel::PublisherColumn, 200);

    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &StartupWindow::onSelectionChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &StartupWindow::onSelectionChanged);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, &StartupWindow::showTableMenu);

    details_ = new QTextBrowser(this);
    details_->setObjectName(u"startupDetails"_s);
    details_->setOpenLinks(false);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(u"startupSplitter"_s);
    splitter_->addWidget(table_);
    splitter_->addWidget(details_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({440, 280});
    setCentralWidget(splitter_);
}

void StartupWindow::createStatusBar() {
    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(u"startupResult"_s);
    workLabel_ = new QLabel(this);
    workLabel_->setObjectName(u"startupProgress"_s);
    warningLabel_ = new QLabel(this);
    warningLabel_->setObjectName(u"startupWarnings"_s);
    warningLabel_->hide();
    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(u"startupCount"_s);
    statusBar()->addWidget(workLabel_);
    statusBar()->addWidget(resultLabel_, 1);
    statusBar()->addPermanentWidget(warningLabel_);
    statusBar()->addPermanentWidget(countLabel_);
}

void StartupWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

void StartupWindow::closeEvent(QCloseEvent* event) {
    if (work_ == Work::Changing || work_ == Work::Undoing) {
        showResult(tr("Avel is changing startup entries. Close the window once that is done."));
        event->ignore();
        return;
    }
    saveLayout();
    QMainWindow::closeEvent(event);
}

void StartupWindow::retranslateUi() {
    setWindowTitle(tr("Startup"));
    refreshAction_->setText(tr("&Refresh"));
    refreshAction_->setToolTip(tr("List the startup entries again (F5)"));
    enableAction_->setText(tr("&Enable"));
    enableAction_->setToolTip(tr("Let the selected entries start with Windows again"));
    disableAction_->setText(tr("&Disable"));
    disableAction_->setToolTip(
        tr("Keep the selected entries from starting with Windows. A service is set to start manually, "
           "when a program asks for it."));
    disableCompletelyAction_->setText(tr("Disable &completely"));
    disableCompletelyAction_->setToolTip(tr("Set the selected services to Disabled: they never start"));
    deleteAction_->setText(tr("De&lete…"));
    deleteAction_->setToolTip(tr("Delete the selected entries: the value, file, task or service (Del)"));
    undoAction_->setText(tr("&Undo last change"));
    undoCenterAction_->setText(tr("Undo &Center…"));
    undoCenterAction_->setToolTip(tr("Everything Avel changed in the last 30 days, to put back"));
    openLocationAction_->setText(tr("&Open file location"));
    copyCommandAction_->setText(tr("&Copy command"));
    goToProgramAction_->setText(tr("&Go to program"));
    goToProgramAction_->setToolTip(tr("Show the program this entry belongs to in the main window"));
    openRegistryAction_->setText(tr("Open in &Registry Editor"));
    openTaskSchedulerAction_->setText(tr("Open &Task Scheduler"));
    openServicesAction_->setText(tr("Open &Services"));
    findAction_->setText(tr("Find"));
    toolBar_->setWindowTitle(tr("Toolbar"));
    filterBar_->setWindowTitle(tr("Filters"));
    searchEdit_->setPlaceholderText(tr("Search by name, command or publisher (Ctrl+F)"));
    searchEdit_->setAccessibleName(tr("Search"));

    sourceCombo_->setAccessibleName(tr("Source"));
    for (int i = 0; i < sourceCombo_->count(); ++i) {
        const int kind = sourceCombo_->itemData(i).toInt();
        sourceCombo_->setItemText(
            i, kind < 0 ? tr("All sources") : StartupText::kindLabel(static_cast<StartupKind>(kind)));
    }
    userCombo_->setAccessibleName(tr("User"));
    userCombo_->setItemText(0, tr("All users"));
    userCombo_->setItemText(1, tr("Machine (all users)"));

    model_->retranslate();
    updateActions();
    updateCounts();
    updateDiagnostics();
    renderDetails();
    if (work_ == Work::Listing) {
        workLabel_->setText(tr("Listing startup entries…"));
    }
}

void StartupWindow::restoreLayout() {
    const QSettings settings;
    restoreGeometry(settings.value(kGeometryKey).toByteArray());
    if (!settings.contains(kGeometryKey)) {
        resize(1200, 760);
    }
    splitter_->restoreState(settings.value(kSplitterKey).toByteArray());
    table_->horizontalHeader()->restoreState(settings.value(kHeaderKey).toByteArray());
}

void StartupWindow::saveLayout() const {
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kSplitterKey, splitter_->saveState());
    settings.setValue(kHeaderKey, table_->horizontalHeader()->saveState());
}

// --- Listing -----------------------------------------------------------------------------------

void StartupWindow::refresh() {
    if (work_ != Work::None) {
        return;
    }
    work_ = Work::Listing;
    workLabel_->setText(tr("Listing startup entries…"));
    updateActions();
    // The listing's LiveEnvironment lives only inside the task: it keeps signed-out users' hives
    // loaded privately, and a change to one of them can run only once it is gone. The pool runs
    // one task at a time, so a change of this window never overlaps it; the hive lock keeps the
    // other windows' changes out too (hive_access.h).
    pool_.start([this, store = stores_->store(), inventory = inventory_, signer = signer_] {
        auto listing = std::make_shared<StartupListing>();
        std::vector<std::wstring> avelDisabled;
        if (store) {
            if (const auto guard = store->lock(kReadTimeout)) {
                avelDisabled = tools::services_disabled_by_avel(*store);
            } else {
                listing->avelDisabledUnknown = true;
            }
        }
        listing->inventory =
            inventory ? inventory : std::make_shared<const inventory::Inventory>(scanInventory());
        const auto started = std::chrono::steady_clock::now();
        {
            const auto hives = sharedHiveAccess();
            const leftovers::LiveEnvironment live;
            tools::StartupEnvironment environment;
            environment.scan = live.environment();
            environment.programs = listing->inventory->programs;
            environment.signer = signer;
            environment.avel_disabled_services = std::move(avelDisabled);
            auto list = tools::list_startup(environment);
            listing->entries = std::move(list.entries);
            listing->diagnostics = std::move(list.diagnostics);
            listing->environmentDiagnostics = environment.scan.diagnostics;
        }
        listing->elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        QMetaObject::invokeMethod(
            this,
            [this, listing = std::shared_ptr<const StartupListing>(std::move(listing))] {
                onListed(listing);
            },
            Qt::QueuedConnection);
    });
}

void StartupWindow::onListed(std::shared_ptr<const StartupListing> listing) {
    work_ = Work::None;
    workLabel_->clear();

    // Keep the selection across a refresh.
    std::vector<std::wstring> selected;
    for (const auto* entry : selectedEntries()) {
        selected.push_back(entry->id);
    }
    const auto* current = currentEntry();
    const std::wstring currentId = current != nullptr ? current->id : std::wstring();

    model_->setListing(std::move(listing));
    populateUserFilter();
    selectIds(selected, currentId);
    updateCounts();
    updateDiagnostics();
    onSelectionChanged();  // The model reset cleared the selection without a selectionChanged signal
}

void StartupWindow::populateUserFilter() {
    const QString chosen = userCombo_->currentData().toString();
    std::map<QString, QString> users;  // By name, then SID
    if (const auto* listing = model_->listing()) {
        for (const auto& entry : listing->entries) {
            if (!entry.user_sid.empty()) {
                users.emplace(StartupText::userText(entry), toQString(entry.user_sid));
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
    const QString sid = userCombo_->currentData().toString();
    filter_->setUser(sid == kAllUsers ? std::nullopt : std::optional(sid.toStdWString()));
}

void StartupWindow::selectIds(const std::vector<std::wstring>& ids, const std::wstring& current) {
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

std::vector<const StartupEntry*> StartupWindow::selectedEntries() const {
    std::vector<const StartupEntry*> entries;
    QModelIndexList rows = table_->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const QModelIndex& row : rows) {
        if (const auto* entry = model_->entryAt(filter_->mapToSource(row).row())) {
            entries.push_back(entry);
        }
    }
    return entries;
}

const StartupEntry* StartupWindow::currentEntry() const {
    const QItemSelectionModel* selection = table_->selectionModel();
    QModelIndex index = table_->currentIndex();
    if (!index.isValid() || !selection->isRowSelected(index.row())) {
        const QModelIndexList rows = selection->selectedRows();
        index = rows.isEmpty() ? QModelIndex() : rows.front();
    }
    return index.isValid() ? model_->entryAt(filter_->mapToSource(index).row()) : nullptr;
}

void StartupWindow::onSelectionChanged() {
    renderDetails();
    updateActions();
}

void StartupWindow::updateActions() {
    const auto entries = selectedEntries();
    const bool idle = work_ == Work::None;
    const bool canChange = idle && stores_->store() != nullptr;
    bool canEnable = false;
    bool canDisable = false;
    bool canDisableCompletely = false;
    bool canDelete = false;
    bool anyCommand = false;
    for (const auto* entry : entries) {
        anyCommand = anyCommand || !entry->command.empty();
        if (entry->protection != StartupProtection::None) {
            continue;
        }
        canDelete = true;
        if (!hasState(*entry)) {
            continue;
        }
        (entry->state == StartupState::Disabled ? canEnable : canDisable) = true;
        canDisableCompletely = canDisableCompletely || (entry->kind == StartupKind::Service &&
                                                        entry->service_start != SERVICE_DISABLED);
    }
    refreshAction_->setEnabled(idle);
    enableAction_->setEnabled(canChange && canEnable);
    disableAction_->setEnabled(canChange && canDisable);
    disableCompletelyAction_->setEnabled(canChange && canDisableCompletely);
    deleteAction_->setEnabled(canChange && canDelete);
    undoAction_->setEnabled(canChange && !undoStack_.empty());
    undoAction_->setToolTip(
        undoStack_.empty()
            ? tr("Undo the last change made in this window (Ctrl+Z)")
            : tr("Undo “%1” (Ctrl+Z)").arg(toQString(undoStack_.back().change->plan.description)));
    // The Undo Center restores on its own; a listing of signed-out users' hives must not overlap it.
    undoCenterAction_->setEnabled(idle);
    if (stores_->store() == nullptr) {
        for (QAction* action : {enableAction_, disableAction_, disableCompletelyAction_, deleteAction_}) {
            action->setToolTip(stores_->unavailableReason());
        }
    }

    const auto* entry = currentEntry();
    openLocationAction_->setEnabled(entry != nullptr && !fileToShow(*entry).isEmpty());
    copyCommandAction_->setEnabled(anyCommand);
    goToProgramAction_->setEnabled(entry != nullptr && !model_->programName(entry->program_id).isEmpty());
    openRegistryAction_->setEnabled(entry != nullptr && registryKeyOf(*entry).has_value());
    openTaskSchedulerAction_->setEnabled(entry != nullptr && entry->kind == StartupKind::Task);
    openServicesAction_->setEnabled(entry != nullptr && entry->kind == StartupKind::Service);
}

void StartupWindow::updateCounts() {
    const auto* listing = model_->listing();
    if (listing == nullptr) {
        countLabel_->clear();
        return;
    }
    const int total = model_->rowCount();
    const int shown = filter_->rowCount();
    const auto disabled = static_cast<int>(
        std::count_if(listing->entries.begin(), listing->entries.end(),
                      [](const StartupEntry& entry) { return entry.state == StartupState::Disabled; }));
    QString text = shown == total ? tr("%n entry(s)", nullptr, total)
                                  : tr("Showing %1 of %n entry(s)", nullptr, total).arg(shown);
    text += u", "_s + tr("%n disabled", nullptr, disabled);
    countLabel_->setText(text);
}

void StartupWindow::updateDiagnostics() {
    QStringList lines;
    if (stores_->store() == nullptr) {
        lines << stores_->unavailableReason();
    }
    if (const auto* listing = model_->listing()) {
        for (const auto& diagnostic : listing->environmentDiagnostics) {
            lines << LeftoverText::diagnosticText(diagnostic);
        }
        for (const auto& diagnostic : listing->diagnostics) {
            lines << StartupText::diagnosticText(diagnostic);
        }
        if (listing->avelDisabledUnknown) {
            lines << tr(
                "Another Avel process was changing the system, so services Avel disabled may be "
                "missing from the list. Refresh with F5.");
        }
    }
    if (lines.isEmpty()) {
        warningLabel_->hide();
        return;
    }
    warningLabel_->setText(u"⚠ "_s + tr("%n warning(s)", nullptr, static_cast<int>(lines.size())));
    warningLabel_->setToolTip(lines.join(u'\n'));
    warningLabel_->show();
}

void StartupWindow::renderDetails() {
    const auto* entry = currentEntry();
    if (entry == nullptr) {
        details_->setHtml(u"<p style=\"color: gray\">"_s +
                          (model_->listing() == nullptr ? tr("Listing startup entries…")
                                                        : tr("Select an entry to see its details."))
                              .toHtmlEscaped() +
                          u"</p>"_s);
        return;
    }
    const StartupEntry& e = *entry;

    QString rows;
    const auto row = [&rows](const QString& label, const QString& valueHtml) {
        if (!valueHtml.isEmpty()) {
            rows += u"<tr><td style=\"color: gray; padding-right: 12px; white-space: nowrap\">"_s +
                    label.toHtmlEscaped() + u"</td><td>"_s + valueHtml + u"</td></tr>"_s;
        }
    };

    row(tr("State"),
        StartupText::stateText(e).toHtmlEscaped() + u"<br>"_s + gray(StartupText::stateDetails(e)));
    row(tr("Source"), StartupText::sourceText(e).toHtmlEscaped());
    row(tr("User"), e.user_sid.empty()
                        ? tr("Machine (all users)").toHtmlEscaped()
                        : StartupText::userText(e).toHtmlEscaped() + u"<br>"_s + code(e.user_sid));
    if (!e.command.empty()) {
        row(tr("Command"), code(e.command));
    } else if (e.kind == StartupKind::Task) {
        row(tr("Command"), gray(tr("None: the task runs a COM handler")));
    } else if (e.kind == StartupKind::StartupFolder) {
        row(tr("Command"),
            gray(tr("None: an advertised shortcut, which Windows Installer resolves when it runs")));
    }
    if (!e.target.empty()) {
        QString target = code(e.target);
        if (e.target_missing) {
            target +=
                u" <span style=\"color: #c42b1c\">"_s + tr("(file not found)").toHtmlEscaped() + u"</span>"_s;
        }
        row(tr("File"), target);
    }
    if (!e.target.empty() && !e.target_missing) {
        row(tr("Signature"), StartupText::signatureText(e.signer).toHtmlEscaped());
    }
    row(tr("Product"), escaped(e.product.product_name));
    row(tr("Company"), escaped(e.product.company_name));
    row(tr("Version"), escaped(e.product.version));
    const QString program = model_->programName(e.program_id);
    row(tr("Program"), program.isEmpty() ? gray(tr("No installed program's folder holds the file"))
                                         : program.toHtmlEscaped());
    if (!e.delete_locations.empty()) {
        row(tr("Stored in"), itemText(e.delete_locations.front()).toHtmlEscaped());
    }
    if (e.kind == StartupKind::Service) {
        row(tr("Registry key"), code(L"HKLM\\SYSTEM\\CurrentControlSet\\Services\\" + e.name));
    }
    if (e.approval) {
        row(tr("State kept in"),
            LeftoverText::locationText(
                leftovers::RegistryValueLocation{e.approval->key, e.approval->value_name})
                    .toHtmlEscaped() +
                (e.approval->value ? QString() : u"<br>"_s + gray(tr("No value yet: enabled"))));
    }
    QStringList removes;
    for (const auto& location : e.delete_locations) {
        removes << itemText(location).toHtmlEscaped();
    }
    row(tr("Delete removes"), removes.join(u"<br>"_s));
    row(tr("Id"), code(e.id));

    QStringList notes;
    if (e.protection == StartupProtection::WindowsOwn) {
        notes << tr(
            "Windows' own entry: it runs a file of Windows itself, from the Windows folder, signed by "
            "Microsoft. Avel lists it but never changes it.");
    }
    if (e.disabled_by_avel) {
        notes << tr("Avel disabled this service. Enable sets it to start automatically again.");
    }
    if (e.target_missing) {
        notes << tr("The file it runs is missing, so it does nothing at startup. Deleting it is safe.");
    }
    QString notesHtml;
    for (const auto& note : notes) {
        notesHtml += u"<p style=\"color: #b36b00\">"_s + note.toHtmlEscaped() + u"</p>"_s;
    }
    const auto selected = selectedEntries().size();
    QString header = u"<h3>"_s + StartupText::nameText(e).toHtmlEscaped() + u"</h3>"_s;
    if (selected > 1) {
        header +=
            u"<p>"_s +
            gray(tr("%n entry(s) selected; an action applies to each of them it fits. Details of this one:",
                    nullptr, static_cast<int>(selected))) +
            u"</p>"_s;
    }
    details_->setHtml(header + notesHtml + u"<table>"_s + rows + u"</table>"_s);
}

// --- Changes -----------------------------------------------------------------------------------

std::shared_ptr<StartupWindow::Change> StartupWindow::planChange(StartupAction action,
                                                                 tools::ServiceDisable serviceDisable) const {
    const auto* listing = model_->listing();
    const auto entries = selectedEntries();
    if (listing == nullptr || entries.empty()) {
        return nullptr;
    }
    std::vector<std::wstring> ids;
    for (const auto* entry : entries) {
        ids.push_back(entry->id);
    }
    const auto nameOf = [&](const std::wstring& id) {
        const auto* entry = model_->entryAt(model_->rowOf(id));
        return entry != nullptr ? StartupText::nameText(*entry) : toQString(id);
    };
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    auto planned = tools::plan_startup(listing->entries, action, ids, serviceDisable, now,
                                       [this](const std::wstring& programId) {
                                           const QString name = model_->programName(programId);
                                           return name.isEmpty() ? programId : name.toStdWString();
                                       });

    auto change = std::make_shared<Change>();
    change->action = action;
    change->serviceDisable = serviceDisable;
    for (const auto& id : planned.planned) {
        const auto* entry = model_->entryAt(model_->rowOf(id));
        const std::size_t items =
            action == StartupAction::Delete && entry != nullptr ? entry->delete_locations.size() : 1;
        change->planned.push_back({nameOf(id), items});
    }
    for (const auto& issue : planned.issues) {
        change->skipped << tr("%1: %2").arg(nameOf(issue.id), StartupText::issueText(issue.code, action));
    }
    change->plan = std::move(planned.plan);
    change->plan.description =
        StartupText::planDescription(action, serviceDisable, static_cast<int>(change->planned.size()),
                                     change->planned.size() == 1 ? change->planned.front().name : QString())
            .toStdWString();
    return change;
}

void StartupWindow::apply(StartupAction action, tools::ServiceDisable serviceDisable) {
    if (work_ != Work::None || stores_->store() == nullptr) {
        return;
    }
    auto change = planChange(action, serviceDisable);
    if (!change) {
        return;
    }
    if (change->plan.items.empty()) {
        showResult(tr("Nothing to change: %n selected entry(s) skipped.", nullptr,
                      static_cast<int>(change->skipped.size())),
                   change->skipped);
        return;
    }
    startChange(std::move(change));
}

void StartupWindow::confirmDelete() {
    if (work_ != Work::None || stores_->store() == nullptr) {
        return;
    }
    auto change = planChange(StartupAction::Delete, tools::ServiceDisable::Manual);
    if (!change) {
        return;
    }
    const int count = static_cast<int>(change->planned.size());
    if (count == 0) {
        showResult(tr("Nothing to delete: %n selected entry(s) skipped.", nullptr,
                      static_cast<int>(change->skipped.size())),
                   change->skipped);
        return;
    }
    QStringList lines;
    const auto& items = change->plan.items;
    for (std::size_t i = 0; i < items.size() && i < static_cast<std::size_t>(kMaxItemsShown); ++i) {
        lines << u"• "_s + itemText(items[i].location);
    }
    if (items.size() > static_cast<std::size_t>(kMaxItemsShown)) {
        lines << tr("… and %n more", nullptr, static_cast<int>(items.size()) - kMaxItemsShown);
    }
    QString informative =
        tr("This removes:") + u'\n' + lines.join(u'\n') + u"\n\n"_s +
        tr("Avel backs it up first: Undo last change (Ctrl+Z) or the Undo Center puts it back "
           "within 30 days.");
    if (!change->skipped.isEmpty()) {
        informative +=
            u"\n\n"_s +
            tr("%n selected entry(s) will be skipped:", nullptr, static_cast<int>(change->skipped.size())) +
            u'\n' + change->skipped.join(u'\n');
    }

    auto* box =
        new QMessageBox(QMessageBox::Warning, tr("Delete startup entries"),
                        count == 1 ? tr("Delete the startup entry %1?").arg(change->planned.front().name)
                                   : tr("Delete %n startup entry(s)?", nullptr, count),
                        QMessageBox::NoButton, this);
    box->setObjectName(u"startupDeleteConfirmation"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextFormat(Qt::PlainText);
    box->setInformativeText(informative);
    QPushButton* remove = box->addButton(tr("Delete"), QMessageBox::DestructiveRole);
    remove->setObjectName(u"startupConfirmDeleteButton"_s);
    box->addButton(QMessageBox::Cancel)->setObjectName(u"startupCancelDeleteButton"_s);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(
        box, &QMessageBox::buttonClicked, this,
        [this, remove, change = std::shared_ptr<const Change>(std::move(change))](QAbstractButton* clicked) {
            if (clicked == remove && work_ == Work::None && stores_->store() != nullptr) {
                startChange(change);
            }
        });
    box->open();
}

void StartupWindow::startChange(std::shared_ptr<const Change> change) {
    work_ = Work::Changing;
    resultLabel_->clear();
    resultLabel_->setToolTip(QString());
    workLabel_->setText(tr("Applying: %1…").arg(toQString(change->plan.description)));
    updateActions();
    pool_.start([this, store = stores_->store(), change] {
        bool busy = false;
        changes::ExecuteResult result;
        const auto hives = exclusiveHiveAccess([this](bool waiting) {
            QMetaObject::invokeMethod(this, [this, waiting] { onHiveWait(waiting); }, Qt::QueuedConnection);
        });
        if (const auto guard = store->lock(kLockTimeout)) {
            result = changes::execute(*store, change->plan);
        } else {
            busy = true;
        }
        QMetaObject::invokeMethod(
            this, [this, change, busy, result] { onChangeFinished(change, busy, result); },
            Qt::QueuedConnection);
    });
}

void StartupWindow::onHiveWait(bool waiting) {
    if (waiting) {
        workText_ = workLabel_->text();
        workLabel_->setText(hiveWaitText());
    } else {
        workLabel_->setText(workText_);
    }
}

void StartupWindow::onChangeFinished(const std::shared_ptr<const Change>& change, bool busy,
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
    undoStack_.push_back({result.set_id, change});

    int changed = 0;
    QStringList details;
    std::size_t index = 0;
    for (const auto& planned : change->planned) {
        const changes::ItemStatus* problem = nullptr;
        const leftovers::Location* where = nullptr;
        for (std::size_t i = 0; i < planned.items; ++i, ++index) {
            if (index < result.items.size() && !finished(result.items[index].status) && problem == nullptr) {
                problem = &result.items[index].status;
                where = &change->plan.items[index].location;
            }
        }
        if (problem == nullptr) {
            ++changed;
        } else {
            details << tr("%1: %2").arg(planned.name, LeftoverText::statusText(*problem, *where));
        }
    }
    const int failed = static_cast<int>(details.size());
    const int skipped = static_cast<int>(change->skipped.size());
    QString text;
    switch (change->action) {
        case StartupAction::Disable:
            text = change->serviceDisable == tools::ServiceDisable::Disabled
                       ? tr("%n entry(s) disabled completely.", nullptr, changed)
                       : tr("%n entry(s) disabled.", nullptr, changed);
            break;
        case StartupAction::Enable:
            text = tr("%n entry(s) enabled.", nullptr, changed);
            break;
        case StartupAction::Delete:
            text = tr("%n entry(s) deleted.", nullptr, changed);
            break;
    }
    if (failed > 0) {
        text += u' ' + tr("%n couldn't be changed.", nullptr, failed);
    }
    if (skipped > 0) {
        text += u' ' + tr("%n skipped.", nullptr, skipped);
    }
    if (changed > 0 || failed > 0) {
        text += u' ' + tr("Undo with Ctrl+Z or in the Undo Center.");
    }
    showResult(text, details + change->skipped, failed > 0);
    refresh();
}

void StartupWindow::undoLast() {
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

void StartupWindow::onUndoFinished(const std::shared_ptr<const Change>& change, const std::wstring& setId,
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
    refresh();
}

void StartupWindow::openUndoCenter() {
    if (work_ != Work::None) {
        return;
    }
    auto* center = new UndoCenterDialog(stores_->store(), stores_->unavailableReason(), this);
    center->setAttribute(Qt::WA_DeleteOnClose);
    connect(center, &QDialog::finished, this, [this, center] {
        if (center->changedAnything()) {
            Q_EMIT changesRestored();
            refresh();
        }
    });
    center->open();
}

void StartupWindow::showResult(const QString& text, const QStringList& details, bool problem) {
    resultLabel_->setText(problem ? u"⚠ "_s + text : text);
    resultLabel_->setToolTip(details.join(u'\n'));
}

// --- Context menu and the other tools ----------------------------------------------------------

void StartupWindow::showTableMenu(const QPoint& position) {
    if (!table_->indexAt(position).isValid()) {
        return;
    }
    const auto entries = selectedEntries();
    const auto* entry = currentEntry();
    QMenu menu(this);
    addCopy(menu, enableAction_);
    addCopy(menu, disableAction_);
    if (std::any_of(entries.begin(), entries.end(),
                    [](const StartupEntry* e) { return e->kind == StartupKind::Service; })) {
        addCopy(menu, disableCompletelyAction_);
    }
    addCopy(menu, deleteAction_);
    menu.addSeparator();
    addCopy(menu, openLocationAction_);
    addCopy(menu, copyCommandAction_);
    addCopy(menu, goToProgramAction_);
    if (entry != nullptr && registryKeyOf(*entry)) {
        addCopy(menu, openRegistryAction_);
    }
    if (entry != nullptr && entry->kind == StartupKind::Task) {
        addCopy(menu, openTaskSchedulerAction_);
    }
    if (entry != nullptr && entry->kind == StartupKind::Service) {
        addCopy(menu, openServicesAction_);
    }
    menu.exec(table_->viewport()->mapToGlobal(position));
}

QString StartupWindow::fileToShow(const StartupEntry& entry) {
    if (!entry.target.empty() && !entry.target_missing && QFileInfo::exists(toQString(entry.target))) {
        return toQString(entry.target);
    }
    // A Startup folder item whose target can't be told: the item itself.
    if (entry.kind == StartupKind::StartupFolder && !entry.delete_locations.empty()) {
        if (const auto* file = std::get_if<leftovers::FileLocation>(&entry.delete_locations.front())) {
            return QFileInfo::exists(toQString(file->path)) ? toQString(file->path) : QString();
        }
    }
    return {};
}

std::optional<platform::RegistryAddress> StartupWindow::registryKeyOf(const StartupEntry& entry) {
    if (entry.kind == StartupKind::Service) {
        return platform::RegistryAddress{
            platform::Hive::Machine, {}, L"SYSTEM\\CurrentControlSet\\Services\\" + entry.name};
    }
    if (entry.delete_locations.empty()) {
        return std::nullopt;
    }
    const auto& location = entry.delete_locations.front();
    if (const auto* value = std::get_if<leftovers::RegistryValueLocation>(&location)) {
        return value->key;
    }
    if (const auto* key = std::get_if<leftovers::RegistryKeyLocation>(&location)) {
        return key->key;
    }
    return std::nullopt;
}

void StartupWindow::openFileLocation() {
    const auto* entry = currentEntry();
    const QString path = entry != nullptr ? fileToShow(*entry) : QString();
    if (path.isEmpty()) {
        return;
    }
    // Explorer's own "select this file" (explorer /select), without a command line to quote.
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(native.c_str())) {
        ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ::ILFree(item);
    }
}

void StartupWindow::copyCommand() {
    QStringList commands;
    for (const auto* entry : selectedEntries()) {
        if (!entry->command.empty()) {
            commands << toQString(entry->command);
        }
    }
    if (!commands.isEmpty()) {
        QGuiApplication::clipboard()->setText(commands.join(u"\r\n"_s));
    }
}

void StartupWindow::goToProgram() {
    const auto* entry = currentEntry();
    if (entry != nullptr && !entry->program_id.empty()) {
        Q_EMIT programRequested(toQString(entry->program_id));
    }
}

void StartupWindow::openInRegistryEditor() {
    const auto* entry = currentEntry();
    const auto key = entry != nullptr ? registryKeyOf(*entry) : std::nullopt;
    if (!key) {
        return;
    }
    const QString path = regeditPath(*key);
    if (path.isEmpty()) {
        showResult(
            tr("%1 is signed out, so their registry isn't loaded where Registry Editor sees it. In Registry "
               "Editor, File > Load Hive opens their NTUSER.DAT.")
                .arg(StartupText::userText(*entry)),
            {}, true);
        return;
    }
    openRegedit(path);
}

}  // namespace avel::app
