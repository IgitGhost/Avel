#include "main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
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

#include "change_store_access.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/post_uninstall.h"
#include "core/platform/elevation.h"
#include "core/uninstall/registration.h"
#include "details_view.h"
#include "icon_loader.h"
#include "inventory_loader.h"
#include "language_manager.h"
#include "leftover_dialog.h"
#include "program_filter_model.h"
#include "program_table_model.h"
#include "program_text.h"
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

}  // namespace

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
    previewPool_.setMaxThreadCount(1);

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

    openLocationAction_ = new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon), QString(), this);
    openLocationAction_->setEnabled(false);
    connect(openLocationAction_, &QAction::triggered, this, &MainWindow::openInstallLocation);

    previewLeftoversAction_ = new QAction(this);
    previewLeftoversAction_->setEnabled(false);
    connect(previewLeftoversAction_, &QAction::triggered, this, &MainWindow::previewLeftovers);

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
    toolBar_->addSeparator();

    searchEdit_ = new QLineEdit(toolBar_);
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(260);
    connect(searchEdit_, &QLineEdit::textChanged, filter_, &ProgramFilterModel::setSearchText);
    toolBar_->addWidget(searchEdit_);

    categoryCombo_ = new QComboBox(toolBar_);
    categoryCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (int i = 0; i <= static_cast<int>(ProgramFilterModel::Category::Everything); ++i) {
        categoryCombo_->addItem(QString(), i);
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
    openLocationAction_->setText(tr("&Open install folder"));
    previewLeftoversAction_->setText(tr("&Preview leftovers…"));
    previewLeftoversAction_->setToolTip(
        tr("Show what the leftover scan finds for the selected programs, without removing anything"));
    exitAction_->setText(tr("E&xit"));
    toolsMenu_->setTitle(tr("&Tools"));
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
        }
        return QString();
    };
    for (int i = 0; i < categoryCombo_->count(); ++i) {
        categoryCombo_->setItemText(
            i, category_name(static_cast<Category>(categoryCombo_->itemData(i).toInt())));
    }
    categoryCombo_->setToolTip(tr("Which programs to show. \"Apps\" matches Settings > Apps."));

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
    const int category = settings.value(kCategoryKey, 0).toInt();
    if (category >= 0 && category < categoryCombo_->count()) {
        categoryCombo_->setCurrentIndex(category);
    }
}

void MainWindow::saveLayout() const {
    QSettings settings;
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kStateKey, saveState());
    settings.setValue(kSplitterKey, splitter_->saveState());
    settings.setValue(kHeaderKey, table_->horizontalHeader()->saveState());
    settings.setValue(kCategoryKey, categoryCombo_->currentIndex());
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
    openLocationAction_->setEnabled(programs.size() == 1 &&
                                    uninstall::install_location_exists(*programs.front()));
    previewLeftoversAction_->setEnabled(!programs.empty() && !previewRunning_);
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
        progress->open();
    });
    confirmation->open();
}

void MainWindow::previewLeftovers() {
    const auto programs = selectedPrograms();
    if (programs.empty() || !inventory_ || previewRunning_) {
        return;
    }
    std::vector<LeftoverModel::Program> names;
    for (const auto* program : programs) {
        names.push_back({program->id,
                         program->display_name.empty() ? tr("(no name)") : toQString(program->display_name)});
    }
    previewRunning_ = true;
    updateActions();
    statusBar()->showMessage(tr("Looking for leftovers…"));
    // The inventory copy keeps the programs alive while the scan reads them.
    previewPool_.start([this, inventory = inventory_, programs, names = std::move(names)] {
        const auto targets = leftovers::capture_targets(programs);
        const leftovers::LiveEnvironment environment;
        auto scan = std::make_shared<const leftovers::ScanResult>(
            leftovers::scan_leftovers(environment.environment(), targets, inventory->programs));
        QMetaObject::invokeMethod(
            this, [this, scan = std::move(scan), names] { onPreviewFinished(scan, names); },
            Qt::QueuedConnection);
    });
}

void MainWindow::onPreviewFinished(std::shared_ptr<const leftovers::ScanResult> scan,
                                   std::vector<LeftoverModel::Program> programs) {
    previewRunning_ = false;
    updateActions();
    statusBar()->clearMessage();
    auto* preview = new LeftoverDialog(std::move(scan), std::move(programs), LeftoverDialog::Mode::Preview,
                                       QString(), this);
    preview->setAttribute(Qt::WA_DeleteOnClose);
    preview->open();
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
    menu.addAction(uninstallAction_);
    menu.addAction(openLocationAction_);
    menu.addAction(previewLeftoversAction_);
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
