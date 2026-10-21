#include "uninstall_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "core/platform/elevation.h"
#include "core/platform/user_profiles.h"
#include "core/uninstall/installer_detection.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

constexpr auto kRestorePointKey = "uninstall/restorePoint"_L1;
constexpr auto kLeftoverScanKey = "uninstall/leftoverScan"_L1;

}  // namespace

UninstallDialog::UninstallDialog(std::vector<const inventory::Program*> programs, QWidget* parent)
    : QDialog(parent), programs_(std::move(programs)) {
    context_.current_user_sid = platform::current_user_sid();
    context_.elevated = platform::is_process_elevated();
    context_.detect_installer = uninstall::detect_installer_type;

    const auto count = static_cast<int>(programs_.size());
    setWindowTitle(count == 1 ? tr("Uninstall %1").arg(toQString(programs_.front()->display_name))
                              : tr("Uninstall %n program(s)", nullptr, count));

    intro_ = new QLabel(tr("Avel runs each program's own uninstaller, one after another:"), this);
    intro_->setWordWrap(true);

    list_ = new QTreeWidget(this);
    list_->setColumnCount(2);
    list_->setHeaderLabels({tr("Program"), tr("How it's removed")});
    list_->setRootIsDecorated(false);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->header()->setStretchLastSection(true);
    for (const auto* program : programs_) {
        auto* item = new QTreeWidgetItem(list_);
        item->setText(0, program->display_name.empty() ? tr("(no name)") : toQString(program->display_name));
    }

    // Silent mode by default for batches: clicking through several uninstallers is tedious.
    quietBox_ = new QCheckBox(tr("Uninstall silently where possible"), this);
    quietBox_->setChecked(count > 1);
    quietBox_->setToolTip(
        tr("Uses the uninstaller's silent mode, so no questions are asked. Programs without a "
           "known silent mode still show their uninstaller."));
    restorePointBox_ = new QCheckBox(tr("Create a System Restore point first"), this);
    restorePointBox_->setChecked(QSettings().value(kRestorePointKey, true).toBool());
    restorePointBox_->setToolTip(
        tr("Lets you roll Windows back if an uninstaller breaks something. Takes up to a "
           "minute, and Windows creates at most one such point a day."));
    leftoverScanBox_ = new QCheckBox(tr("Look for leftovers afterwards"), this);
    leftoverScanBox_->setChecked(QSettings().value(kLeftoverScanKey, true).toBool());
    leftoverScanBox_->setToolTip(
        tr("Once the uninstallers are done, Avel looks for the files, registry entries, services and tasks "
           "they left behind, and lets you choose what to remove."));
    simulateBox_ = new QCheckBox(tr("Simulate only: show what would happen, change nothing"), this);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro_);
    layout->addWidget(list_, 1);
    layout->addWidget(quietBox_);
    layout->addWidget(restorePointBox_);
    layout->addWidget(leftoverScanBox_);
    layout->addWidget(simulateBox_);
    layout->addWidget(buttons);

    connect(quietBox_, &QCheckBox::toggled, this, &UninstallDialog::updatePlans);
    connect(simulateBox_, &QCheckBox::toggled, this, &UninstallDialog::updatePlans);
    updatePlans();

    list_->resizeColumnToContents(0);
    list_->setColumnWidth(0, std::min(list_->columnWidth(0) + 16, 320));
    resize(760, std::min(280 + count * 22, 620));
}

uninstall::BatchOptions UninstallDialog::options() const {
    uninstall::BatchOptions options;
    options.dry_run = simulateBox_->isChecked();
    options.restore_point = restorePointBox_->isChecked();
    const auto count = static_cast<int>(programs_.size());
    // Shown in System Restore's list of restore points.
    options.restore_point_description =
        (count == 1 ? tr("Avel: uninstall %1").arg(toQString(programs_.front()->display_name))
                    : tr("Avel: uninstall %n program(s)", nullptr, count))
            .toStdWString();
    return options;
}

bool UninstallDialog::scanLeftovers() const {
    return leftoverScanBox_->isChecked() && !simulateBox_->isChecked();
}

void UninstallDialog::accept() {
    QSettings().setValue(kRestorePointKey, restorePointBox_->isChecked());
    QSettings().setValue(kLeftoverScanKey, leftoverScanBox_->isChecked());
    QDialog::accept();
}

void UninstallDialog::updatePlans() {
    const auto mode = quietBox_->isChecked() ? uninstall::Mode::Quiet : uninstall::Mode::Interactive;
    items_.clear();
    int possible = 0;
    for (int i = 0; i < static_cast<int>(programs_.size()); ++i) {
        const auto* program = programs_[static_cast<size_t>(i)];
        uninstall::BatchItem item{program, uninstall::make_uninstall_plan(*program, mode, context_)};
        QTreeWidgetItem* row = list_->topLevelItem(i);
        row->setText(1, ProgramText::planText(item.plan, mode));
        const QString command = toQString(item.plan.command_line());
        row->setToolTip(1, command.isEmpty() ? toQString(item.plan.package_full_name) : command);
        row->setForeground(1, item.plan.possible() ? palette().text() : QBrush(QColor(0xC0, 0x40, 0x30)));
        possible += item.plan.possible() ? 1 : 0;
        items_.push_back(std::move(item));
    }
    okButton_->setText(simulateBox_->isChecked() ? tr("Simulate") : tr("Uninstall"));
    // A simulated run removes nothing, so there would be nothing to find.
    leftoverScanBox_->setEnabled(!simulateBox_->isChecked());
    okButton_->setEnabled(possible > 0);
}

}  // namespace avel::app
