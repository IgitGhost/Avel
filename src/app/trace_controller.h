#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/tracker/trace_record.h"
#include "core/tracker/trace_session.h"
#include "core/tracker/trace_store.h"

class QDialog;
class QLabel;
class QPushButton;
class QWidget;

namespace avel::app {

class TraceStoreAccess;

// Drives Avel Trace for the GUI: a manual trace (Start tracing, Stop tracing) whose session reads
// the journals on a worker thread for as long as Avel runs, Install and trace (its dialogs), and
// what is left of an open trace after a restart or a crash (finish it, keep tracing, discard it).
// Starting and stopping run off the GUI thread behind a progress dialog; a stopped trace is linked
// to the program it installed and reviewRequested() asks for its review.
class TraceController : public QObject {
    Q_OBJECT

public:
    enum class State : std::uint8_t {
        Idle,        // No trace is driven here (one may be open: openTraceId())
        Starting,    // A manual trace starts
        Tracing,     // This process drives the open trace
        Stopping,    // A trace is being stopped (or discarded) and linked
        Installing,  // Install and trace runs
    };

    // `window` parents the dialogs; `traces` must outlive the controller.
    TraceController(TraceStoreAccess* traces, QWidget* window);
    ~TraceController() override;

    State state() const { return state_; }
    // Starting, stopping or installing: nothing else can be done with traces meanwhile.
    bool busy() const { return state_ != State::Idle && state_ != State::Tracing; }
    // The trace that is open (or stopping), as the store last said; empty when there is none.
    QString openTraceId() const { return openId_; }
    // The open trace is held by another process (avel-cli, another Avel window).
    bool openTraceHeldElsewhere() const { return openHeldElsewhere_; }
    // The trace this process drives while Tracing, or that it is stopping.
    QString drivenTraceId() const { return drivenId_; }

    // "Tracing since 14:02" with a Stop button, for the status bar; shown while tracing.
    QWidget* statusWidget() const { return statusWidget_; }
    // The status bar's texts, again in the UI language.
    void retranslateUi();

    // At startup: asks what to do with a trace left open (Finish, Keep tracing, Discard).
    void checkAtStartup();
    // Install and trace…: the installer to trace, then the progress until the trace stops.
    void installAndTrace();
    // Asks for a description, then starts a manual trace.
    void startTracing();
    // Stops the trace driven here, or finishes the open one.
    void stopTracing();
    // Finishes an open trace (Traces dialog, startup): stops it, links it, asks for its review.
    void finishTrace(const QString& id);
    // Resumes recording an open trace no process holds.
    void keepTracing(const QString& id);
    // Discards an open trace (after asking), or removes one that died while starting.
    void discardTrace(const QString& id);

Q_SIGNALS:
    void stateChanged();
    // A trace has finished: open its review.
    void reviewRequested(const QString& traceId);
    // An installer ran or a trace was linked: the program list may be out of date.
    void programsChanged();

private:
    struct StopJob;

    void setState(State state);
    void refresh();
    void onRefreshed(std::optional<tracker::TraceInfo> open, bool held);
    void promptOpenTrace(const tracker::TraceInfo& info);
    void beginStart(const QString& description);
    void onStarted(std::shared_ptr<tracker::TraceSession> session, tracker::TraceError error,
                   const std::optional<tracker::TraceInfo>& info, bool resumed);
    void beginResume(const QString& id);
    void beginStop(const QString& id, bool discard);
    void onStopped(const std::shared_ptr<const StopJob>& job);
    void showStep(const QString& text);
    void hideStep();
    void showMessage(const QString& title, const QString& text, const QString& details = {},
                     bool warning = false);
    bool storeAvailable();
    void updateStatusWidget();

    TraceStoreAccess* traces_ = nullptr;
    std::shared_ptr<tracker::TraceStore> store_;
    QWidget* window_ = nullptr;
    State state_ = State::Idle;
    std::shared_ptr<tracker::TraceSession> session_;  // While Tracing
    std::chrono::system_clock::time_point since_{};
    QString drivenId_;
    QString openId_;
    bool openHeldElsewhere_ = false;
    bool refreshing_ = false;
    bool refreshAgain_ = false;

    QWidget* statusWidget_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPointer<QDialog> stepDialog_;
    QLabel* stepLabel_ = nullptr;
    // Declared last so it's destroyed first: its destructor waits for the worker.
    QThreadPool pool_;
};

}  // namespace avel::app
