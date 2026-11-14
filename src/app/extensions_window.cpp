#include "extensions_window.h"

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
#include <QUrl>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "change_store_access.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/protection.h"
#include "core/platform/text.h"
#include "extensions_model.h"
#include "extensions_text.h"
#include "hive_access.h"
#include "leftover_text.h"
#include "menu_copy.h"
#include "program_text.h"
#include "registry_editor.h"
#include "undo_center_dialog.h"
#include "unelevated_launch.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::BrowserExtension;
using tools::ExtensionRemoval;
using tools::ExtensionSource;
using tools::ExtensionSourceKind;

namespace {

// Settings keys for the window layout.
constexpr auto kGeometryKey = "extensions/geometry"_L1;
constexpr auto kSplitterKey = "extensions/splitter"_L1;
constexpr auto kHeaderKey = "extensions/header"_L1;

// The user filter's "every user"; never a SID.
constexpr auto kAllUsers = "*"_L1;

// The details pane's links: "regedit:<source>", "policies:<source>", "page:".
constexpr auto kRegeditScheme = "regedit"_L1;
constexpr auto kPoliciesScheme = "policies"_L1;
constexpr auto kPageScheme = "page"_L1;

// A change waits this long for another Avel process changing the system.
constexpr auto kLockTimeout = std::chrono::seconds(30);

// The confirmation lists this many items, and this many other profiles, then "... and n more".
constexpr int kMaxItemsShown = 12;

// Done, or left to go with the key that contains it.
bool finished(const changes::ItemStatus& status) {
    return status.state == changes::ItemState::Done ||
           (status.state == changes::ItemState::Skipped && status.reason == changes::Reason::Contained);
}

QString escaped(const std::wstring& text) { return toQString(text).toHtmlEscaped(); }

QString code(const std::wstring& text) { return u"<code>"_s + escaped(text) + u"</code>"_s; }

QString gray(const QString& text) {
    return u"<span style=\"color: gray\">"_s + text.toHtmlEscaped() + u"</span>"_s;
}

QString warning(const QString& text) {
    return u"<span style=\"color: #b36b00\">"_s + text.toHtmlEscaped() + u"</span>"_s;
}

QString link(const QString& href, const QString& text) {
    return u"<a href=\""_s + href.toHtmlEscaped() + u"\">"_s + text.toHtmlEscaped() + u"</a>"_s;
}

// Bulleted lines, at most kMaxItemsShown of them.
QString bullets(const QStringList& lines) {
    QStringList shown;
    for (qsizetype i = 0; i < lines.size() && i < kMaxItemsShown; ++i) {
        shown << u"• "_s + lines[i];
    }
    if (lines.size() > kMaxItemsShown) {
        shown << ExtensionsWindow::tr("… and %n more", nullptr,
                                      static_cast<int>(lines.size()) - kMaxItemsShown);
    }
    return shown.join(u'\n');
}

// A source's key, with its value: "HKLM\SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist : 1".
QString sourceWhere(const ExtensionSource& source) {
    if (source.kind == ExtensionSourceKind::PoliciesJson) {
        return toQString(source.file);
    }
    if (source.value_name.empty() || source.kind == ExtensionSourceKind::ExternalRegistry ||
        source.settings_subkey) {
        return LeftoverText::locationText(leftovers::RegistryKeyLocation{source.key});
    }
    return LeftoverText::locationText(leftovers::RegistryValueLocation{source.key, source.value_name});
}

}  // namespace

ExtensionsWindow::ExtensionsWindow(ChangeStoreAccess* stores,
                                   std::shared_ptr<const inventory::Inventory> inventory, QWidget* parent)
    : QMainWindow(parent), stores_(stores), inventory_(std::move(inventory)) {
    setObjectName(u"extensionsWindow"_s);
    pool_.setMaxThreadCount(1);

    model_ = new ExtensionsTableModel(this);
    filter_ = new ExtensionsFilterModel(this);
    filter_->setSourceExtensionsModel(model_);

    createActions();
    createToolBar();
    createCentralArea();
    createStatusBar();
    retranslateUi();
    restoreLayout();

    connect(filter_, &QAbstractItemModel::rowsInserted, this, &ExtensionsWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, &ExtensionsWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::modelReset, this, &ExtensionsWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::layoutChanged, this, &ExtensionsWindow::updateCounts);

    refresh();
}

ExtensionsWindow::~ExtensionsWindow() = default;

void ExtensionsWindow::setInventory(std::shared_ptr<const inventory::Inventory> inventory) {
    inventory_ = std::move(inventory);
}

void ExtensionsWindow::createActions() {
    const auto make = [this](QStyle::StandardPixmap icon, const QString& name, auto slot) {
        auto* action = icon == QStyle::SP_CustomBase
                           ? new QAction(this)
                           : new QAction(style()->standardIcon(icon), QString(), this);
        action->setObjectName(name);
        connect(action, &QAction::triggered, this, slot);
        addAction(action);  // Its shortcut works anywhere in the window
        return action;
    };
    refreshAction_ = make(QStyle::SP_BrowserReload, u"extensionsRefreshAction"_s, [this] { refresh(); });
    refreshAction_->setShortcut(QKeySequence::Refresh);
    removeAction_ = make(QStyle::SP_TrashIcon, u"removeExtensionAction"_s, [this] { confirmRemove(); });
    removeAction_->setShortcut(QKeySequence::Delete);
    openPageAction_ =
        make(QStyle::SP_DesktopIcon, u"openExtensionsPageAction"_s, [this] { openExtensionsPage(); });
    openPoliciesJsonAction_ =
        make(QStyle::SP_FileIcon, u"openPoliciesJsonAction"_s, [this] { openPoliciesJson(); });
    undoAction_ = make(QStyle::SP_ArrowBack, u"extensionsUndoAction"_s, [this] { undoLast(); });
    undoAction_->setShortcut(QKeySequence::Undo);
    undoCenterAction_ =
        make(QStyle::SP_CustomBase, u"extensionsUndoCenterAction"_s, [this] { openUndoCenter(); });
    copyIdAction_ = make(QStyle::SP_CustomBase, u"extensionsCopyIdAction"_s, [this] { copyExtensionId(); });
    openRegistryAction_ = make(QStyle::SP_CustomBase, u"extensionsOpenRegistryAction"_s, [this] {
        const auto* listing = model_->listing();
        const auto* entry = currentEntry();
        if (listing != nullptr && entry != nullptr) {
            if (const auto* source = firstRegistrySource(*entry, listing->list)) {
                openInRegistryEditor(*source);
            }
        }
    });
    openProfileAction_ =
        make(QStyle::SP_DirOpenIcon, u"extensionsOpenProfileAction"_s, [this] { openProfileFolder(); });
    findAction_ = make(QStyle::SP_CustomBase, u"extensionsFindAction"_s, [this] {
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->selectAll();
    });
    findAction_->setShortcut(QKeySequence::Find);
}

void ExtensionsWindow::createToolBar() {
    toolBar_ = addToolBar(QString());
    toolBar_->setObjectName(u"extensionsToolBar"_s);
    toolBar_->setMovable(false);
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar_->addAction(refreshAction_);
    toolBar_->addSeparator();
    for (QAction* action : {removeAction_, openPageAction_, openPoliciesJsonAction_}) {
        toolBar_->addAction(action);
    }
    toolBar_->addSeparator();
    toolBar_->addAction(undoAction_);
    toolBar_->addAction(undoCenterAction_);
    // The buttons are found by name in UI Automation: "removeExtensionAction" -> "removeExtensionButton".
    for (QAction* action : toolBar_->actions()) {
        if (QWidget* button = toolBar_->widgetForAction(action);
            button != nullptr && !action->isSeparator()) {
            button->setObjectName(action->objectName().replace(u"Action"_s, u"Button"_s));
        }
    }

    addToolBarBreak();
    filterBar_ = addToolBar(QString());
    filterBar_->setObjectName(u"extensionsFilterBar"_s);
    filterBar_->setMovable(false);

    searchEdit_ = new QLineEdit(filterBar_);
    searchEdit_->setObjectName(u"extensionsSearch"_s);
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(300);
    connect(searchEdit_, &QLineEdit::textChanged, filter_, &ExtensionsFilterModel::setSearchText);
    filterBar_->addWidget(searchEdit_);

    browserCombo_ = new QComboBox(filterBar_);
    browserCombo_->setObjectName(u"extensionsBrowserFilter"_s);
    browserCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    browserCombo_->addItem(QString(), -1);
    connect(browserCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int browser = browserCombo_->itemData(index).toInt();
        filter_->setBrowser(browser < 0 ? std::nullopt
                                        : std::optional(static_cast<tools::BrowserKind>(browser)));
        updateCounts();
    });
    filterBar_->addWidget(browserCombo_);

    userCombo_ = new QComboBox(filterBar_);
    userCombo_->setObjectName(u"extensionsUserFilter"_s);
    userCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    userCombo_->addItem(QString(), QString(kAllUsers));
    userCombo_->addItem(QString(), QString());  // The machine's sources' entries
    connect(userCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString sid = userCombo_->itemData(index).toString();
        filter_->setUser(sid == kAllUsers ? std::nullopt : std::optional(sid.toStdWString()));
        updateCounts();
    });
    filterBar_->addWidget(userCombo_);

    removableCheck_ = new QCheckBox(filterBar_);
    removableCheck_->setObjectName(u"extensionsRemovableFilter"_s);
    connect(removableCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        filter_->setOnlyRemovable(checked);
        updateCounts();
    });
    filterBar_->addWidget(removableCheck_);
}

void ExtensionsWindow::createCentralArea() {
    table_ = new QTableView(this);
    table_->setObjectName(u"extensionsTable"_s);
    table_->setModel(filter_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(ExtensionsTableModel::NameColumn, Qt::AscendingOrder);
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
    header->setStretchLastSection(true);  // Removal
    header->resizeSection(ExtensionsTableModel::NameColumn, 260);
    header->resizeSection(ExtensionsTableModel::BrowserColumn, 140);
    header->resizeSection(ExtensionsTableModel::ProfileColumn, 120);
    header->resizeSection(ExtensionsTableModel::UserColumn, 150);
    header->resizeSection(ExtensionsTableModel::StateColumn, 90);
    header->resizeSection(ExtensionsTableModel::InstallColumn, 120);
    header->resizeSection(ExtensionsTableModel::StoreColumn, 80);
    header->resizeSection(ExtensionsTableModel::VersionColumn, 100);

    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &ExtensionsWindow::onSelectionChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &ExtensionsWindow::onSelectionChanged);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, &ExtensionsWindow::showTableMenu);

    details_ = new QTextBrowser(this);
    details_->setObjectName(u"extensionsDetails"_s);
    details_->setOpenLinks(false);
    connect(details_, &QTextBrowser::anchorClicked, this, &ExtensionsWindow::onDetailsLink);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(u"extensionsSplitter"_s);
    splitter_->addWidget(table_);
    splitter_->addWidget(details_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({420, 320});
    setCentralWidget(splitter_);
}

void ExtensionsWindow::createStatusBar() {
    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(u"extensionsResult"_s);
    workLabel_ = new QLabel(this);
    workLabel_->setObjectName(u"extensionsProgress"_s);
    warningLabel_ = new QLabel(this);
    warningLabel_->setObjectName(u"extensionsWarnings"_s);
    warningLabel_->hide();
    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(u"extensionsCount"_s);
    statusBar()->addWidget(workLabel_);
    statusBar()->addWidget(resultLabel_, 1);
    statusBar()->addPermanentWidget(warningLabel_);
    statusBar()->addPermanentWidget(countLabel_);
}

void ExtensionsWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

void ExtensionsWindow::closeEvent(QCloseEvent* event) {
    if (work_ == Work::Changing || work_ == Work::Undoing) {
        showResult(tr("Avel is changing browser extensions. Close the window once that is done."));
        event->ignore();
        return;
    }
    saveLayout();
    QMainWindow::closeEvent(event);
}

void ExtensionsWindow::retranslateUi() {
    setWindowTitle(tr("Browser extensions"));
    refreshAction_->setText(tr("&Refresh"));
    refreshAction_->setToolTip(tr("List the extensions again (F5)"));
    removeAction_->setText(tr("Re&move…"));
    openPageAction_->setText(tr("Open &extensions page"));
    openPoliciesJsonAction_->setText(tr("Open &policies.json"));
    openPoliciesJsonAction_->setToolTip(
        tr("Show the Firefox policies.json that installs the selected extension in Explorer. Avel doesn't "
           "edit it."));
    undoAction_->setText(tr("&Undo last change"));
    undoCenterAction_->setText(tr("Undo &Center…"));
    undoCenterAction_->setToolTip(tr("Everything Avel changed in the last 30 days, to put back"));
    copyIdAction_->setText(tr("&Copy extension id"));
    openRegistryAction_->setText(tr("Open in &Registry Editor"));
    openRegistryAction_->setToolTip(tr("Open the key of the first policy or registration that installs it"));
    openProfileAction_->setText(tr("Open &profile folder"));
    findAction_->setText(tr("Find"));
    toolBar_->setWindowTitle(tr("Toolbar"));
    filterBar_->setWindowTitle(tr("Filters"));
    searchEdit_->setPlaceholderText(tr("Search by name, extension id, description or profile (Ctrl+F)"));
    searchEdit_->setAccessibleName(tr("Search"));
    browserCombo_->setAccessibleName(tr("Browser"));
    for (int i = 0; i < browserCombo_->count(); ++i) {
        const int browser = browserCombo_->itemData(i).toInt();
        browserCombo_->setItemText(
            i, browser < 0 ? tr("All browsers")
                           : ExtensionsText::browserName(static_cast<tools::BrowserKind>(browser)));
    }
    userCombo_->setAccessibleName(tr("User"));
    userCombo_->setItemText(0, tr("All users"));
    userCombo_->setItemText(1, ExtensionsText::userText({}, {}, {}));
    removableCheck_->setText(tr("Only removable"));
    removableCheck_->setToolTip(
        tr("Only the extensions that a policy or a program's registration installs, which Avel can remove"));
    populateFilters();  // The current user's "(you)"

    model_->retranslate();
    updateActions();
    updateCounts();
    updateDiagnostics();
    renderDetails();
    if (work_ == Work::Listing) {
        workLabel_->setText(tr("Listing browser extensions…"));
    }
}

void ExtensionsWindow::restoreLayout() {
    const QSettings settings;
    restoreGeometry(settings.value(kGeometryKey).toByteArray());
    if (!settings.contains(kGeometryKey)) {
        resize(1240, 780);
    }
    splitter_->restoreState(settings.value(kSplitterKey).toByteArray());
    table_->horizontalHeader()->restoreState(settings.value(kHeaderKey).toByteArray());
}

void ExtensionsWindow::saveLayout() const {
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kSplitterKey, splitter_->saveState());
    settings.setValue(kHeaderKey, table_->horizontalHeader()->saveState());
}

// --- Listing -----------------------------------------------------------------------------------

void ExtensionsWindow::refresh() {
    if (work_ != Work::None) {
        return;
    }
    work_ = Work::Listing;
    workLabel_->setText(tr("Listing browser extensions…"));
    updateActions();
    // The listing's LiveEnvironment lives only inside the task: it keeps signed-out users' hives
    // loaded privately, and a change to one of them can run only once it is gone. The pool runs
    // one task at a time, so a change of this window never overlaps it; the hive lock keeps the
    // other windows' changes out too (hive_access.h).
    pool_.start([this, inventory = inventory_] {
        auto listing = std::make_shared<ExtensionsListing>();
        listing->inventory =
            inventory ? inventory : std::make_shared<const inventory::Inventory>(scanInventory());
        // Built once per process from the machine's folders and profiles; the removal's planning
        // asks it on the GUI thread.
        static_cast<void>(leftovers::HardFloor::live());
        const auto started = std::chrono::steady_clock::now();
        {
            const auto hives = sharedHiveAccess();
            const leftovers::LiveEnvironment live;
            tools::BrowserExtensionsEnvironment environment;
            environment.scan = live.environment();
            environment.programs = listing->inventory->programs;
            listing->list = tools::list_browser_extensions(environment);
            listing->environmentDiagnostics = environment.scan.diagnostics;
            listing->currentUserSid = environment.scan.current_user_sid;
        }
        listing->shellUserSid = shellUserSid();
        listing->elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        QMetaObject::invokeMethod(
            this,
            [this, listing = std::shared_ptr<const ExtensionsListing>(std::move(listing))] {
                onListed(listing);
            },
            Qt::QueuedConnection);
    });
}

void ExtensionsWindow::onListed(std::shared_ptr<const ExtensionsListing> listing) {
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
    populateFilters();
    selectIds(selected, currentId);
    updateCounts();
    updateDiagnostics();
    onSelectionChanged();  // The model reset cleared the selection without a selectionChanged signal
}

void ExtensionsWindow::populateFilters() {
    const auto* listing = model_->listing();

    // The browsers found, in BrowserKind's order.
    std::set<int> browsers;
    std::map<QString, QString> users;  // By name, then SID
    if (listing != nullptr) {
        for (const auto& profile : listing->list.profiles) {
            browsers.insert(static_cast<int>(profile.browser));
        }
        for (const auto& entry : listing->list.extensions) {
            browsers.insert(static_cast<int>(entry.browser));
            if (!entry.user_sid.empty()) {
                users.emplace(model_->userText(entry), toQString(entry.user_sid));
            }
        }
    }
    {
        const int chosen = browserCombo_->currentData().toInt();
        const QSignalBlocker blocker(browserCombo_);
        while (browserCombo_->count() > 1) {
            browserCombo_->removeItem(1);
        }
        for (const int browser : browsers) {
            browserCombo_->addItem(ExtensionsText::browserName(static_cast<tools::BrowserKind>(browser)),
                                   browser);
        }
        browserCombo_->setCurrentIndex(std::max(browserCombo_->findData(chosen), 0));
        const int browser = browserCombo_->currentData().toInt();
        filter_->setBrowser(browser < 0 ? std::nullopt
                                        : std::optional(static_cast<tools::BrowserKind>(browser)));
    }
    {
        const QString chosen = userCombo_->currentData().toString();
        const QSignalBlocker blocker(userCombo_);
        while (userCombo_->count() > 2) {
            userCombo_->removeItem(2);
        }
        for (const auto& [name, sid] : users) {
            userCombo_->addItem(name, sid);
        }
        userCombo_->setCurrentIndex(std::max(userCombo_->findData(chosen), 0));
        const QString sid = userCombo_->currentData().toString();
        filter_->setUser(sid == kAllUsers ? std::nullopt : std::optional(sid.toStdWString()));
    }
}

void ExtensionsWindow::selectIds(const std::vector<std::wstring>& ids, const std::wstring& current) {
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

std::vector<const BrowserExtension*> ExtensionsWindow::selectedEntries() const {
    std::vector<const BrowserExtension*> entries;
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

const BrowserExtension* ExtensionsWindow::currentEntry() const {
    const QItemSelectionModel* selection = table_->selectionModel();
    QModelIndex index = table_->currentIndex();
    if (!index.isValid() || !selection->isRowSelected(index.row())) {
        const QModelIndexList rows = selection->selectedRows();
        index = rows.isEmpty() ? QModelIndex() : rows.front();
    }
    return index.isValid() ? model_->entryAt(filter_->mapToSource(index).row()) : nullptr;
}

void ExtensionsWindow::onSelectionChanged() {
    renderDetails();
    updateActions();
}

QString ExtensionsWindow::pageUnavailableReason(const BrowserExtension& entry) const {
    const auto* listing = model_->listing();
    if (listing == nullptr) {
        return {};
    }
    if (entry.page.executable.empty()) {
        return tr("%1 wasn't found on this PC, so its extensions page can't be opened from here.")
            .arg(ExtensionsText::browserName(entry.browser));
    }
    if (listing->shellUserSid.empty()) {
        return tr(
            "Windows' desktop (Explorer) isn't running, so Avel can't start the browser without "
            "administrator rights.");
    }
    if (!entry.user_sid.empty() && !platform::iequals(entry.user_sid, listing->shellUserSid)) {
        return tr("This profile belongs to %1: only the profiles of the user signed in to this desktop open "
                  "from here.")
            .arg(model_->userText(entry));
    }
    return {};
}

QString ExtensionsWindow::policiesJsonOf(const BrowserExtension& entry,
                                         const tools::BrowserExtensionsList& list) {
    for (const std::size_t s : entry.sources) {
        if (s < list.sources.size() && list.sources[s].kind == ExtensionSourceKind::PoliciesJson &&
            !list.sources[s].file.empty()) {
            return toQString(list.sources[s].file);
        }
    }
    return {};
}

const ExtensionSource* ExtensionsWindow::firstRegistrySource(const BrowserExtension& entry,
                                                             const tools::BrowserExtensionsList& list) {
    for (const std::size_t s : entry.sources) {
        if (s < list.sources.size() && list.sources[s].kind != ExtensionSourceKind::PoliciesJson) {
            return &list.sources[s];
        }
    }
    return nullptr;
}

void ExtensionsWindow::updateActions() {
    const auto* listing = model_->listing();
    const auto entries = selectedEntries();
    const bool idle = work_ == Work::None;
    const bool canChange = idle && stores_->store() != nullptr;
    const bool anyRemovable = std::any_of(entries.begin(), entries.end(), [](const BrowserExtension* e) {
        return e->removal == ExtensionRemoval::Removable;
    });
    const bool anyId = std::any_of(entries.begin(), entries.end(),
                                   [](const BrowserExtension* e) { return !e->extension_id.empty(); });

    refreshAction_->setEnabled(idle);
    removeAction_->setEnabled(canChange && anyRemovable);
    removeAction_->setToolTip(
        stores_->store() == nullptr
            ? stores_->unavailableReason()
            : tr("Remove the policy entries and registrations that install the selected extensions (Del). "
                 "Avel asks first; the browser removes the extensions at its next start."));
    undoAction_->setEnabled(canChange && !undoStack_.empty());
    undoAction_->setToolTip(
        undoStack_.empty()
            ? tr("Undo the last change made in this window (Ctrl+Z)")
            : tr("Undo “%1” (Ctrl+Z)").arg(toQString(undoStack_.back().change->plan.description)));
    // The Undo Center restores on its own; a listing of signed-out users' hives must not overlap it.
    undoCenterAction_->setEnabled(idle);

    const auto* entry = currentEntry();
    const QString pageReason = entry != nullptr ? pageUnavailableReason(*entry) : QString();
    openPageAction_->setEnabled(entry != nullptr && listing != nullptr && pageReason.isEmpty());
    openPageAction_->setToolTip(
        pageReason.isEmpty() ? tr("Open the selected profile's extensions page in its browser, as you, "
                                  "without administrator rights")
                             : pageReason);
    const QString policies =
        entry != nullptr && listing != nullptr ? policiesJsonOf(*entry, listing->list) : QString();
    openPoliciesJsonAction_->setEnabled(!policies.isEmpty() && QFileInfo::exists(policies));
    copyIdAction_->setEnabled(anyId);
    openRegistryAction_->setEnabled(entry != nullptr && listing != nullptr &&
                                    firstRegistrySource(*entry, listing->list) != nullptr);
    openProfileAction_->setEnabled(entry != nullptr && entry->in_profile && !entry->profile_path.empty() &&
                                   QFileInfo(toQString(entry->profile_path)).isDir());
}

void ExtensionsWindow::updateCounts() {
    const auto* listing = model_->listing();
    if (listing == nullptr) {
        countLabel_->clear();
        return;
    }
    const auto& extensions = listing->list.extensions;
    const int total = model_->rowCount();
    const int shown = filter_->rowCount();
    const auto removable = static_cast<int>(std::count_if(
        extensions.begin(), extensions.end(),
        [](const BrowserExtension& entry) { return entry.removal == ExtensionRemoval::Removable; }));
    const auto disabled = static_cast<int>(
        std::count_if(extensions.begin(), extensions.end(),
                      [](const BrowserExtension& entry) { return entry.in_profile && !entry.enabled; }));
    QString text = shown == total ? tr("%n extension(s)", nullptr, total)
                                  : tr("Showing %1 of %n extension(s)", nullptr, total).arg(shown);
    text += u", "_s + tr("%n disabled", nullptr, disabled);
    text += u", "_s + tr("%n removable by Avel", nullptr, removable);
    countLabel_->setText(text);
}

void ExtensionsWindow::updateDiagnostics() {
    QStringList lines;
    if (stores_->store() == nullptr) {
        lines << stores_->unavailableReason();
    }
    if (const auto* listing = model_->listing()) {
        if (listing->list.policies_managed) {
            lines << ExtensionsText::managedText();
        }
        for (const auto& diagnostic : listing->environmentDiagnostics) {
            lines << LeftoverText::diagnosticText(diagnostic);
        }
        for (const auto& diagnostic : listing->list.diagnostics) {
            lines << ExtensionsText::diagnosticText(diagnostic);
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

void ExtensionsWindow::renderDetails() {
    const auto* entry = currentEntry();
    const auto* listing = model_->listing();
    if (entry == nullptr || listing == nullptr) {
        details_->setHtml(u"<p style=\"color: gray\">"_s +
                          (listing == nullptr ? tr("Listing browser extensions…")
                                              : tr("Select an extension to see its details."))
                              .toHtmlEscaped() +
                          u"</p>"_s);
        return;
    }
    const BrowserExtension& e = *entry;
    const auto& list = listing->list;

    QString rows;
    const auto row = [&rows](const QString& label, const QString& valueHtml) {
        if (!valueHtml.isEmpty()) {
            rows += u"<tr><td style=\"color: gray; padding-right: 12px; white-space: nowrap\">"_s +
                    label.toHtmlEscaped() + u"</td><td>"_s + valueHtml + u"</td></tr>"_s;
        }
    };
    const auto codes = [](const std::vector<std::wstring>& items) {
        QStringList parts;
        for (const auto& item : items) {
            parts << code(item);
        }
        return parts.join(u", "_s);
    };

    row(tr("Description"), escaped(e.description));
    row(tr("Extension id"), code(e.extension_id));
    row(tr("Version"), escaped(e.version));
    row(tr("Browser"), ExtensionsText::browserName(e.browser).toHtmlEscaped());
    if (e.in_profile) {
        row(tr("Profile"), ExtensionsText::profileText(e).toHtmlEscaped() + u"<br>"_s + code(e.profile_path));
    } else {
        row(tr("Profile"), gray(ExtensionsText::profileText(e)));
    }
    row(tr("User"), e.user_sid.empty() ? model_->userText(e).toHtmlEscaped()
                                       : model_->userText(e).toHtmlEscaped() + u"<br>"_s + code(e.user_sid));
    QString state = ExtensionsText::stateText(e).toHtmlEscaped();
    if (const QString details = ExtensionsText::stateDetails(e); !details.isEmpty()) {
        state += u"<br>"_s + gray(details);
    }
    row(tr("State"), state);
    QString install = ExtensionsText::installText(e.install).toHtmlEscaped() + u"<br>"_s +
                      gray(ExtensionsText::installDetails(e.install));
    if (e.installed_by_default) {
        install += u"<br>"_s + gray(tr("Installed with the browser or by the PC's maker"));
    }
    row(tr("Installed by"), install);
    row(tr("Store"),
        e.from_store ? ExtensionsText::storeText(e.store).toHtmlEscaped() : gray(tr("Not from a store")));
    row(tr("Update URL"), e.update_url.empty() ? QString() : code(e.update_url));
    row(tr("Installed on"), ExtensionsText::installTimeText(e.install_time).toHtmlEscaped());
    row(tr("Files"), e.folder.empty() ? QString() : code(e.folder));
    row(tr("Permissions"), codes(e.permissions));
    row(tr("Site access"), codes(e.host_permissions));
    row(tr("Optional permissions"), codes(e.optional_permissions));
    // Why it can't be removed here is a note above.
    row(tr("Removal"), e.removal == ExtensionRemoval::Removable
                           ? ExtensionsText::removalText(e.removal).toHtmlEscaped() + u"<br>"_s +
                                 gray(ExtensionsText::removalDetails(e.removal))
                           : ExtensionsText::removalText(e.removal).toHtmlEscaped());
    const QString pageReason = pageUnavailableReason(e);
    if (!e.page.executable.empty()) {
        std::vector<std::wstring> command{e.page.executable};
        command.insert(command.end(), e.page.arguments.begin(), e.page.arguments.end());
        QString page = code(joinArguments(command));
        page += u"<br>"_s + (pageReason.isEmpty()
                                 ? link(QString(kPageScheme) + u':', tr("Open the extensions page"))
                                 : gray(pageReason));
        row(tr("Extensions page"), page);
    } else {
        row(tr("Extensions page"), gray(pageReason));
    }
    row(tr("Id"), code(e.id));

    // What installs it from outside the browser.
    QString sources;
    for (const std::size_t s : e.sources) {
        if (s >= list.sources.size()) {
            continue;
        }
        const ExtensionSource& source = list.sources[s];
        const QString index = QString::number(s);
        QString block = u"<p><b>"_s + ExtensionsText::sourceKindText(source.kind).toHtmlEscaped() +
                        u"</b><br>"_s + gray(ExtensionsText::sourceKindDetails(source.kind)) + u"<br>"_s;
        if (source.kind == ExtensionSourceKind::PoliciesJson) {
            block += code(source.file) + u" "_s +
                     link(QString(kPoliciesScheme) + u':' + index, tr("Show in Explorer"));
        } else {
            block += sourceWhere(source).toHtmlEscaped() + u" "_s +
                     link(QString(kRegeditScheme) + u':' + index, tr("Open in Registry Editor"));
            if (source.wow64) {
                block += u"<br>"_s + gray(tr("In the registry's 32-bit view"));
            }
        }
        if (!source.data.empty()) {
            block += u"<br>"_s + tr("Data: %1").toHtmlEscaped().arg(code(source.data));
        }
        if (!source.installation_mode.empty()) {
            block += u"<br>"_s + tr("Mode: %1").toHtmlEscaped().arg(code(source.installation_mode));
        }
        if (source.kind != ExtensionSourceKind::PoliciesJson) {
            block += u"<br>"_s + tr("Applies to: %1")
                                     .arg(ExtensionsText::userText(source.user_sid, source.user_name,
                                                                   listing->currentUserSid))
                                     .toHtmlEscaped();
        }
        if (!source.program_id.empty()) {
            const QString program = model_->programName(source.program_id);
            block += u"<br>"_s + tr("Program: %1")
                                     .arg(program.isEmpty() ? toQString(source.program_id) : program)
                                     .toHtmlEscaped();
        }
        if (source.ignored_by_browser) {
            block += u"<br>"_s + warning(ExtensionsText::ignoredByBrowserText());
        }
        sources += block + u"</p>"_s;
    }

    QStringList notes;
    if (e.removal != ExtensionRemoval::Removable) {
        notes << ExtensionsText::removalDetails(e.removal);
    }
    if (e.keeps_user_copy) {
        notes << ExtensionsText::keepsUserCopyText();
    }
    if (e.removal == ExtensionRemoval::Removable && list.policies_managed) {
        notes << ExtensionsText::managedText();
    }
    QString notesHtml;
    for (const auto& note : notes) {
        notesHtml += u"<p>"_s + warning(note) + u"</p>"_s;
    }
    QString header = u"<h3>"_s + ExtensionsText::nameText(e).toHtmlEscaped() + u"</h3>"_s;
    if (const auto selected = selectedEntries().size(); selected > 1) {
        header += u"<p>"_s +
                  gray(tr("%n extension(s) selected; Remove applies to the ones Avel can remove. Details of "
                          "this one:",
                          nullptr, static_cast<int>(selected))) +
                  u"</p>"_s;
    }
    QString html = header + notesHtml + u"<table>"_s + rows + u"</table>"_s;
    if (!sources.isEmpty()) {
        html += u"<h4>"_s + tr("What installs it from outside the browser").toHtmlEscaped() + u"</h4>"_s +
                sources;
    }
    details_->setHtml(html);
}

void ExtensionsWindow::onDetailsLink(const QUrl& url) {
    const auto* listing = model_->listing();
    if (listing == nullptr) {
        return;
    }
    const QString scheme = url.scheme();
    if (scheme == kPageScheme) {
        openExtensionsPage();
        return;
    }
    bool ok = false;
    const auto index = static_cast<std::size_t>(url.path().toULongLong(&ok));
    if (!ok || index >= listing->list.sources.size()) {
        return;
    }
    const ExtensionSource& source = listing->list.sources[index];
    if (scheme == kRegeditScheme) {
        openInRegistryEditor(source);
    } else if (scheme == kPoliciesScheme) {
        const std::wstring file = QDir::toNativeSeparators(toQString(source.file)).toStdWString();
        if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(file.c_str())) {
            ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ::ILFree(item);
        }
    }
}

// --- Removal -----------------------------------------------------------------------------------

void ExtensionsWindow::confirmRemove() {
    if (work_ != Work::None || stores_->store() == nullptr) {
        return;
    }
    const auto* listing = model_->listing();
    const auto entries = selectedEntries();
    if (listing == nullptr || entries.empty()) {
        return;
    }
    const auto& list = listing->list;
    std::vector<std::wstring> ids;
    for (const auto* entry : entries) {
        ids.push_back(entry->id);
    }
    const auto nameOf = [this](const std::wstring& id) {
        const auto* entry = model_->entryWithId(id);
        return entry != nullptr ? ExtensionsText::nameText(*entry) : toQString(id);
    };
    // "Contoso Helper (Microsoft Edge, Default, Ann)".
    const auto where = [this](const BrowserExtension& entry) {
        return tr("%1 (%2, %3, %4)")
            .arg(ExtensionsText::nameText(entry), ExtensionsText::browserName(entry.browser),
                 ExtensionsText::profileText(entry), model_->userText(entry));
    };
    auto planned = tools::plan_remove_extensions(list, ids, [](const leftovers::Location& location) {
        return leftovers::HardFloor::live().check(location);
    });

    auto change = std::make_shared<Change>();
    for (const auto& issue : planned.issues) {
        change->skipped << tr("%1: %2", "extension name: why it was skipped")
                               .arg(nameOf(issue.id), ExtensionsText::issueText(issue.code));
    }
    if (planned.planned.empty() || planned.plan.items.empty()) {
        showResult(tr("Nothing to remove: %n selected extension(s) skipped.", nullptr,
                      static_cast<int>(change->skipped.size())),
                   change->skipped);
        return;
    }

    // The planned entries' sources, and the extensions they are (by extension id, else name).
    std::set<std::size_t> removedSources;
    std::set<QString> extensions;
    for (const auto& id : planned.planned) {
        if (const auto* entry = model_->entryWithId(id)) {
            removedSources.insert(entry->sources.begin(), entry->sources.end());
            extensions.insert(entry->extension_id.empty() ? ExtensionsText::nameText(*entry)
                                                          : toQString(entry->extension_id).toLower());
        }
    }
    const int count = static_cast<int>(extensions.size());
    change->extensions = count;
    const QString singleName = count == 1 ? nameOf(planned.planned.front()) : QString();
    change->plan = std::move(planned.plan);
    change->plan.description = ExtensionsText::planDescription(count, singleName).toStdWString();

    QStringList items;
    for (const auto& item : change->plan.items) {
        items << ExtensionsText::itemText(item.location);
    }
    // The other entries the same sources install: gone with them, or kept by another source.
    QStringList alsoRemoved;
    QStringList stillInstalled;
    for (const auto& id : planned.affected) {
        if (std::find(planned.planned.begin(), planned.planned.end(), id) != planned.planned.end()) {
            continue;
        }
        const auto* entry = model_->entryWithId(id);
        if (entry == nullptr) {
            continue;
        }
        const bool all = std::all_of(entry->sources.begin(), entry->sources.end(),
                                     [&](std::size_t s) { return removedSources.contains(s); });
        (all ? alsoRemoved : stillInstalled) << where(*entry);
    }
    QStringList userCopies;
    for (const auto& note : planned.notes) {
        if (note.code == tools::ExtensionPlanNoteCode::KeepsUserCopy) {
            if (const auto* entry = model_->entryWithId(note.id)) {
                userCopies << where(*entry);
            }
        }
    }

    QString informative =
        tr("Avel takes out what installs it from outside the browser:") + u'\n' + bullets(items);
    if (!alsoRemoved.isEmpty()) {
        informative += u"\n\n"_s +
                       tr("The same policies or registrations install it here too, so it goes from these as "
                          "well:") +
                       u'\n' + bullets(alsoRemoved);
    }
    if (!stillInstalled.isEmpty()) {
        informative += u"\n\n"_s +
                       tr("These lose one of their sources too, but something else still installs them:") +
                       u'\n' + bullets(stillInstalled);
    }
    if (!userCopies.isEmpty()) {
        informative += u"\n\n"_s + ExtensionsText::keepsUserCopyText() + u'\n' + bullets(userCopies);
    }
    informative += u"\n\n"_s + ExtensionsText::nextStartText();
    if (list.policies_managed) {
        informative += u"\n\n"_s + ExtensionsText::managedText();
    }
    informative +=
        u"\n\n"_s + tr("Avel backs it up first: Undo last change (Ctrl+Z) or the Undo Center puts it back "
                       "within 30 days.");
    if (!change->skipped.isEmpty()) {
        informative += u"\n\n"_s +
                       tr("%n selected extension(s) will be skipped:", nullptr,
                          static_cast<int>(change->skipped.size())) +
                       u'\n' + change->skipped.join(u'\n');
    }

    auto* box = new QMessageBox(QMessageBox::Warning, tr("Remove browser extensions"),
                                count == 1 ? tr("Remove the browser extension %1?").arg(singleName)
                                           : tr("Remove %n browser extension(s)?", nullptr, count),
                                QMessageBox::NoButton, this);
    box->setObjectName(u"removeExtensionsConfirmation"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextFormat(Qt::PlainText);
    box->setInformativeText(informative);
    QPushButton* remove = box->addButton(tr("Remove"), QMessageBox::DestructiveRole);
    remove->setObjectName(u"removeExtensionsConfirmButton"_s);
    box->addButton(QMessageBox::Cancel)->setObjectName(u"removeExtensionsCancelButton"_s);
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

void ExtensionsWindow::startChange(std::shared_ptr<const Change> change) {
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
            result = changes::execute(*store, change->plan);
        } else {
            busy = true;
        }
        QMetaObject::invokeMethod(
            this, [this, change, busy, result] { onChangeFinished(change, busy, result); },
            Qt::QueuedConnection);
    });
}

void ExtensionsWindow::onHiveWait(bool waiting) {
    if (waiting) {
        workText_ = workLabel_->text();
        workLabel_->setText(hiveWaitText());
    } else {
        workLabel_->setText(workText_);
    }
}

void ExtensionsWindow::onChangeFinished(const std::shared_ptr<const Change>& change, bool busy,
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

    int done = 0;
    QStringList details;
    for (std::size_t i = 0; i < result.items.size() && i < change->plan.items.size(); ++i) {
        const auto& status = result.items[i].status;
        const auto& location = change->plan.items[i].location;
        if (finished(status)) {
            ++done;
        } else {
            details << tr("%1: %2", "change: its status")
                           .arg(ExtensionsText::itemText(location),
                                LeftoverText::statusText(status, location));
        }
    }
    const int failed = static_cast<int>(details.size());
    const int skipped = static_cast<int>(change->skipped.size());
    QString text;
    if (failed == 0) {
        text = tr("%n browser extension(s) removed; the browser drops them at its next start.", nullptr,
                  change->extensions);
    } else if (done > 0) {
        text = tr("Removed in part: %n change(s) couldn't be made.", nullptr, failed);
    } else {
        text = tr("Nothing was removed: %n change(s) couldn't be made.", nullptr, failed);
    }
    if (skipped > 0) {
        text += u' ' + tr("%n skipped.", nullptr, skipped);
    }
    text += u' ' + tr("Undo with Ctrl+Z or in the Undo Center.");
    showResult(text, details + change->skipped, failed > 0);
    refresh();
}

void ExtensionsWindow::undoLast() {
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

void ExtensionsWindow::onUndoFinished(const std::shared_ptr<const Change>& change, const std::wstring& setId,
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
    QStringList details;
    for (const auto& [index, item] : result.items) {
        if (item.status.state != changes::ItemState::Restored && index < change->plan.items.size()) {
            const auto& location = change->plan.items[index].location;
            details << tr("%1: %2", "change: its status")
                           .arg(ExtensionsText::itemText(location),
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

void ExtensionsWindow::openUndoCenter() {
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

void ExtensionsWindow::showResult(const QString& text, const QStringList& details, bool problem) {
    resultLabel_->setText(problem ? u"⚠ "_s + text : text);
    resultLabel_->setToolTip(details.join(u'\n'));
}

// --- Context menu and the other tools ----------------------------------------------------------

void ExtensionsWindow::showTableMenu(const QPoint& position) {
    if (!table_->indexAt(position).isValid()) {
        return;
    }
    QMenu menu(this);
    addCopy(menu, removeAction_);
    addCopy(menu, openPageAction_);
    if (openPoliciesJsonAction_->isEnabled()) {
        addCopy(menu, openPoliciesJsonAction_);
    }
    menu.addSeparator();
    addCopy(menu, copyIdAction_);
    if (openRegistryAction_->isEnabled()) {
        addCopy(menu, openRegistryAction_);
    }
    addCopy(menu, openProfileAction_);
    menu.exec(table_->viewport()->mapToGlobal(position));
}

void ExtensionsWindow::openExtensionsPage() {
    const auto* entry = currentEntry();
    if (entry == nullptr || model_->listing() == nullptr) {
        return;
    }
    if (const QString reason = pageUnavailableReason(*entry); !reason.isEmpty()) {
        showResult(reason, {}, true);
        return;
    }
    // Never started from here: avel.exe is elevated, and so would the browser be.
    const HRESULT hr = launchUnelevated(entry->page.executable, joinArguments(entry->page.arguments));
    const QString browser = ExtensionsText::browserName(entry->browser);
    if (FAILED(hr)) {
        showResult(tr("%1's extensions page couldn't be opened: Windows' desktop didn't take the request. %2")
                       .arg(browser, ProgramText::systemErrorText(static_cast<std::uint32_t>(hr))),
                   {}, true);
        return;
    }
    showResult(tr("Opening %1's extensions page…").arg(browser));
}

void ExtensionsWindow::openPoliciesJson() {
    const auto* listing = model_->listing();
    const auto* entry = currentEntry();
    const QString file =
        listing != nullptr && entry != nullptr ? policiesJsonOf(*entry, listing->list) : QString();
    if (file.isEmpty()) {
        return;
    }
    // Explorer's own "select this file": the file is shown, never opened for editing.
    const std::wstring native = QDir::toNativeSeparators(file).toStdWString();
    if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(native.c_str())) {
        ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ::ILFree(item);
    }
}

void ExtensionsWindow::openProfileFolder() {
    const auto* entry = currentEntry();
    if (entry == nullptr || !entry->in_profile || entry->profile_path.empty()) {
        return;
    }
    const std::wstring folder = QDir::toNativeSeparators(toQString(entry->profile_path)).toStdWString();
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ExtensionsWindow::copyExtensionId() {
    QStringList ids;
    for (const auto* entry : selectedEntries()) {
        if (!entry->extension_id.empty() && !ids.contains(toQString(entry->extension_id))) {
            ids << toQString(entry->extension_id);
        }
    }
    if (!ids.isEmpty()) {
        QGuiApplication::clipboard()->setText(ids.join(u"\r\n"_s));
    }
}

void ExtensionsWindow::openInRegistryEditor(const ExtensionSource& source) {
    if (source.kind == ExtensionSourceKind::PoliciesJson) {
        return;
    }
    const QString path = regeditPath(source.key);
    if (path.isEmpty()) {
        showResult(
            tr("%1 is signed out, so their registry isn't loaded where Registry Editor sees it. In Registry "
               "Editor, File > Load Hive opens their NTUSER.DAT.")
                .arg(toQString(source.user_name.empty() ? source.user_sid : source.user_name)),
            {}, true);
        return;
    }
    openRegistryEditor(path);
}

}  // namespace avel::app
