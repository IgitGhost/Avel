#include "main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QToolBar>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include "change_set_progress_dialog.h"
#include "change_store_access.h"
#include "core/force/force.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/orphans.h"
#include "core/leftovers/post_uninstall.h"
#include "core/platform/elevation.h"
#include "core/platform/processes.h"
#include "core/uninstall/registration.h"
#include "details_view.h"
#include "icon_loader.h"
#include "inventory_loader.h"
#include "language_manager.h"
#include "leftover_dialog.h"
#include "program_filter_model.h"
#include "program_table_model.h"
#include "program_text.h"
#include "running_processes_dialog.h"
#include "undo_center_dialog.h"
#include "uninstall_dialog.h"
#include "uninstall_progress_dialog.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

constexpr int kIconSize = 20;  // Logical pixels

// Settings keys for the window layout.
constexpr auto kGeometryKey = "window/geometry"_L1;
constexpr auto kStateKey = "window/state"_L1;
constexpr auto kSplitterKey = "window/splitter"_L1;
constexpr auto kHeaderKey = "window/programHeader"_L1;
constexpr auto kCategoryKey = "window/category"_L1;

// The orphan finder's items all go under one heading: their own owner ids name each folder.
constexpr wchar_t kOrphansId[] = L"orphans";

// Popup menus show the window's actions through copies. Qt's accessibility keeps one interface
// per action, made for the first menu that shows it; if that is a popup, which is gone once it
// closes, screen readers and UI Automation no longer see the action in the menu bar.
void addCopy(QMenu& menu, QAction* action) {
    QAction* copy = menu.addAction(action->icon(), action->text());
    copy->setEnabled(action->isEnabled());
    copy->setShortcut(action->shortcut());
    copy->setToolTip(action->toolTip());
    QObject::connect(copy, &QAction::triggered, action, &QAction::trigger);
}

}  // namespace

// What the Hunter found about a window, read on the worker pool.
struct MainWindow::Hunt {
    HunterTarget::Window window;
    QPoint position;
    std::shared_ptr<const inventory::Inventory> inventory;  // Keeps owner.program alive
    std::wstring image;      // The process's executable; empty when Windows won't tell
    bool critical = false;   // Ending it would stop Windows
    bool service = false;    // It hosts a running service
    force::FileOwner owner;  // The installed program, or the folder to force
    leftovers::ProtectionReason folderProtection = leftovers::ProtectionReason::None;
};

MainWindow::MainWindow(LanguageManager& languages, QWidget* parent)
    : QMainWindow(parent), languages_(languages), elevated_(platform::is_process_elevated()) {
    // The product name is the same in every language.
    setWindowTitle(u"Avel"_s);

    model_ = new ProgramTableModel(this);
    filter_ = new ProgramFilterModel(this);
    filter_->setSourceProgramModel(model_);
    loader_ = new InventoryLoader(this);
    iconLoader_ = new IconLoader(static_cast<int>(std::ceil(kIconSize * devicePixelRatioF())), this);
    stores_ = new ChangeStoreAccess(this);
    workers_.setMaxThreadCount(1);

    createActions();
    createMenus();
    createToolBar();
    createCentralArea();
    createStatusBar();
    retranslateUi();
    restoreLayout();

    connect(loader_, &InventoryLoader::started, this, &MainWindow::onScanStarted);
    connect(loader_, &InventoryLoader::finished, this, &MainWindow::onScanFinished);
    connect(iconLoader_, &IconLoader::iconLoaded, model_, &ProgramTableModel::setIcon);
    connect(filter_, &QAbstractItemModel::rowsInserted, this, &MainWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::rowsRemoved, this, &MainWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::modelReset, this, &MainWindow::updateCounts);
    connect(filter_, &QAbstractItemModel::layoutChanged, this, &MainWindow::updateCounts);

    connect(stores_, &ChangeStoreAccess::maintenanceFinished, this, &MainWindow::onMaintenanceFinished);

    loader_->start();
    stores_->startMaintenance();
}

void MainWindow::createActions() {
    refreshAction_ = new QAction(style()->standardIcon(QStyle::SP_BrowserReload), QString(), this);
    refreshAction_->setShortcut(QKeySequence::Refresh);
    connect(refreshAction_, &QAction::triggered, loader_, &InventoryLoader::start);

    uninstallAction_ = new QAction(style()->standardIcon(QStyle::SP_TrashIcon), QString(), this);
    uninstallAction_->setShortcut(QKeySequence::Delete);
    uninstallAction_->setEnabled(false);
    connect(uninstallAction_, &QAction::triggered, this, &MainWindow::uninstallSelected);

    forceAction_ = new QAction(this);
    forceAction_->setShortcut(Qt::SHIFT | Qt::Key_Delete);
    forceAction_->setEnabled(false);
    connect(forceAction_, &QAction::triggered, this, &MainWindow::forceSelected);

    forceFolderAction_ = new QAction(this);
    connect(forceFolderAction_, &QAction::triggered, this, &MainWindow::forceFolder);

    openLocationAction_ = new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon), QString(), this);
    openLocationAction_->setEnabled(false);
    connect(openLocationAction_, &QAction::triggered, this, &MainWindow::openInstallLocation);

    previewLeftoversAction_ = new QAction(this);
    previewLeftoversAction_->setEnabled(false);
    connect(previewLeftoversAction_, &QAction::triggered, this, &MainWindow::previewLeftovers);

    orphansAction_ = new QAction(this);
    orphansAction_->setEnabled(false);
    connect(orphansAction_, &QAction::triggered, this, &MainWindow::findOrphans);

    undoCenterAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowBack), QString(), this);
    connect(undoCenterAction_, &QAction::triggered, this, &MainWindow::openUndoCenter);

    findAction_ = new QAction(this);
    findAction_->setShortcut(QKeySequence::Find);
    connect(findAction_, &QAction::triggered, this, [this] {
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->selectAll();
    });
    addAction(findAction_);

    exitAction_ = new QAction(this);
    connect(exitAction_, &QAction::triggered, this, &QWidget::close);

    languageGroup_ = new QActionGroup(this);
    languageGroup_->setExclusive(true);

    // An empty code means "follow the system language".
    systemLanguageAction_ = new QAction(this);
    systemLanguageAction_->setData(QString());
    languageGroup_->addAction(systemLanguageAction_);
    for (const auto& language : LanguageManager::supportedLanguages()) {
        auto* action = new QAction(language.nativeName, this);
        action->setData(language.code);
        languageGroup_->addAction(action);
    }
    for (QAction* action : languageGroup_->actions()) {
        action->setCheckable(true);
        action->setChecked(action->data().toString() == languages_.preference());
    }
    connect(languageGroup_, &QActionGroup::triggered, this,
            [this](QAction* action) { languages_.setPreference(action->data().toString()); });

    aboutAction_ = new QAction(this);
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::showAbout);

    // Showing Qt's version and license from the GUI is part of meeting the LGPL.
    aboutQtAction_ = new QAction(this);
    connect(aboutQtAction_, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::createMenus() {
    fileMenu_ = menuBar()->addMenu(QString());
    fileMenu_->addAction(refreshAction_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(uninstallAction_);
    fileMenu_->addAction(forceAction_);
    fileMenu_->addAction(forceFolderAction_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(openLocationAction_);
    fileMenu_->addAction(previewLeftoversAction_);
    fileMenu_->addSeparator();
    fileMenu_->addAction(exitAction_);

    viewMenu_ = menuBar()->addMenu(QString());
    languageMenu_ = viewMenu_->addMenu(QString());
    languageMenu_->addAction(systemLanguageAction_);
    languageMenu_->addSeparator();
    for (QAction* action : languageGroup_->actions()) {
        if (action != systemLanguageAction_) {
            languageMenu_->addAction(action);
        }
    }

    toolsMenu_ = menuBar()->addMenu(QString());
    toolsMenu_->addAction(orphansAction_);
    toolsMenu_->addAction(undoCenterAction_);

    helpMenu_ = menuBar()->addMenu(QString());
    helpMenu_->addAction(aboutAction_);
    helpMenu_->addAction(aboutQtAction_);
}

void MainWindow::createToolBar() {
    toolBar_ = addToolBar(QString());
    toolBar_->setObjectName(u"mainToolBar"_s);
    toolBar_->setMovable(false);
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar_->addAction(refreshAction_);
    toolBar_->addAction(uninstallAction_);
    hunter_ = new HunterTarget(toolBar_);
    hunter_->setObjectName(u"hunter"_s);
    hunter_->setEnabled(false);
    connect(hunter_, &HunterTarget::hovering, this, &MainWindow::onWindowHovered);
    connect(hunter_, &HunterTarget::picked, this, &MainWindow::onWindowPicked);
    connect(hunter_, &HunterTarget::cancelled, this, [this] { statusBar()->clearMessage(); });
    toolBar_->addWidget(hunter_);
    toolBar_->addSeparator();

    searchEdit_ = new QLineEdit(toolBar_);
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(260);
    connect(searchEdit_, &QLineEdit::textChanged, filter_, &ProgramFilterModel::setSearchText);
    toolBar_->addWidget(searchEdit_);

    categoryCombo_ = new QComboBox(toolBar_);
    categoryCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    using Category = ProgramFilterModel::Category;
    for (const Category category :
         {Category::Apps, Category::DesktopApps, Category::StoreApps, Category::Updates,
          Category::SystemComponents, Category::Broken, Category::Everything}) {
        categoryCombo_->addItem(QString(), static_cast<int>(category));
    }
    connect(categoryCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        filter_->setCategory(
            static_cast<ProgramFilterModel::Category>(categoryCombo_->itemData(index).toInt()));
        updateCounts();
    });
    toolBar_->addWidget(categoryCombo_);
}

void MainWindow::createCentralArea() {
    table_ = new QTableView(this);
    table_->setModel(filter_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(ProgramTableModel::NameColumn, Qt::AscendingOrder);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setIconSize(QSize(kIconSize, kIconSize));
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->verticalHeader()->setDefaultSectionSize(kIconSize + 8);

    auto* header = table_->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionsMovable(true);
    // The name takes whatever the other columns leave, so the table never scrolls sideways.
    header->setSectionResizeMode(ProgramTableModel::NameColumn, QHeaderView::Stretch);
    header->resizeSection(ProgramTableModel::PublisherColumn, 200);
    header->resizeSection(ProgramTableModel::VersionColumn, 120);
    header->resizeSection(ProgramTableModel::SizeColumn, 80);
    header->resizeSection(ProgramTableModel::DateColumn, 100);
    header->resizeSection(ProgramTableModel::TypeColumn, 110);

    // Selection, not the current index: accessibility tools and programmatic selection change the
    // selection without moving the current index.
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &MainWindow::onSelectionChanged);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::showTableMenu);

    details_ = new DetailsView(this);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(table_);
    splitter_->addWidget(details_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({900, 380});
    setCentralWidget(splitter_);
}

void MainWindow::createStatusBar() {
    countLabel_ = new QLabel(this);
    scanLabel_ = new QLabel(this);
    warningLabel_ = new QLabel(this);
    warningLabel_->hide();
    elevationLabel_ = new QLabel(this);
    statusBar()->addWidget(countLabel_);
    statusBar()->addWidget(scanLabel_);
    statusBar()->addWidget(warningLabel_);
    statusBar()->addPermanentWidget(elevationLabel_);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::retranslateUi() {
    fileMenu_->setTitle(tr("&File"));
    refreshAction_->setText(tr("&Refresh"));
    refreshAction_->setToolTip(tr("Scan installed programs again (F5)"));
    uninstallAction_->setText(tr("&Uninstall…"));
    uninstallAction_->setToolTip(tr("Uninstall the selected programs (Del)"));
    forceAction_->setText(tr("&Force uninstall…"));
    forceAction_->setToolTip(
        tr("Remove the selected programs without their uninstallers, for when they are missing or broken "
           "(Shift+Del)"));
    forceFolderAction_->setText(tr("Force uninstall a fol&der…"));
    forceFolderAction_->setToolTip(tr("Remove a program's folder that no entry in the list describes"));
    openLocationAction_->setText(tr("&Open install folder"));
    previewLeftoversAction_->setText(tr("&Preview leftovers…"));
    previewLeftoversAction_->setToolTip(
        tr("Show what the leftover scan finds for the selected programs, without removing anything"));
    exitAction_->setText(tr("E&xit"));
    toolsMenu_->setTitle(tr("&Tools"));
    orphansAction_->setText(tr("Find &orphaned folders…"));
    orphansAction_->setToolTip(
        tr("Look for folders in Program Files, ProgramData and the users' program folders that no installed "
           "program owns"));
    hunter_->setToolTip(tr(
        "Hunter: drag the crosshair onto a program's window and let go to find the program it belongs to"));
    hunter_->setAccessibleName(tr("Hunter"));
    undoCenterAction_->setText(tr("&Undo Center…"));
    undoCenterAction_->setToolTip(tr("Put back leftovers Avel removed in the last 30 days"));
    viewMenu_->setTitle(tr("&View"));
    languageMenu_->setTitle(tr("&Language"));
    systemLanguageAction_->setText(tr("System default"));
    helpMenu_->setTitle(tr("&Help"));
    aboutAction_->setText(tr("&About Avel"));
    aboutQtAction_->setText(tr("About &Qt"));
    toolBar_->setWindowTitle(tr("Toolbar"));
    searchEdit_->setPlaceholderText(tr("Search by name, publisher or version (Ctrl+F)"));

    using Category = ProgramFilterModel::Category;
    const auto category_name = [](Category category) {
        switch (category) {
            case Category::Apps:
                return tr("Apps");
            case Category::DesktopApps:
                return tr("Desktop apps");
            case Category::StoreApps:
                return tr("Store apps");
            case Category::Updates:
                return tr("Updates");
            case Category::SystemComponents:
                return tr("System components");
            case Category::Everything:
                return tr("Everything");
            case Category::Broken:
                return tr("Broken");
        }
        return QString();
    };
    for (int i = 0; i < categoryCombo_->count(); ++i) {
        categoryCombo_->setItemText(
            i, category_name(static_cast<Category>(categoryCombo_->itemData(i).toInt())));
    }
    categoryCombo_->setToolTip(
        tr("Which programs to show. \"Apps\" matches Settings > Apps. \"Broken\" lists the ones that can't "
           "uninstall themselves."));

    elevationLabel_->setText(elevated_ ? tr("Running as administrator")
                                       : tr("No administrator rights: some features are unavailable"));
    model_->retranslate();
    ensureColumnsFit();
    updateCounts();
    updateDiagnostics();
    if (loader_->isRunning()) {
        scanLabel_->setText(tr("Scanning installed programs…"));
    } else if (inventory_) {
        scanLabel_->setText(tr("Scanned in %1 ms").arg(inventory_->elapsed.count()));
    }
}

void MainWindow::restoreLayout() {
    const QSettings settings;
    restoreGeometry(settings.value(kGeometryKey).toByteArray());
    if (!settings.contains(kGeometryKey)) {
        resize(1280, 760);
    }
    restoreState(settings.value(kStateKey).toByteArray());
    splitter_->restoreState(settings.value(kSplitterKey).toByteArray());
    table_->horizontalHeader()->restoreState(settings.value(kHeaderKey).toByteArray());
    table_->horizontalHeader()->setSectionResizeMode(ProgramTableModel::NameColumn, QHeaderView::Stretch);
    const int category = categoryCombo_->findData(settings.value(kCategoryKey, 0).toInt());
    if (category >= 0) {
        categoryCombo_->setCurrentIndex(category);
    }
}

void MainWindow::saveLayout() const {
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kStateKey, saveState());
    settings.setValue(kSplitterKey, splitter_->saveState());
    settings.setValue(kHeaderKey, table_->horizontalHeader()->saveState());
    settings.setValue(kCategoryKey, categoryCombo_->currentData().toInt());
}

void MainWindow::onScanStarted() {
    refreshAction_->setEnabled(false);
    scanLabel_->setText(tr("Scanning installed programs…"));
}

void MainWindow::onScanFinished(std::shared_ptr<const inventory::Inventory> inventory) {
    // Keep the selection across a refresh.
    std::wstring selected_id;
    if (const auto* program = model_->programAt(filter_->mapToSource(table_->currentIndex()).row())) {
        selected_id = program->id;
    }

    inventory_ = std::move(inventory);
    model_->setInventory(inventory_);
    iconLoader_->load(model_->iconSources());

    refreshAction_->setEnabled(true);
    scanLabel_->setText(tr("Scanned in %1 ms").arg(inventory_->elapsed.count()));
    ensureColumnsFit();
    updateCounts();
    updateDiagnostics();
    updateActions();  // The model reset cleared the selection without a selectionChanged signal

    for (int row = 0; row < filter_->rowCount() && !selected_id.empty(); ++row) {
        const QModelIndex index = filter_->index(row, 0);
        if (model_->programAt(filter_->mapToSource(index).row())->id == selected_id) {
            table_->setCurrentIndex(index);
            table_->scrollTo(index);
            return;
        }
    }
    details_->setProgram(nullptr);
}

void MainWindow::onSelectionChanged() {
    // Show the current row if it's selected, otherwise the first selected row.
    const QItemSelectionModel* selection = table_->selectionModel();
    QModelIndex index = table_->currentIndex();
    if (!index.isValid() || !selection->isRowSelected(index.row())) {
        const QModelIndexList rows = selection->selectedRows();
        index = rows.isEmpty() ? QModelIndex() : rows.front();
    }
    details_->setProgram(index.isValid() ? model_->programAt(filter_->mapToSource(index).row()) : nullptr);
    updateActions();
}

void MainWindow::updateActions() {
    const auto programs = selectedPrograms();
    uninstallAction_->setEnabled(!programs.empty());
    // Packaged apps are Windows' to remove; Force is for classic programs.
    forceAction_->setEnabled(!programs.empty() && forceFlow_ == nullptr &&
                             std::none_of(programs.begin(), programs.end(), [](const auto* program) {
                                 return program->source == inventory::Source::Appx;
                             }));
    forceFolderAction_->setEnabled(forceFlow_ == nullptr);
    openLocationAction_->setEnabled(programs.size() == 1 &&
                                    uninstall::install_location_exists(*programs.front()));
    previewLeftoversAction_->setEnabled(!programs.empty() && !busy_);
    orphansAction_->setEnabled(inventory_ != nullptr && !busy_);
    hunter_->setEnabled(inventory_ != nullptr && !busy_);
}

bool MainWindow::selectProgram(const std::wstring& id) {
    int row = 0;
    while (model_->programAt(row) != nullptr && model_->programAt(row)->id != id) {
        ++row;
    }
    if (model_->programAt(row) == nullptr) {
        return false;
    }
    QModelIndex index = filter_->mapFromSource(model_->index(row, 0));
    if (!index.isValid()) {
        // Hidden by the search or the category: show everything.
        searchEdit_->clear();
        index = filter_->mapFromSource(model_->index(row, 0));
    }
    if (!index.isValid()) {
        categoryCombo_->setCurrentIndex(
            categoryCombo_->findData(static_cast<int>(ProgramFilterModel::Category::Everything)));
        index = filter_->mapFromSource(model_->index(row, 0));
    }
    table_->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table_->scrollTo(index);
    return true;
}

std::vector<const inventory::Program*> MainWindow::selectedPrograms() const {
    std::vector<const inventory::Program*> programs;
    QModelIndexList rows = table_->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const QModelIndex& row : rows) {
        if (const auto* program = model_->programAt(filter_->mapToSource(row).row())) {
            programs.push_back(program);
        }
    }
    return programs;
}

void MainWindow::uninstallSelected() {
    const auto programs = selectedPrograms();
    if (programs.empty() || !inventory_) {
        return;
    }
    // Window-modal dialogs opened with open(), not exec(): no nested event loop, and the main
    // window stays blocked until they close. The inventory copy keeps the programs alive even if
    // a refresh replaces it meanwhile.
    auto* confirmation = new UninstallDialog(programs, this);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    connect(confirmation, &QDialog::accepted, this, [this, confirmation, inventory = inventory_] {
        auto* progress =
            new UninstallProgressDialog(inventory, confirmation->items(), confirmation->options(),
                                        confirmation->scanLeftovers(), stores_, this);
        progress->setAttribute(Qt::WA_DeleteOnClose);
        connect(progress, &QDialog::finished, this, [this, progress] {
            if (progress->changedAnything()) {
                loader_->start();
            }
        });
        connect(progress, &UninstallProgressDialog::forceRequested, this,
                [this](const std::vector<std::wstring>& ids) { startForce({.programIds = ids}); });
        progress->open();
    });
    connect(confirmation, &UninstallDialog::forceRequested, this,
            [this](const std::vector<std::wstring>& ids) { startForce({.programIds = ids}); });
    confirmation->open();
}

void MainWindow::forceSelected() {
    ForceFlow::Request request;
    for (const auto* program : selectedPrograms()) {
        request.programIds.push_back(program->id);
    }
    if (!request.programIds.empty()) {
        startForce(std::move(request));
    }
}

void MainWindow::forceFolder() {
    auto* dialog = new QFileDialog(this, tr("Force uninstall a folder"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly);
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& folder) {
        startForce({.folder = QDir::toNativeSeparators(folder).toStdWString()});
    });
    dialog->open();
}

void MainWindow::startForce(ForceFlow::Request request) {
    if (forceFlow_ != nullptr) {
        return;
    }
    forceFlow_ = new ForceFlow(std::move(request), stores_, this);
    connect(forceFlow_, &ForceFlow::progress, this, [this](const QString& message) {
        message.isEmpty() ? statusBar()->clearMessage() : statusBar()->showMessage(message);
    });
    connect(forceFlow_, &ForceFlow::finished, this, [this](bool changedAnything) {
        forceFlow_ = nullptr;
        updateActions();
        if (changedAnything) {
            loader_->start();
        }
    });
    updateActions();
    forceFlow_->start();
}

void MainWindow::previewLeftovers() {
    const auto programs = selectedPrograms();
    if (programs.empty() || !inventory_ || busy_) {
        return;
    }
    std::vector<LeftoverModel::Program> names;
    for (const auto* program : programs) {
        names.push_back({program->id,
                         program->display_name.empty() ? tr("(no name)") : toQString(program->display_name)});
    }
    busy_ = true;
    updateActions();
    statusBar()->showMessage(tr("Looking for leftovers…"));
    // The inventory copy keeps the programs alive while the scan reads them.
    workers_.start([this, inventory = inventory_, programs, names = std::move(names)] {
        auto targets = leftovers::capture_targets(programs);
        const leftovers::LiveEnvironment environment;
        auto scan = std::make_shared<const leftovers::ScanResult>(
            leftovers::scan_leftovers(environment.environment(), targets, inventory->programs));
        QMetaObject::invokeMethod(
            this,
            [this, scan = std::move(scan), names, targets = std::move(targets)] {
                onPreviewFinished(scan, names, targets);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::onPreviewFinished(std::shared_ptr<const leftovers::ScanResult> scan,
                                   std::vector<LeftoverModel::Program> programs,
                                   std::vector<leftovers::ScanTarget> targets) {
    busy_ = false;
    updateActions();
    statusBar()->clearMessage();
    auto* preview = new LeftoverDialog(std::move(scan), std::move(programs), LeftoverDialog::Mode::Preview,
                                       QString(), this);
    preview->setAttribute(Qt::WA_DeleteOnClose);
    preview->setRescan([inventory = inventory_,
                        targets = std::move(targets)](const leftovers::ScanOptions& options) {
        const leftovers::LiveEnvironment environment;
        return leftovers::scan_leftovers(environment.environment(), targets, inventory->programs, options);
    });
    preview->open();
}

void MainWindow::findOrphans() {
    if (!inventory_ || busy_) {
        return;
    }
    busy_ = true;
    updateActions();
    statusBar()->showMessage(tr("Looking for orphaned folders…"));
    workers_.start([this, inventory = inventory_] {
        const leftovers::LiveEnvironment environment;
        auto result = leftovers::find_orphans(environment.environment(), inventory->programs);
        for (auto& leftover : result.leftovers) {
            leftover.owner_ids = {kOrphansId};
        }
        auto scan = std::make_shared<const leftovers::ScanResult>(std::move(result));
        QMetaObject::invokeMethod(
            this, [this, scan = std::move(scan)] { onOrphansFound(scan); }, Qt::QueuedConnection);
    });
}

void MainWindow::onOrphansFound(std::shared_ptr<const leftovers::ScanResult> scan) {
    busy_ = false;
    updateActions();
    statusBar()->clearMessage();
    const auto store = stores_->store();
    auto* review = new LeftoverDialog(std::move(scan), {{kOrphansId, tr("Orphaned folders")}},
                                      LeftoverDialog::Mode::Orphans,
                                      store ? QString() : stores_->unavailableReason(), this);
    review->setAttribute(Qt::WA_DeleteOnClose);
    connect(review, &QDialog::accepted, this, [this, review] {
        auto* removal =
            new ChangeSetProgressDialog(stores_->store(), review->plan(), review->allowIrreversible(), this);
        removal->setAttribute(Qt::WA_DeleteOnClose);
        removal->setWindowTitle(tr("Removing orphaned folders"));
        removal->open();
    });
    review->open();
}

void MainWindow::onWindowHovered(const HunterTarget::Window& window) {
    if (window.pid == 0) {
        statusBar()->showMessage(tr("Drag onto a program's window and let go."));
        return;
    }
    const QString process =
        window.processName.isEmpty() ? tr("process %1").arg(window.pid) : window.processName;
    statusBar()->showMessage(window.title.isEmpty() ? process : tr("%1 (%2)").arg(window.title, process));
}

void MainWindow::onWindowPicked(const HunterTarget::Window& window, QPoint position) {
    if (!inventory_ || busy_) {
        return;
    }
    busy_ = true;
    updateActions();
    statusBar()->showMessage(tr("Looking for the program of %1…")
                                 .arg(window.processName.isEmpty() ? window.title : window.processName));
    workers_.start([this, window, position, inventory = inventory_] {
        auto hunt = std::make_shared<Hunt>();
        hunt->window = window;
        hunt->position = position;
        hunt->inventory = inventory;
        for (const auto& process : platform::running_processes()) {
            if (process.pid == window.pid) {
                hunt->image = process.image;
                hunt->critical = process.critical;
            }
        }
        const auto services = platform::service_process_ids();
        hunt->service = std::find(services.begin(), services.end(), window.pid) != services.end();
        if (!hunt->image.empty()) {
            const leftovers::LiveEnvironment live;
            hunt->owner = force::owner_of(live.environment(), inventory->programs, hunt->image);
            if (!hunt->owner.program) {
                hunt->folderProtection = force::folder_protection(live.environment(), hunt->owner.folder);
            }
        }
        QMetaObject::invokeMethod(
            this, [this, hunt = std::shared_ptr<const Hunt>(std::move(hunt))] { onHuntFinished(hunt); },
            Qt::QueuedConnection);
    });
}

void MainWindow::onHuntFinished(const std::shared_ptr<const Hunt>& hunt) {
    busy_ = false;
    updateActions();
    statusBar()->clearMessage();
    const auto& window = hunt->window;
    const QString processName =
        window.processName.isEmpty() ? tr("process %1").arg(window.pid) : window.processName;

    QMenu menu(this);
    const auto* program = hunt->owner.program;
    QString heading;
    if (program != nullptr) {
        heading = program->display_name.empty() ? tr("(no name)") : toQString(program->display_name);
        // The menu's program actions work on the selection.
        if (!selectProgram(program->id)) {
            heading = tr("%1 (no longer listed; refresh with F5)").arg(heading);
        }
    } else if (hunt->image.empty()) {
        heading = tr("Windows doesn't tell Avel which program %1 is").arg(processName);
    } else {
        heading = tr("No installed program owns %1").arg(QDir::toNativeSeparators(toQString(hunt->image)));
    }
    menu.addAction(heading)->setEnabled(false);
    menu.addSeparator();

    QAction* forceFolder = nullptr;
    QAction* openFolder = nullptr;
    if (program != nullptr && selectedPrograms().size() == 1 &&
        selectedPrograms().front()->id == program->id) {
        addCopy(menu, uninstallAction_);
        addCopy(menu, forceAction_);
        addCopy(menu, previewLeftoversAction_);
        addCopy(menu, openLocationAction_);
    } else if (program == nullptr && !hunt->owner.folder.empty()) {
        const QString folder = QDir::toNativeSeparators(toQString(hunt->owner.folder));
        forceFolder = menu.addAction(tr("Force uninstall the folder %1…").arg(folder));
        const bool isProtected = hunt->folderProtection != leftovers::ProtectionReason::None;
        forceFolder->setEnabled(!isProtected && forceFlow_ == nullptr);
        if (isProtected) {
            forceFolder->setText(
                tr("Force uninstall the folder %1 (Windows or other programs need it)").arg(folder));
        }
        openFolder = menu.addAction(tr("Open folder"));
    }
    menu.addSeparator();
    QAction* end = menu.addAction(tr("End process %1").arg(processName));
    if (hunt->critical || hunt->service) {
        end->setEnabled(false);
        end->setText(hunt->critical ? tr("End process %1 (Windows needs it)").arg(processName)
                                    : tr("End process %1 (it runs a service)").arg(processName));
    }

    QAction* chosen = menu.exec(hunt->position);
    if (chosen == nullptr) {
        return;
    }
    if (chosen == forceFolder) {
        startForce(
            {.folder = hunt->owner.folder, .name = hunt->owner.name, .publisher = hunt->owner.publisher});
    } else if (chosen == openFolder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(hunt->owner.folder)));
    } else if (chosen == end) {
        endProcess(window.pid, processName);
    }
}

void MainWindow::endProcess(std::uint32_t pid, const QString& name) {
    auto* box = new QMessageBox(QMessageBox::Question, tr("End process"),
                                tr("End %1 (process %2)?").arg(name).arg(pid), QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(tr("Avel asks it to close first, then ends it. Unsaved work in it is lost."));
    QPushButton* end = box->addButton(tr("End process"), QMessageBox::DestructiveRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::buttonClicked, this, [this, end, pid, name](QAbstractButton* clicked) {
        if (clicked != end) {
            return;
        }
        statusBar()->showMessage(tr("Ending %1…").arg(name));
        workers_.start([this, pid, name] {
            const auto result = platform::stop_process(pid);
            QMetaObject::invokeMethod(
                this,
                [this, name, result] {
                    statusBar()->showMessage(
                        tr("%1: %2").arg(name, RunningProcessesDialog::outcomeText(result)), 10000);
                },
                Qt::QueuedConnection);
        });
    });
    box->open();
}

void MainWindow::openUndoCenter() {
    auto* center = new UndoCenterDialog(stores_->store(), stores_->unavailableReason(), this);
    center->setAttribute(Qt::WA_DeleteOnClose);
    connect(center, &QDialog::finished, this, [this, center] {
        // What it restored may include a program's Uninstall entry.
        if (center->changedAnything()) {
            loader_->start();
        }
    });
    center->open();
}

void MainWindow::onMaintenanceFinished(const QStringList& settled) {
    if (settled.isEmpty()) {
        return;
    }
    auto* box =
        new QMessageBox(QMessageBox::Information, tr("Interrupted removal"),
                        tr("%n removal(s) didn't finish last time: Avel or Windows stopped while they ran.",
                           nullptr, static_cast<int>(settled.size())),
                        QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(tr("Avel has checked what was removed. The Undo Center can put it back."));
    QPushButton* open = box->addButton(tr("Open Undo Center"), QMessageBox::AcceptRole);
    box->addButton(QMessageBox::Close);
    box->setDefaultButton(open);
    connect(box, &QMessageBox::buttonClicked, this, [this, open](QAbstractButton* clicked) {
        if (clicked == open) {
            openUndoCenter();
        }
    });
    box->open();
}

void MainWindow::openInstallLocation() {
    const auto programs = selectedPrograms();
    if (programs.size() == 1) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(programs.front()->install_location)));
    }
}

void MainWindow::showTableMenu(const QPoint& position) {
    if (!table_->indexAt(position).isValid()) {
        return;
    }
    QMenu menu(this);
    addCopy(menu, uninstallAction_);
    addCopy(menu, forceAction_);
    menu.addSeparator();
    addCopy(menu, openLocationAction_);
    addCopy(menu, previewLeftoversAction_);
    menu.exec(table_->viewport()->mapToGlobal(position));
}

// Widens the short columns when their content (or a longer translation) doesn't fit, but never
// narrows a column the user has sized. The name column stretches, so it absorbs the difference.
void MainWindow::ensureColumnsFit() {
    auto* header = table_->horizontalHeader();
    // QTableView makes sizeHintForColumn protected; it's public on the base class.
    const QAbstractItemView* view = table_;
    for (const int column : {ProgramTableModel::VersionColumn, ProgramTableModel::SizeColumn,
                             ProgramTableModel::DateColumn, ProgramTableModel::TypeColumn}) {
        const int needed = std::max(view->sizeHintForColumn(column), header->sectionSizeHint(column));
        if (header->sectionSize(column) < needed) {
            header->resizeSection(column, needed);
        }
    }
}

void MainWindow::updateCounts() {
    if (!inventory_) {
        countLabel_->clear();
        return;
    }
    const int shown = filter_->rowCount();
    const int total = filter_->categoryRowCount();
    countLabel_->setText(shown == total ? tr("%n program(s)", nullptr, total)
                                        : tr("Showing %1 of %n program(s)", nullptr, total).arg(shown));
}

void MainWindow::updateDiagnostics() {
    if (!inventory_ || inventory_->diagnostics.empty()) {
        warningLabel_->hide();
        return;
    }
    QStringList lines;
    for (const auto& diagnostic : inventory_->diagnostics) {
        lines << ProgramText::diagnosticText(diagnostic);
    }
    warningLabel_->setText(u"⚠ "_s + tr("%n warning(s)", nullptr, static_cast<int>(lines.size())));
    warningLabel_->setToolTip(lines.join(u'\n'));
    warningLabel_->show();
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About Avel"),
                       tr("<h3>Avel %1</h3>"
                          "<p>Professional software removal suite for Windows.</p>"
                          "<p>Avel (Ավել) is the Armenian word for “broom”.</p>"
                          "<p>Licensed under the Apache License 2.0. Uses Qt under the LGPLv3.</p>")
                           .arg(QApplication::applicationVersion()));
}

}  // namespace avel::app
