#include "trace_controller.h"

#include <QCloseEvent>
#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/inventory/inventory.h"
#include "core/tracker/linking.h"
#include "install_trace_dialog.h"
#include "program_text.h"
#include "trace_progress_dialog.h"
#include "trace_store_access.h"
#include "trace_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

using tracker::TraceError;
using tracker::TraceState;

bool isFinished(TraceState state) { return state == TraceState::Complete || state == TraceState::Incomplete; }

// Starting and stopping a trace can't be interrupted half way; the dialog that shows them waits.
class StepDialog : public QDialog {
public:
    using QDialog::QDialog;
    void reject() override {}

protected:
    void closeEvent(QCloseEvent* event) override { event->ignore(); }
};

}  // namespace

// Stopping or discarding a trace on the worker, and what came of it.
struct TraceController::StopJob {
    QString id;
    bool discard = false;
    std::shared_ptr<tracker::TraceSession> session;  // The session driven here, if it's this trace's
    TraceError error = TraceError::None;             // Resuming or discarding it failed
    std::optional<tracker::TraceInfo> info;          // Once stopped (and linked)
    TraceError linkError = TraceError::None;
    bool discarded = false;
    bool finishedMeanwhile = false;  // Another process finished it before it could be discarded
};

TraceController::TraceController(TraceStoreAccess* traces, QWidget* window)
    : QObject(window), traces_(traces), store_(traces->store()), window_(window) {
    pool_.setMaxThreadCount(1);

    statusWidget_ = new QWidget(window);
    statusWidget_->setObjectName(u"tracingStatus"_s);
    auto* layout = new QHBoxLayout(statusWidget_);
    layout->setContentsMargins(0, 0, 0, 0);
    statusLabel_ = new QLabel(statusWidget_);
    statusLabel_->setObjectName(u"tracingLabel"_s);
    stopButton_ = new QPushButton(statusWidget_);
    stopButton_->setObjectName(u"stopTracingButton"_s);
    stopButton_->setFlat(true);
    connect(stopButton_, &QPushButton::clicked, this, &TraceController::stopTracing);
    layout->addWidget(statusLabel_);
    layout->addWidget(stopButton_);
    statusWidget_->hide();

    connect(traces_, &TraceStoreAccess::tracesChanged, this, &TraceController::refresh);
    retranslateUi();
}

TraceController::~TraceController() {
    pool_.waitForDone();
    // A trace driven here stays open: its session lets go of it, and the next start offers to
    // finish it.
    session_.reset();
}

void TraceController::retranslateUi() {
    stopButton_->setText(tr("Stop tracing"));
    stopButton_->setToolTip(tr("Stop the trace and review what the installation changed"));
    updateStatusWidget();
}

void TraceController::setState(State state) {
    state_ = state;
    updateStatusWidget();
    Q_EMIT stateChanged();
}

void TraceController::updateStatusWidget() {
    const bool tracing = state_ == State::Tracing;
    const bool stopping = state_ == State::Stopping && !drivenId_.isEmpty();
    statusLabel_->setText(stopping ? tr("Stopping the trace…")
                                   : tr("Tracing since %1").arg(TraceText::sinceText(since_)));
    stopButton_->setEnabled(tracing);
    statusWidget_->setVisible(tracing || stopping);
}

bool TraceController::storeAvailable() {
    if (store_) {
        return true;
    }
    showMessage(tr("Avel Trace"), traces_->unavailableReason(), {}, true);
    return false;
}

void TraceController::refresh() {
    if (!store_) {
        return;
    }
    if (refreshing_) {
        refreshAgain_ = true;
        return;
    }
    refreshing_ = true;
    pool_.start([this, store = store_] {
        auto open = store->open_trace();
        const bool held = open && store->held(open->id);
        QMetaObject::invokeMethod(
            this, [this, open = std::move(open), held] { onRefreshed(open, held); }, Qt::QueuedConnection);
    });
}

void TraceController::onRefreshed(std::optional<tracker::TraceInfo> open, bool held) {
    refreshing_ = false;
    const QString id = open ? toQString(open->id) : QString();
    openId_ = id;
    // This process's own session holds the trace it drives.
    openHeldElsewhere_ = held && id != drivenId_;
    Q_EMIT stateChanged();
    if (refreshAgain_) {
        refreshAgain_ = false;
        refresh();
    }
}

void TraceController::checkAtStartup() {
    if (!store_) {
        return;
    }
    refreshing_ = true;
    pool_.start([this, store = store_] {
        auto open = store->open_trace();
        const bool held = open && store->held(open->id);
        QMetaObject::invokeMethod(
            this,
            [this, open = std::move(open), held] {
                onRefreshed(open, held);
                // A trace that died while starting holds nothing: the store's maintenance removes it.
                if (open && !held && state_ == State::Idle &&
                    (open->state == TraceState::Open || open->state == TraceState::Stopping)) {
                    promptOpenTrace(*open);
                }
            },
            Qt::QueuedConnection);
    });
}

void TraceController::promptOpenTrace(const tracker::TraceInfo& info) {
    const QString name = TraceText::traceName(info);
    const QString started = TraceText::timeText(info.started);
    auto* box =
        new QMessageBox(QMessageBox::Question, tr("Open trace"), QString(), QMessageBox::NoButton, window_);
    box->setObjectName(u"openTracePrompt"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* finish = box->addButton(tr("Finish the trace"), QMessageBox::AcceptRole);
    finish->setObjectName(u"finishOpenTrace"_s);
    QPushButton* keep = nullptr;
    if (info.state == TraceState::Stopping) {
        box->setText(tr("The trace of %1, started %2, didn't finish stopping.").arg(name, started));
        box->setInformativeText(tr("Avel can finish it now: everything it needs was saved."));
    } else {
        keep = box->addButton(tr("Keep tracing"), QMessageBox::ActionRole);
        keep->setObjectName(u"keepTracing"_s);
        if (info.stop_reason == tracker::StopReason::RebootPending) {
            box->setText(
                tr("The installer of %1 asked Windows to restart. Its trace, started %2, stayed open so "
                   "that what the installer did during the restart is recorded too.")
                    .arg(name, started));
            box->setInformativeText(tr("If Windows has restarted since, finish the trace now."));
        } else {
            box->setText(
                tr("The trace of %1, started %2, is still open: Avel or Windows stopped while it was "
                   "recording.")
                    .arg(name, started));
            box->setInformativeText(tr(
                "Finish it if the installation is over, or keep tracing if it isn't. What changed while Avel "
                "wasn't running is recorded as long as the drives' change journals still hold it."));
        }
    }
    QPushButton* discard = box->addButton(tr("Discard it"), QMessageBox::DestructiveRole);
    discard->setObjectName(u"discardOpenTrace"_s);
    box->addButton(tr("Later"), QMessageBox::RejectRole);
    box->setDefaultButton(finish);
    const QString id = toQString(info.id);
    connect(box, &QMessageBox::buttonClicked, this,
            [this, id, finish, keep, discard](QAbstractButton* clicked) {
                if (clicked == finish) {
                    finishTrace(id);
                } else if (keep != nullptr && clicked == keep) {
                    keepTracing(id);
                } else if (clicked == discard) {
                    beginStop(id, true);
                }
            });
    box->open();
}

void TraceController::installAndTrace() {
    if (!storeAvailable() || busy()) {
        return;
    }
    if (state_ == State::Tracing) {
        showMessage(tr("Install and trace"), tr("A trace is being recorded. Stop it first."));
        return;
    }
    if (!openId_.isEmpty()) {
        showMessage(tr("Install and trace"),
                    tr("Another trace is open. Finish or discard it first (Trace > Traces)."));
        return;
    }
    auto* dialog = new InstallTraceDialog(window_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog] {
        if (busy() || state_ == State::Tracing) {
            return;
        }
        auto* progress = new TraceProgressDialog(
            store_, {dialog->installer(), dialog->arguments(), dialog->description()}, window_);
        progress->setAttribute(Qt::WA_DeleteOnClose);
        connect(progress, &TraceProgressDialog::reviewRequested, this, &TraceController::reviewRequested);
        connect(progress, &TraceProgressDialog::traceEnded, this, [this](bool installerRan) {
            setState(State::Idle);
            traces_->notifyChanged();
            if (installerRan) {
                Q_EMIT programsChanged();
            }
        });
        setState(State::Installing);
        progress->open();
    });
    dialog->open();
}

void TraceController::startTracing() {
    if (!storeAvailable() || state_ != State::Idle) {
        return;
    }
    if (!openId_.isEmpty()) {
        showMessage(tr("Start tracing"),
                    tr("Another trace is open. Finish or discard it first (Trace > Traces)."));
        return;
    }
    auto* input = new QInputDialog(window_);
    input->setObjectName(u"startTracingDialog"_s);
    input->setAttribute(Qt::WA_DeleteOnClose);
    input->setWindowTitle(tr("Start tracing"));
    input->setLabelText(tr("What are you about to install? The trace is named after it."));
    input->setOkButtonText(tr("Start tracing"));
    connect(input, &QInputDialog::textValueSelected, this, &TraceController::beginStart);
    input->open();
}

void TraceController::stopTracing() {
    if (state_ == State::Tracing) {
        beginStop(drivenId_, false);
    } else if (state_ == State::Idle && !openId_.isEmpty()) {
        finishTrace(openId_);
    }
}

void TraceController::finishTrace(const QString& id) {
    if (!storeAvailable() || busy() || (state_ == State::Tracing && id != drivenId_)) {
        return;
    }
    beginStop(id, false);
}

void TraceController::keepTracing(const QString& id) {
    if (!storeAvailable() || state_ != State::Idle) {
        return;
    }
    beginResume(id);
}

void TraceController::discardTrace(const QString& id) {
    if (!storeAvailable() || busy() || (state_ == State::Tracing && id != drivenId_)) {
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Warning, tr("Discard the trace"),
                                tr("Discard the open trace? What it recorded is lost."),
                                QMessageBox::Yes | QMessageBox::Cancel, window_);
    box->setObjectName(u"discardTraceQuestion"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::finished, this, [this, id](int result) {
        if (result == QMessageBox::Yes && !busy()) {
            beginStop(id, true);
        }
    });
    box->open();
}

void TraceController::beginStart(const QString& description) {
    if (state_ != State::Idle) {
        return;
    }
    setState(State::Starting);
    showStep(TraceText::stepText(tracker::SessionStep::Volumes));
    pool_.start([this, store = store_, description = description.trimmed().toStdWString()] {
        tracker::StartOptions start;
        start.description = description;
        start.mode = tracker::TraceMode::Manual;
        TraceError error = TraceError::None;
        std::shared_ptr<tracker::TraceSession> session =
            tracker::TraceSession::start(*store, start, error, [this](tracker::SessionStep step) {
                QMetaObject::invokeMethod(
                    this, [this, step] { showStep(TraceText::stepText(step)); }, Qt::QueuedConnection);
            });
        std::optional<tracker::TraceInfo> info;
        if (session) {
            info = session->info();
            session->start_draining();
        } else if (error == TraceError::AlreadyOpen) {
            info = store->open_trace();
        }
        QMetaObject::invokeMethod(
            this,
            [this, session = std::move(session), error, info = std::move(info)] {
                onStarted(session, error, info, false);
            },
            Qt::QueuedConnection);
    });
}

void TraceController::beginResume(const QString& id) {
    setState(State::Starting);
    showStep(tr("Continuing the trace…"));
    pool_.start([this, store = store_, id = id.toStdWString()] {
        TraceError error = TraceError::None;
        std::shared_ptr<tracker::TraceSession> session = tracker::TraceSession::resume(*store, id, error);
        std::optional<tracker::TraceInfo> info;
        if (session && session->info().state != TraceState::Open) {
            // Stopping: it can only be finished.
            session.reset();
            error = TraceError::NotOpen;
        }
        if (session) {
            info = session->info();
            session->start_draining();
        }
        QMetaObject::invokeMethod(
            this,
            [this, session = std::move(session), error, info = std::move(info)] {
                onStarted(session, error, info, true);
            },
            Qt::QueuedConnection);
    });
}

void TraceController::onStarted(std::shared_ptr<tracker::TraceSession> session, TraceError error,
                                const std::optional<tracker::TraceInfo>& info, bool resumed) {
    hideStep();
    if (!session || !info) {
        setState(State::Idle);
        refresh();
        const QString title = resumed ? tr("Keep tracing") : tr("Start tracing");
        if (error == TraceError::AlreadyOpen) {
            showMessage(title, tr("Another trace is open (%1). Finish or discard it first (Trace > Traces).")
                                   .arg(info ? TraceText::traceName(*info) : QString()));
        } else if (resumed && error == TraceError::InUse) {
            showMessage(title, tr("Another Avel process is recording this trace (avel-cli or another Avel "
                                  "window). Finish it there."));
        } else if (resumed && error == TraceError::NotOpen) {
            showMessage(title, tr("The trace can't be continued: it's being stopped, or it didn't finish "
                                  "starting. Finish it or discard it instead."));
        } else {
            showMessage(title, tr("The trace couldn't start: %1.").arg(TraceText::errorText(error)), {},
                        true);
        }
        return;
    }
    session_ = std::move(session);
    drivenId_ = toQString(info->id);
    since_ = info->started;
    setState(State::Tracing);
    refresh();
    if (!resumed) {
        showMessage(
            tr("Tracing"),
            tr("Avel is recording what changes. Install the program now. When the installation is over, "
               "choose Stop tracing, in the Trace menu or the status bar."),
            tr("Close the programs you don't need meanwhile: what they change is recorded too, and only "
               "what Avel recognizes as noise is set aside. Avel reads the drives' change journals as "
               "long as it runs; if it closes, the trace stays open and Avel offers to finish it at its "
               "next start."));
    }
}

void TraceController::beginStop(const QString& id, bool discard) {
    if (busy() || id.isEmpty()) {
        return;
    }
    auto job = std::make_shared<StopJob>();
    job->id = id;
    job->discard = discard;
    if (session_ && drivenId_ == id) {
        // The worker owns it now; the trace is let go of once it's stopped.
        job->session = std::move(session_);
    }
    drivenId_ = id;
    setState(State::Stopping);
    showStep(discard ? tr("Discarding the trace…") : TraceText::stepText(tracker::SessionStep::Draining));
    pool_.start([this, store = store_, job] {
        const std::wstring id = job->id.toStdWString();
        if (!job->session) {
            job->session = tracker::TraceSession::resume(*store, id, job->error);
        }
        if (!job->session) {
            // It died while starting, or can't be read: there's nothing to stop, only files to
            // delete. Unless another process finished it meanwhile.
            if (job->discard && (job->error == TraceError::NotOpen || job->error == TraceError::Damaged)) {
                if (const auto now = store->read(id); now && isFinished(now->state)) {
                    job->finishedMeanwhile = true;
                } else {
                    job->discarded = store->remove(id, job->error);
                }
            }
        } else if (job->discard) {
            job->discarded = job->session->cancel();
            job->error = job->discarded ? TraceError::None : TraceError::WriteFailed;
            job->session.reset();
        } else {
            const auto result =
                job->session->stop(tracker::StopReason::Manual, [this](tracker::SessionStep step) {
                    QMetaObject::invokeMethod(
                        this, [this, step] { showStep(TraceText::stepText(step)); }, Qt::QueuedConnection);
                });
            job->info = job->session->info();
            job->session.reset();  // Lets go of the trace, so it can be linked
            if (isFinished(job->info->state)) {
                QMetaObject::invokeMethod(
                    this, [this] { showStep(tr("Linking the trace to the installed program…")); },
                    Qt::QueuedConnection);
                inventory::ScanOptions scan;
                scan.appx = false;  // Avel doesn't trace packaged apps
                const auto inventory = inventory::scan_inventory(scan);
                if (auto linked = tracker::link_trace(*store, id, result, inventory, job->linkError)) {
                    job->info = std::move(*linked);
                }
                // Retention, as after any trace that stops; an empty inventory is no evidence.
                if (!inventory.programs.empty()) {
                    store->purge([&inventory](tracker::TraceInfo& info) {
                        return tracker::refresh_links(info, inventory);
                    });
                }
            }
        }
        QMetaObject::invokeMethod(
            this, [this, job = std::shared_ptr<const StopJob>(job)] { onStopped(job); },
            Qt::QueuedConnection);
    });
}

void TraceController::onStopped(const std::shared_ptr<const StopJob>& job) {
    hideStep();
    drivenId_.clear();
    setState(State::Idle);
    traces_->notifyChanged();
    if (job->discard) {
        if (job->finishedMeanwhile) {
            showMessage(tr("Discard the trace"),
                        tr("The trace was finished in the meantime. Trace > Traces can delete it."));
        } else if (!job->discarded) {
            showMessage(
                tr("Discard the trace"),
                job->error == TraceError::InUse
                    ? tr("Another Avel process is recording this trace (avel-cli or another Avel window).")
                    : tr("The trace couldn't be discarded completely: %1.")
                          .arg(TraceText::errorText(job->error)),
                {}, true);
        }
        return;
    }
    if (!job->info) {
        QString text;
        switch (job->error) {
            case TraceError::InUse:
                text = tr(
                    "Another Avel process is recording this trace (avel-cli or another Avel window). Finish "
                    "it there, or wait until it's done.");
                break;
            case TraceError::NotOpen:
                text = tr("The trace didn't finish starting, so there's nothing to finish. Discard it.");
                break;
            case TraceError::Damaged:
                text = tr("The trace's files can't be read. Discard it.");
                break;
            default:
                text = tr("The trace can't be finished: %1.").arg(TraceText::errorText(job->error));
                break;
        }
        showMessage(tr("Finish the trace"), text, {}, true);
        return;
    }
    if (!isFinished(job->info->state)) {
        // Still `stopping`: everything it needs is on disk, so finishing again completes it.
        showMessage(tr("Finish the trace"),
                    tr("The trace's result couldn't be saved. Finish it again from Trace > Traces."), {},
                    true);
        return;
    }
    Q_EMIT programsChanged();
    Q_EMIT reviewRequested(job->id);
    if (job->linkError != TraceError::None) {
        showMessage(
            tr("Finish the trace"),
            tr("The trace couldn't be linked to the program it installed: %1. You can link it by hand in "
               "the review.")
                .arg(TraceText::errorText(job->linkError)));
    }
}

void TraceController::showStep(const QString& text) {
    if (!stepDialog_) {
        auto* dialog = new StepDialog(window_);
        dialog->setObjectName(u"traceStepDialog"_s);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Avel Trace"));
        dialog->setWindowFlag(Qt::WindowCloseButtonHint, false);
        dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        stepLabel_ = new QLabel(dialog);
        stepLabel_->setObjectName(u"traceStepText"_s);
        stepLabel_->setWordWrap(true);
        stepLabel_->setMinimumWidth(420);
        auto* bar = new QProgressBar(dialog);
        bar->setRange(0, 0);
        bar->setTextVisible(false);
        auto* layout = new QVBoxLayout(dialog);
        layout->addWidget(stepLabel_);
        layout->addWidget(bar);
        stepDialog_ = dialog;
        dialog->open();
    }
    stepLabel_->setText(text);
}

void TraceController::hideStep() {
    if (stepDialog_) {
        stepDialog_->hide();
        stepDialog_->deleteLater();
        stepDialog_ = nullptr;
        stepLabel_ = nullptr;
    }
}

void TraceController::showMessage(const QString& title, const QString& text, const QString& details,
                                  bool warning) {
    auto* box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, title, text,
                                QMessageBox::Ok, window_);
    box->setObjectName(u"traceMessage"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    if (!details.isEmpty()) {
        box->setInformativeText(details);
    }
    box->open();
}

}  // namespace avel::app
