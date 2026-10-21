#include "leftover_dialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
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
    : QDialog(parent), scan_(std::move(scan)), mode_(mode) {
    QStringList names;
    for (const auto& program : programs) {
        names << program.name;
    }
    const auto count = static_cast<int>(names.size());
    const QString subject = static_cast<std::size_t>(count) <= kNamedPrograms
                                ? names.join(u", "_s)
                                : tr("%n program(s)", nullptr, count);
    if (mode_ == Mode::Review) {
        setWindowTitle(tr("Leftovers of %1").arg(subject));
        // Stored with the change set and shown in the Undo Center.
        description_ = tr("Leftovers of %1").arg(subject);
    } else {
        setWindowTitle(tr("Leftover preview: %1").arg(subject));
    }
    removalAvailable_ = mode_ == Mode::Review && removalUnavailable.isEmpty();

    const QSettings settings;
    const int savedPreset =
        settings.value(kPresetKey, static_cast<int>(LeftoverModel::Preset::Moderate)).toInt();
    const auto preset = savedPreset >= 0 && savedPreset <= static_cast<int>(LeftoverModel::Preset::Advanced)
                            ? static_cast<LeftoverModel::Preset>(savedPreset)
                            : LeftoverModel::Preset::Moderate;

    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(
        mode_ == Mode::Review
            ? tr("The uninstallers left these items behind. Checked items will be removed. Avel backs "
                 "up each one first, so Undo can put it back for 30 days.")
            : tr("What the scan finds for programs that are still installed: what would remain if "
                 "their uninstallers removed nothing. Nothing can be removed from here."));

    auto* diagnostics = new QLabel(this);
    diagnostics->setWordWrap(true);
    QStringList notes;
    for (const auto& diagnostic : scan_->diagnostics) {
        notes << LeftoverText::diagnosticText(diagnostic);
    }
    diagnostics->setText(u"⚠ "_s + notes.join(u"\n⚠ "_s));
    diagnostics->setVisible(!notes.isEmpty());

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

    const auto unlikely =
        std::count_if(scan_->leftovers.begin(), scan_->leftovers.end(), [](const auto& leftover) {
            return leftovers::level(leftover.records) <= leftovers::Confidence::Bad;
        });
    unlikelyBox_ = new QCheckBox(tr("Show unlikely matches (%1)").arg(QLocale().toString(unlikely)), this);
    unlikelyBox_->setChecked(settings.value(kShowUnlikelyKey, false).toBool());
    unlikelyBox_->setEnabled(unlikely > 0);
    unlikelyBox_->setToolTip(
        tr("Matches with more evidence against than for them, such as folders other programs still use. They "
           "are never checked by themselves."));

    model_ = new LeftoverModel(this);
    model_->setShowUnlikely(unlikelyBox_->isChecked());
    model_->setScan(scan_, std::move(programs), preset);

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
    unavailable->setVisible(mode_ == Mode::Review && !removalUnavailable.isEmpty());

    auto* buttons = new QDialogButtonBox(this);
    if (mode_ == Mode::Review) {
        removeButton_ = buttons->addButton(tr("Remove"), QDialogButtonBox::AcceptRole);
        removeButton_->setDefault(true);
        buttons->addButton(tr("Keep all"), QDialogButtonBox::RejectRole);
    } else {
        buttons->addButton(QDialogButtonBox::Close);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* options = new QHBoxLayout;
    options->addWidget(presetLabel);
    options->addWidget(presetCombo_);
    options->addStretch();
    options->addWidget(unlikelyBox_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(diagnostics);
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

    expandDefault();
    tree_->resizeColumnToContents(LeftoverModel::TypeColumn);
    header->resizeSection(LeftoverModel::TypeColumn,
                          std::max(header->sectionSize(LeftoverModel::TypeColumn) + 16, 110));
    updateCounts();
    resize(1100, 640);
}

void LeftoverDialog::accept() {
    if (mode_ == Mode::Preview) {
        QDialog::accept();
        return;
    }
    if (!removalAvailable_ || model_->checkedCount() == 0) {
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

void LeftoverDialog::finishAccept() {
    plan_ = model_->plan(description_.toStdWString());
    QDialog::accept();
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
        removeButton_->setEnabled(removalAvailable_ && checked > 0);
    }
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
