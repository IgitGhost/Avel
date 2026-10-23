#include "running_processes_dialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using platform::StopOutcome;

namespace {

enum Column { NameColumn, PidColumn, StatusColumn, PathColumn };

bool gone(const platform::StopResult& result) {
    return result.outcome == StopOutcome::Closed || result.outcome == StopOutcome::Terminated ||
           result.outcome == StopOutcome::Gone;
}

}  // namespace

RunningProcessesDialog::RunningProcessesDialog(std::vector<platform::ProcessInfo> processes,
                                               const QString& subject, QWidget* parent)
    : QDialog(parent), processes_(std::move(processes)) {
    setWindowTitle(tr("Running programs"));
    const auto count = static_cast<int>(processes_.size());

    auto* intro =
        new QLabel(tr("%n program(s) run from the folders of %1. Avel ends them before it removes anything; "
                      "unsaved work in them is lost.",
                      nullptr, count)
                       .arg(subject),
                   this);
    intro->setWordWrap(true);

    list_ = new QTreeWidget(this);
    list_->setColumnCount(4);
    list_->setHeaderLabels({tr("Program"), tr("Process ID"), tr("Status"), tr("Path")});
    list_->setRootIsDecorated(false);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setTextElideMode(Qt::ElideMiddle);
    list_->header()->setStretchLastSection(true);
    for (const auto& process : processes_) {
        auto* row = new QTreeWidgetItem(list_);
        row->setText(NameColumn, toQString(process.name));
        row->setText(PidColumn, QString::number(process.pid));
        row->setTextAlignment(PidColumn, Qt::AlignRight | Qt::AlignVCenter);
        row->setText(StatusColumn, tr("Running"));
        row->setText(PathColumn, toQString(process.image));
        row->setToolTip(PathColumn, row->text(PathColumn));
    }
    list_->resizeColumnToContents(NameColumn);
    list_->setColumnWidth(NameColumn, std::min(list_->columnWidth(NameColumn) + 16, 220));
    list_->resizeColumnToContents(PidColumn);
    list_->setColumnWidth(StatusColumn, 160);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    statusLabel_->hide();

    auto* buttons = new QDialogButtonBox(this);
    endButton_ = buttons->addButton(tr("End processes"), QDialogButtonBox::AcceptRole);
    endButton_->setDefault(true);
    cancelButton_ = buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(list_, 1);
    layout->addWidget(statusLabel_);
    layout->addWidget(buttons);
    resize(760, std::min(240 + count * 24, 520));
}

RunningProcessesDialog::~RunningProcessesDialog() {
    if (worker_.joinable()) {
        // Only reachable if the application quits while processes are being ended.
        worker_.join();
    }
}

void RunningProcessesDialog::accept() {
    if (running_) {
        return;
    }
    if (ended_) {
        QDialog::accept();  // "Continue" without the processes that wouldn't end
        return;
    }
    endProcesses();
}

void RunningProcessesDialog::reject() {
    if (!running_) {
        QDialog::reject();
    }
}

void RunningProcessesDialog::closeEvent(QCloseEvent* event) {
    if (running_) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

QString RunningProcessesDialog::outcomeText(const platform::StopResult& result) {
    switch (result.outcome) {
        case StopOutcome::Closed:
            return tr("Closed");
        case StopOutcome::Terminated:
            return tr("Ended");
        case StopOutcome::Gone:
            return tr("Had already exited");
        case StopOutcome::AccessDenied:
            return tr("Windows denied access");
        case StopOutcome::Failed:
            return result.error != 0 ? tr("Still running: %1").arg(ProgramText::systemErrorText(result.error))
                                     : tr("Still running");
    }
    return {};
}

void RunningProcessesDialog::endProcesses() {
    running_ = true;
    endButton_->setEnabled(false);
    cancelButton_->setEnabled(false);
    statusLabel_->setText(tr("Asking the programs to close…"));
    statusLabel_->show();
    QApplication::setOverrideCursor(Qt::BusyCursor);
    results_.assign(processes_.size(), {});
    // The worker reads the list, which stays as it is while it runs.
    worker_ = std::thread([this] {
        for (std::size_t i = 0; i < processes_.size(); ++i) {
            const auto result = platform::stop_process(processes_[i].pid);
            QMetaObject::invokeMethod(this, [=, this] { onProcessStopped(i, result); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] { onAllStopped(); }, Qt::QueuedConnection);
    });
}

void RunningProcessesDialog::onProcessStopped(std::size_t index, platform::StopResult result) {
    results_[index] = result;
    auto* row = list_->topLevelItem(static_cast<int>(index));
    row->setText(StatusColumn, outcomeText(result));
    row->setToolTip(StatusColumn, row->text(StatusColumn));
    row->setForeground(StatusColumn, gone(result) ? QColor(0x2E, 0x8B, 0x57) : QColor(0xC0, 0x40, 0x30));
}

void RunningProcessesDialog::onAllStopped() {
    if (worker_.joinable()) {
        worker_.join();
    }
    QApplication::restoreOverrideCursor();
    running_ = false;
    ended_ = true;
    const auto left = static_cast<int>(
        std::count_if(results_.begin(), results_.end(), [](const auto& result) { return !gone(result); }));
    if (left == 0) {
        QDialog::accept();
        return;
    }
    statusLabel_->setText(
        tr("%n process(es) couldn't be ended. Files they use stay in place; the rest can still be removed.",
           nullptr, left));
    statusLabel_->setStyleSheet(u"color: #C04030"_s);
    endButton_->setText(tr("Continue"));
    endButton_->setEnabled(true);
    cancelButton_->setEnabled(true);
}

}  // namespace avel::app
