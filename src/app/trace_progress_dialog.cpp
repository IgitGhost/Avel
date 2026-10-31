#include "trace_progress_dialog.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "core/inventory/inventory.h"
#include "core/tracker/linking.h"
#include "program_text.h"
#include "trace_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

using tracker::InstallTracePhase;
using tracker::TraceError;

bool isFinished(tracker::TraceState state) {
    return state == tracker::TraceState::Complete || state == tracker::TraceState::Incomplete;
}

enum ProcessColumn { ProcessNameColumn, ProcessIdColumn, ProcessRoleColumn };

}  // namespace

// What install_and_trace reported, copied for the GUI thread.
struct TraceProgressDialog::Status {
    InstallTracePhase phase = InstallTracePhase::Starting;
    tracker::SessionStep step = tracker::SessionStep::Volumes;
    tracker::WatchPhase watch = tracker::WatchPhase::Running;
    std::vector<tracker::WatchProcess> running;
};

struct TraceProgressDialog::Outcome {
    tracker::InstallTraceResult result;  // `trace` linked, once it's finished
    TraceError linkError = TraceError::None;
};

TraceProgressDialog::TraceProgressDialog(std::shared_ptr<tracker::TraceStore> store, Request request,
                                         QWidget* parent)
    : QDialog(parent), store_(std::move(store)), request_(std::move(request)) {
    setObjectName(u"traceProgressDialog"_s);
    const QString installer = QFileInfo(toQString(request_.installer)).fileName();
    setWindowTitle(tr("Install and trace: %1").arg(installer));

    auto* intro =
        new QLabel(tr("Avel records what the installation changes. Go through the installer's windows as "
                      "usual; Avel stops the trace by itself once the installation is over."),
                   this);
    intro->setWordWrap(true);

    const std::array<std::pair<const char*, QString>, PhaseCount> phases{{
        {"phaseSnapshot", tr("Taking a snapshot of the registry")},
        {"phaseInstaller", tr("Waiting for the installer")},
        {"phaseWindowsInstaller", tr("Waiting for Windows Installer")},
        {"phaseSettling", tr("Waiting for the disk to settle")},
        {"phaseFinishing", tr("Finishing the trace")},
    }};
    auto* phaseLayout = new QVBoxLayout;
    phaseLayout->setSpacing(2);
    for (std::size_t i = 0; i < phases.size(); ++i) {
        auto* label = new QLabel(phases[i].second, this);
        label->setObjectName(QString::fromLatin1(phases[i].first));
        phaseLabels_[i] = label;
        phaseLayout->addWidget(label);
    }

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(u"traceStatus"_s);
    statusLabel_->setWordWrap(true);

    processesLabel_ = new QLabel(tr("The installer's processes:"), this);
    processes_ = new QTreeWidget(this);
    processes_->setObjectName(u"installerProcesses"_s);
    processes_->setAccessibleName(tr("The installer's processes"));
    processes_->setColumnCount(3);
    processes_->setHeaderLabels({tr("Program"), tr("Process ID"), tr("Role")});
    processes_->setRootIsDecorated(false);
    processes_->setUniformRowHeights(true);
    processes_->setSelectionMode(QAbstractItemView::NoSelection);
    processes_->setColumnWidth(ProcessNameColumn, 380);
    processes_->setColumnWidth(ProcessIdColumn, 90);
    processes_->header()->setStretchLastSection(true);

    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(u"traceResult"_s);
    resultLabel_->setWordWrap(true);
    resultLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultLabel_->hide();

    auto* buttons = new QDialogButtonBox(this);
    stopButton_ = buttons->addButton(tr("Stop…"), QDialogButtonBox::ActionRole);
    stopButton_->setObjectName(u"stopTraceButton"_s);
    stopButton_->setToolTip(tr("Stop the trace now"));
    reviewButton_ = buttons->addButton(tr("Review…"), QDialogButtonBox::ActionRole);
    reviewButton_->setObjectName(u"reviewTraceButton"_s);
    reviewButton_->hide();
    closeButton_ = buttons->addButton(QDialogButtonBox::Close);
    closeButton_->setObjectName(u"closeTraceProgressButton"_s);
    closeButton_->setEnabled(false);
    connect(stopButton_, &QPushButton::clicked, this, &TraceProgressDialog::requestStop);
    connect(reviewButton_, &QPushButton::clicked, this, [this] {
        if (!traceId_.isEmpty()) {
            Q_EMIT reviewRequested(traceId_);
        }
    });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(phaseLayout);
    layout->addWidget(statusLabel_);
    layout->addWidget(processesLabel_);
    layout->addWidget(processes_, 1);
    layout->addWidget(resultLabel_);
    layout->addWidget(buttons);
    resize(720, 520);
    setPhase(SnapshotPhase);
    statusLabel_->setText(TraceText::stepText(tracker::SessionStep::Volumes));
}

TraceProgressDialog::~TraceProgressDialog() {
    if (worker_.joinable()) {
        // Only reachable if the application quits meanwhile: the trace stops, the installer stays.
        cancellation_.stop_batch = true;
        worker_.join();
    }
}

void TraceProgressDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!started_) {
        started_ = true;
        start();
    }
}

void TraceProgressDialog::closeEvent(QCloseEvent* event) {
    if (running_) {
        event->ignore();
        requestStop();
        return;
    }
    QDialog::closeEvent(event);
}

void TraceProgressDialog::reject() {
    // Esc while it runs asks to stop instead of closing.
    if (running_) {
        requestStop();
        return;
    }
    QDialog::reject();
}

void TraceProgressDialog::start() {
    running_ = true;
    // The worker only touches its copies and the cancellation flags; everything visible happens on
    // the GUI thread.
    worker_ = std::thread([this, store = store_, request = request_] {
        tracker::InstallTraceOptions options;
        options.installer = request.installer;
        options.arguments = request.arguments;
        options.description = request.description;
        auto outcome = std::make_shared<Outcome>();
        outcome->result = tracker::install_and_trace(
            *store, options, cancellation_, [this](const tracker::InstallTraceStatus& status) {
                Status copy{
                    status.phase, status.step, status.watch, {status.running.begin(), status.running.end()}};
                QMetaObject::invokeMethod(
                    this, [this, copy = std::move(copy)] { onStatus(copy); }, Qt::QueuedConnection);
            });
        if (const auto& trace = outcome->result.trace; trace && isFinished(trace->state)) {
            QMetaObject::invokeMethod(this, [this] { onLinking(); }, Qt::QueuedConnection);
            inventory::ScanOptions scan;
            scan.appx = false;  // Avel doesn't trace packaged apps
            const auto inventory = inventory::scan_inventory(scan);
            if (auto linked = tracker::link_trace(*store, trace->id, outcome->result.result, inventory,
                                                  outcome->linkError)) {
                outcome->result.trace = std::move(*linked);
            }
            // Retention, as after any trace that stops; an empty inventory is no evidence.
            if (!inventory.programs.empty()) {
                store->purge([&inventory](tracker::TraceInfo& info) {
                    return tracker::refresh_links(info, inventory);
                });
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, outcome = std::shared_ptr<const Outcome>(std::move(outcome))] { onFinished(outcome); },
            Qt::QueuedConnection);
    });
}

void TraceProgressDialog::onStatus(const Status& status) {
    switch (status.phase) {
        case InstallTracePhase::Starting:
            setPhase(SnapshotPhase);
            statusLabel_->setText(TraceText::stepText(status.step));
            break;
        case InstallTracePhase::Watching:
            launched_ = true;
            switch (status.watch) {
                case tracker::WatchPhase::Running:
                case tracker::WatchPhase::WaitingForHelpers:
                    setPhase(InstallerPhase);
                    break;
                case tracker::WatchPhase::WaitingForWindowsInstaller:
                    setPhase(WindowsInstallerPhase);
                    break;
                case tracker::WatchPhase::Settling:
                    setPhase(SettlingPhase);
                    break;
            }
            if (!cancellation_.stop_batch) {
                statusLabel_->setText(TraceText::watchText(status.watch));
            }
            showProcesses(status.running);
            break;
        case InstallTracePhase::Finishing:
            setPhase(FinishingPhase);
            stopButton_->setEnabled(false);
            statusLabel_->setText(TraceText::stepText(status.step));
            break;
    }
}

void TraceProgressDialog::onLinking() {
    setPhase(FinishingPhase);
    stopButton_->setEnabled(false);
    statusLabel_->setText(tr("Linking the trace to the installed program…"));
}

void TraceProgressDialog::onFinished(const std::shared_ptr<const Outcome>& outcome) {
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
    stopButton_->setEnabled(false);
    stopButton_->hide();
    closeButton_->setEnabled(true);
    closeButton_->setDefault(true);
    // A question about stopping that is still open has nothing left to stop.
    for (auto* box : findChildren<QMessageBox*>()) {
        box->close();
    }

    const auto& result = outcome->result;
    const bool installerRan = result.watch && result.watch->end != tracker::WatchEnd::LaunchFailed;
    QStringList lines;
    bool review = false;
    if (!result.trace) {
        statusLabel_->setText(tr("No trace was recorded."));
        switch (result.error) {
            case TraceError::Cancelled:
                lines << tr("Stopped before the installer was started. The trace was discarded.");
                break;
            case TraceError::LaunchFailed:
                lines << tr("%1 couldn't be started, so the trace was discarded: %2")
                             .arg(QDir::toNativeSeparators(toQString(request_.installer)),
                                  TraceText::launchErrorText(result.watch ? result.watch->error : 0));
                break;
            case TraceError::AlreadyOpen:
                lines << tr("Another trace is open. Finish or discard it first (Trace > Traces).");
                break;
            default:
                lines << tr("The trace couldn't start: %1.").arg(TraceText::errorText(result.error));
                break;
        }
    } else if (const auto& info = *result.trace; info.stop_reason == tracker::StopReason::RebootPending) {
        traceId_ = toQString(info.id);
        statusLabel_->setText(tr("The trace stays open."));
        lines << tr(
            "The installer asked Windows to restart. The trace stays open, so that what the installer "
            "does during the restart is recorded too: restart Windows, then finish the trace. Avel "
            "offers to when it starts.");
        if (result.error != TraceError::None) {
            lines << tr("The trace's state couldn't be saved; it stays open all the same.");
        }
    } else if (!isFinished(info.state)) {
        traceId_ = toQString(info.id);
        statusLabel_->setText(tr("The trace isn't finished."));
        lines << tr("The trace's result couldn't be saved. Finish it again from Trace > Traces.");
    } else {
        traceId_ = toQString(info.id);
        review = true;
        statusLabel_->setText(tr("The trace is finished."));
        if (info.watch) {
            switch (info.watch->end) {
                case tracker::WatchEnd::InstallerDone:
                    lines << tr("The installation is over (the installer's exit code: %1).")
                                 .arg(info.watch->exit_code);
                    break;
                case tracker::WatchEnd::Stopped:
                    lines << tr(
                        "The trace was stopped before the installer was done. What the installer does "
                        "from now on isn't recorded.");
                    break;
                case tracker::WatchEnd::TimedOut:
                    lines << tr("Avel gave up waiting for the installer, which may still be running.");
                    break;
                case tracker::WatchEnd::LaunchFailed:
                    break;
            }
        }
        // Noise isn't counted among the changes.
        lines << tr("%Ln change(s) recorded.", nullptr, static_cast<int>(info.counts.changes)) + u' ' +
                     tr("%Ln more set aside as noise.", nullptr, static_cast<int>(info.counts.noise));
        if (outcome->linkError != TraceError::None) {
            lines
                << tr("The trace couldn't be linked to the program it installed: %1. You can link it by hand "
                      "in the review.")
                       .arg(TraceText::errorText(outcome->linkError));
        } else if (info.links.empty()) {
            lines << tr(
                "Avel found no installed program to link the trace to. You can link it by hand in the "
                "review.");
        } else {
            lines << tr("Linked to %1.").arg(TraceText::linkText(info.links.front()));
        }
        if (info.watch && !info.watch->left_running.empty()) {
            lines << tr("Started by the installer and still running: %1.")
                         .arg(TraceText::processList(info.watch->left_running));
        }
        if (info.state == tracker::TraceState::Incomplete) {
            lines << tr(
                "The trace is incomplete: part of the system couldn't be compared. The review says what.");
        }
    }
    setPhase(PhaseCount);
    resultLabel_->setText(lines.join(u'\n'));
    resultLabel_->show();
    reviewButton_->setVisible(review);
    if (review) {
        reviewButton_->setDefault(true);
    }
    Q_EMIT traceEnded(installerRan);
    if (review) {
        // Straight on to the review; the outcome stays here, behind it.
        Q_EMIT reviewRequested(traceId_);
    }
}

void TraceProgressDialog::requestStop() {
    if (!running_ || !stopButton_->isEnabled() || cancellation_.stop_batch) {
        return;
    }
    if (!launched_) {
        // Nothing runs yet: the trace is discarded and the installer isn't started.
        cancellation_.stop_batch = true;
        stopButton_->setEnabled(false);
        statusLabel_->setText(tr("Stopping; the installer won't be started…"));
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Question, tr("Stop the trace"), tr("What should Avel do?"),
                                QMessageBox::NoButton, this);
    box->setObjectName(u"stopTraceQuestion"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(
        tr("Stopping the trace now leaves the installer running: what it does from then on isn't recorded. "
           "Ending the installer can leave the program half installed."));
    QPushButton* stop = box->addButton(tr("Stop the trace"), QMessageBox::AcceptRole);
    stop->setObjectName(u"stopTraceOnly"_s);
    QPushButton* end = box->addButton(tr("Stop and end the installer"), QMessageBox::DestructiveRole);
    end->setObjectName(u"stopTraceAndInstaller"_s);
    QPushButton* keep = box->addButton(tr("Keep tracing"), QMessageBox::RejectRole);
    box->setDefaultButton(keep);
    connect(box, &QMessageBox::buttonClicked, this, [this, stop, end](QAbstractButton* clicked) {
        if (!running_ || (clicked != stop && clicked != end)) {
            return;
        }
        if (clicked == end) {
            cancellation_.terminate_current = true;
        }
        cancellation_.stop_batch = true;
        stopButton_->setEnabled(false);
        statusLabel_->setText(clicked == end ? tr("Ending the installer and stopping the trace…")
                                             : tr("Stopping the trace; the installer keeps running…"));
    });
    box->open();
}

void TraceProgressDialog::setPhase(int phase) {
    for (int i = 0; i < PhaseCount; ++i) {
        QLabel* label = phaseLabels_[static_cast<std::size_t>(i)];
        QFont font = label->font();
        font.setBold(i == phase);
        label->setFont(font);
        label->setEnabled(i <= phase);
    }
}

void TraceProgressDialog::showProcesses(const std::vector<tracker::WatchProcess>& processes) {
    processes_->clear();
    QList<QTreeWidgetItem*> rows;
    for (const auto& process : processes) {
        auto* row = new QTreeWidgetItem;
        const QString image = QDir::toNativeSeparators(toQString(process.image));
        row->setText(ProcessNameColumn, image.isEmpty() ? tr("(unknown)") : QFileInfo(image).fileName());
        row->setToolTip(ProcessNameColumn, image);
        row->setText(ProcessIdColumn, QString::number(process.pid));
        row->setText(ProcessRoleColumn,
                     process.helper ? tr("The installer or its helper") : tr("A program it started"));
        rows << row;
    }
    processes_->addTopLevelItems(rows);
}

}  // namespace avel::app
