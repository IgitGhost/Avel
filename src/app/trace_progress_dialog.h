#pragma once

#include <QDialog>
#include <QString>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/tracker/install_trace.h"
#include "core/tracker/trace_store.h"
#include "core/uninstall/process_runner.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace avel::app {

// Install and trace: starts a trace, runs the installer, waits until the installation is over and
// stops the trace (tracker::install_and_trace), on a worker thread, then links the trace to the
// program it installed. Shows what it's waiting for, with the installer's processes. Stop stops
// the trace at once and leaves the installer running, or also ends the installer. Once the trace
// is finished, reviewRequested() asks for its review.
class TraceProgressDialog : public QDialog {
    Q_OBJECT

public:
    struct Request {
        std::wstring installer;  // Full path
        std::wstring arguments;
        std::wstring description;
    };

    TraceProgressDialog(std::shared_ptr<tracker::TraceStore> store, Request request,
                        QWidget* parent = nullptr);
    ~TraceProgressDialog() override;

    void reject() override;

Q_SIGNALS:
    // The trace has finished and is linked: open its review.
    void reviewRequested(const QString& traceId);
    // The worker is done, whatever came of it. `installerRan`: the installer was started, so the
    // program list may be out of date.
    void traceEnded(bool installerRan);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct Status;
    struct Outcome;

    enum Phase {
        SnapshotPhase,
        InstallerPhase,
        WindowsInstallerPhase,
        SettlingPhase,
        FinishingPhase,
        PhaseCount
    };

    void start();
    void onStatus(const Status& status);
    void onLinking();
    void onFinished(const std::shared_ptr<const Outcome>& outcome);
    void requestStop();
    void setPhase(int phase);
    void showProcesses(const std::vector<tracker::WatchProcess>& processes);

    std::shared_ptr<tracker::TraceStore> store_;
    Request request_;
    uninstall::Cancellation cancellation_;
    std::thread worker_;
    bool started_ = false;
    bool running_ = false;
    bool launched_ = false;  // The installer was started
    QString traceId_;

    std::array<QLabel*, PhaseCount> phaseLabels_{};
    QLabel* statusLabel_ = nullptr;
    QLabel* processesLabel_ = nullptr;
    QTreeWidget* processes_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* reviewButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

}  // namespace avel::app
