#include "leftover_dialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

#include "leftover_text.h"
#include "program_text.h"
#include "right_elide_delegate.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

constexpr auto kPresetKey = "leftovers/preset"_L1;
constexpr auto kShowUnlikelyKey = "leftovers/showUnlikely"_L1;

// Groups and folders with up to this many items open by themselves.
constexpr int kExpandLimit = 50;
// Up to this many programs are named in the title and in the Undo Center.
constexpr std::size_t kNamedPrograms = 3;

}  // namespace

LeftoverDialog::LeftoverDialog(std::shared_ptr<const leftovers::ScanResult> scan,
                               std::vector<LeftoverModel::Program> programs, Mode mode,
                               const QString& removalUnavailable, QWidget* parent)
    : QDialog(parent), scan_(std::move(scan)), programs_(std::move(programs)), mode_(mode) {
    QStringList names;
    for (const auto& program : programs_) {
        names << program.name;
    }
    const auto count = static_cast<int>(names.size());
    const QString subject = static_cast<std::size_t>(count) <= kNamedPrograms
                                ? names.join(u", "_s)
                                : tr("%n program(s)", nullptr, count);
    // The description is stored with the change set and shown in the Undo Center.
    QString intro;
    switch (mode_) {
        case Mode::Review:
            setWindowTitle(tr("Leftovers of %1").arg(subject));
            description_ = windowTitle();
            intro =
                tr("The uninstallers left these items behind. Checked items will be removed. Avel backs "
                   "up each one first, so Undo can put it back for 30 days.");
            break;
        case Mode::Preview:
            setWindowTitle(tr("Leftover preview: %1").arg(subject));
            intro =
                tr("What the scan finds for programs that are still installed: what would remain if "
                   "their uninstallers removed nothing. Nothing can be removed from here.");
            break;
        case Mode::Force:
            setWindowTitle(tr("Forced removal of %1").arg(subject));
            description_ = windowTitle();
            intro =
                tr("Avel doesn't run the program's uninstaller. It removes what belongs to the program "
                   "instead: its folder, its registry entries and what it left elsewhere. Checked items "
                   "will be removed. Avel backs up each one first, so Undo can put it back for 30 days.");
            break;
        case Mode::Orphans:
            setWindowTitle(tr("Orphaned folders"));
            description_ = windowTitle();
            intro =
                tr("No installed program owns these folders. Programs removed long ago may have left "
                   "them, but portable programs and data that programs keep on their own look the same, "
                   "so none is checked by itself. Avel backs up what you remove, so Undo can put it back "
                   "for 30 days.");
            break;
    }
    removalAvailable_ = mode_ != Mode::Preview && removalUnavailable.isEmpty();

    const QSettings settings;
    const int savedPreset =
        settings.value(kPresetKey, static_cast<int>(LeftoverModel::Preset::Moderate)).toInt();
    const auto preset = savedPreset >= 0 && savedPreset <= static_cast<int>(LeftoverModel::Preset::Advanced)
                            ? static_cast<LeftoverModel::Preset>(savedPreset)
                            : LeftoverModel::Preset::Moderate;

    auto* introLabel = new QLabel(intro, this);
    introLabel->setWordWrap(true);

    diagnosticsLabel_ = new QLabel(this);
    diagnosticsLabel_->setWordWrap(true);

    presetCombo_ = new QComboBox(this);
    presetCombo_->addItem(tr("Safe: very likely items only"), static_cast<int>(LeftoverModel::Preset::Safe));
    presetCombo_->addItem(tr("Moderate: likely items (recommended)"),
                          static_cast<int>(LeftoverModel::Preset::Moderate));
    presetCombo_->addItem(tr("Advanced: uncertain items too"),
                          static_cast<int>(LeftoverModel::Preset::Advanced));
    presetCombo_->setCurrentIndex(presetCombo_->findData(static_cast<int>(preset)));
    presetCombo_->setToolTip(tr("Which items to check. You can still check and uncheck any item yourself."));
    auto* presetLabel = new QLabel(tr("&Check:"), this);
    presetLabel->setBuddy(presetCombo_);

    unlikelyBox_ = new QCheckBox(this);
    unlikelyBox_->setChecked(settings.value(kShowUnlikelyKey, false).toBool());
    unlikelyBox_->setToolTip(
        tr("Matches with more evidence against than for them, such as folders other programs still use. They "
           "are never checked by themselves."));

    model_ = new LeftoverModel(this);
    model_->setShowUnlikely(unlikelyBox_->isChecked());
    model_->setScan(scan_, programs_, preset);

    tree_ = new QTreeView(this);
    tree_->setModel(model_);
    tree_->setUniformRowHeights(true);  // Thousands of rows stay fast
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setAllColumnsShowFocus(true);
    tree_->setTextElideMode(Qt::ElideMiddle);  // A path's end tells most
    tree_->setItemDelegateForColumn(LeftoverModel::ReasonColumn, new RightElideDelegate(tree_));
    auto* header = tree_->header();
    header->setStretchLastSection(true);
    header->resizeSection(LeftoverModel::ItemColumn, 460);
    // Wide enough for the longest label in the UI language, with its color swatch.
    int confidenceWidth = 0;
    for (const auto level : {leftovers::Confidence::VeryGood, leftovers::Confidence::Good,
                             leftovers::Confidence::Questionable, leftovers::Confidence::Bad}) {
        confidenceWidth = std::max(
            confidenceWidth, tree_->fontMetrics().horizontalAdvance(LeftoverText::confidenceLabel(level)));
    }
    header->resizeSection(LeftoverModel::ConfidenceColumn,
                          confidenceWidth + style()->pixelMetric(QStyle::PM_SmallIconSize) + 32);

    auto* copyAction = new QAction(tr("&Copy"), tree_);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setShortcutContext(Qt::WidgetShortcut);
    connect(copyAction, &QAction::triggered, this, &LeftoverDialog::copySelection);
    tree_->addAction(copyAction);
    tree_->setContextMenuPolicy(Qt::ActionsContextMenu);

    countLabel_ = new QLabel(this);
    countLabel_->setWordWrap(true);
    auto* unavailable = new QLabel(removalUnavailable, this);
    unavailable->setWordWrap(true);
    unavailable->setStyleSheet(u"color: #C04030"_s);
    unavailable->setVisible(mode_ != Mode::Preview && !removalUnavailable.isEmpty());

    auto* buttons = new QDialogButtonBox(this);
    deepButton_ = buttons->addButton(tr("Search the whole disk"), QDialogButtonBox::ActionRole);
    deepButton_->setToolTip(
        tr("Also look for folders named after the program anywhere on the fixed drives. Takes a few seconds. "
           "What only this search finds is never checked by itself."));
    deepButton_->hide();
    connect(deepButton_, &QPushButton::clicked, this, &LeftoverDialog::startDeepSearch);
    if (mode_ != Mode::Preview) {
        removeButton_ = buttons->addButton(tr("Remove"), QDialogButtonBox::AcceptRole);
        removeButton_->setDefault(true);
        closeButton_ = buttons->addButton(tr("Keep all"), QDialogButtonBox::RejectRole);
    } else {
        closeButton_ = buttons->addButton(QDialogButtonBox::Close);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* options = new QHBoxLayout;
    options->addWidget(presetLabel);
    options->addWidget(presetCombo_);
    options->addStretch();
    options->addWidget(unlikelyBox_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(introLabel);
    layout->addWidget(diagnosticsLabel_);
    layout->addLayout(options);
    layout->addWidget(tree_, 1);
    layout->addWidget(countLabel_);
    layout->addWidget(unavailable);
    layout->addWidget(buttons);

    connect(presetCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int value = presetCombo_->itemData(index).toInt();
        model_->applyPreset(static_cast<LeftoverModel::Preset>(value));
        QSettings().setValue(kPresetKey, value);
    });
    connect(unlikelyBox_, &QCheckBox::toggled, this, [this](bool show) {
        model_->setShowUnlikely(show);
        expandDefault();
        updateCounts();
        QSettings().setValue(kShowUnlikelyKey, show);
    });
    connect(model_, &LeftoverModel::checkedCountChanged, this, &LeftoverDialog::updateCounts);

    updateScanInfo();
    expandDefault();
    tree_->resizeColumnToContents(LeftoverModel::TypeColumn);
    header->resizeSection(LeftoverModel::TypeColumn,
                          std::max(header->sectionSize(LeftoverModel::TypeColumn) + 16, 110));
    updateCounts();
    resize(1100, 640);
}

LeftoverDialog::~LeftoverDialog() {
    if (rescanWorker_.joinable()) {
        // Only reachable if the application quits mid-search.
        rescanWorker_.join();
    }
}

void LeftoverDialog::setRescan(Rescan rescan) {
    rescan_ = std::move(rescan);
    deepButton_->setVisible(static_cast<bool>(rescan_));
}

void LeftoverDialog::accept() {
    if (mode_ == Mode::Preview) {
        if (!searching_) {
            QDialog::accept();
        }
        return;
    }
    if (searching_ || !removalAvailable_ || model_->checkedCount() == 0) {
        return;
    }
    const int packages = model_->checkedPackageCount();
    if (packages == 0) {
        allowIrreversible_ = false;
        finishAccept();
        return;
    }
    // Asked with open(), not exec(): UI Automation clients stay responsive.
    auto* box = new QMessageBox(
        QMessageBox::Warning, tr("Packages can't be restored"),
        tr("%n checked package(s) can't be put back after removal: Windows deletes a package for good.",
           nullptr, packages),
        QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(tr("Everything else is backed up and can be restored."));
    QPushButton* removeAll = box->addButton(tr("Remove them too"), QMessageBox::DestructiveRole);
    QPushButton* keep = box->addButton(tr("Keep packages"), QMessageBox::AcceptRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(keep);
    connect(box, &QMessageBox::buttonClicked, this, [this, removeAll, keep](QAbstractButton* clicked) {
        if (clicked == keep) {
            model_->uncheckPackages();
            allowIrreversible_ = false;
            if (model_->checkedCount() > 0) {
                finishAccept();
            }
        } else if (clicked == removeAll) {
            allowIrreversible_ = true;
            finishAccept();
        }
    });
    box->open();
}

void LeftoverDialog::reject() {
    // The search can't be stopped half way; it takes seconds.
    if (!searching_) {
        QDialog::reject();
    }
}

void LeftoverDialog::closeEvent(QCloseEvent* event) {
    if (searching_) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void LeftoverDialog::finishAccept() {
    plan_ = model_->plan(description_.toStdWString());
    QDialog::accept();
}

// What depends on the scan, apart from the tree: its warnings and the unlikely matches' count.
void LeftoverDialog::updateScanInfo() {
    QStringList notes;
    for (const auto& diagnostic : scan_->diagnostics) {
        notes << LeftoverText::diagnosticText(diagnostic);
    }
    diagnosticsLabel_->setText(u"⚠ "_s + notes.join(u"\n⚠ "_s));
    diagnosticsLabel_->setVisible(!notes.isEmpty());

    const auto unlikely =
        std::count_if(scan_->leftovers.begin(), scan_->leftovers.end(), [](const auto& leftover) {
            return leftovers::level(leftover.records) <= leftovers::Confidence::Bad;
        });
    unlikelyBox_->setText(tr("Show unlikely matches (%1)").arg(QLocale().toString(unlikely)));
    unlikelyBox_->setEnabled(unlikely > 0);
}

void LeftoverDialog::updateCounts() {
    const int items = model_->itemCount();
    const int checked = model_->checkedCount();
    QString text =
        items == 0 ? tr("No leftovers found.") : tr("%1 of %n item(s) checked.", nullptr, items).arg(checked);
    if (scan_->protected_count > 0) {
        text += u' ' + tr("%n match(es) in protected locations left out.", nullptr,
                          static_cast<int>(scan_->protected_count));
    }
    countLabel_->setText(text);
    if (removeButton_ != nullptr) {
        removeButton_->setText(tr("Remove %n item(s)", nullptr, checked));
        removeButton_->setEnabled(removalAvailable_ && checked > 0 && !searching_);
    }
}

void LeftoverDialog::startDeepSearch() {
    if (!rescan_ || searching_) {
        return;
    }
    searching_ = true;
    deepButton_->setEnabled(false);
    deepButton_->setText(tr("Searching the whole disk…"));
    closeButton_->setEnabled(false);
    updateCounts();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    rescanWorker_ = std::thread([this, rescan = rescan_] {
        auto scan = std::make_shared<const leftovers::ScanResult>(rescan({.deep = true}));
        QMetaObject::invokeMethod(
            this, [this, scan = std::move(scan)] { onDeepSearchFinished(scan); }, Qt::QueuedConnection);
    });
}

void LeftoverDialog::onDeepSearchFinished(std::shared_ptr<const leftovers::ScanResult> scan) {
    if (rescanWorker_.joinable()) {
        rescanWorker_.join();
    }
    QApplication::restoreOverrideCursor();
    searching_ = false;
    deepButton_->setText(tr("Whole disk searched"));
    closeButton_->setEnabled(true);
    // The new scan has the old one's items and more; the checks start over from the preset.
    scan_ = std::move(scan);
    model_->setScan(scan_, programs_,
                    static_cast<LeftoverModel::Preset>(presetCombo_->currentData().toInt()));
    updateScanInfo();
    expandDefault();
    updateCounts();
}

void LeftoverDialog::expandDefault() {
    tree_->collapseAll();
    for (int row = 0; row < model_->rowCount(); ++row) {
        const QModelIndex program = model_->index(row, 0);
        tree_->expand(program);
        expandSmall(program);
    }
}

void LeftoverDialog::expandSmall(const QModelIndex& parent) {
    for (int row = 0; row < model_->rowCount(parent); ++row) {
        const QModelIndex child = model_->index(row, 0, parent);
        if (model_->rowCount(child) > 0 && child.data(LeftoverModel::ItemCountRole).toInt() <= kExpandLimit) {
            tree_->expand(child);
            expandSmall(child);
        }
    }
}

void LeftoverDialog::copySelection() {
    QStringList lines;
    for (const QModelIndex& index : tree_->selectionModel()->selectedRows(LeftoverModel::ItemColumn)) {
        const QVariant leftover = index.data(LeftoverModel::LeftoverRole);
        lines << (leftover.isValid()
                      ? LeftoverText::locationText(scan_->leftovers[leftover.toULongLong()].location)
                      : index.data().toString());
    }
    if (!lines.isEmpty()) {
        QApplication::clipboard()->setText(lines.join(u'\n'));
    }
}

}  // namespace avel::app
