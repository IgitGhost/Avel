#pragma once

#include <QDialog>

#include <cstddef>
#include <thread>
#include <vector>

#include "core/platform/processes.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace avel::app {

// Lists the processes that run from the folders Avel Force is about to remove and, once the user
// agrees, ends them on a worker thread: each is asked to close first, then ended. Accepted when
// they are all gone, or when the user goes on without the ones that wouldn't end (their files
// stay in place).
class RunningProcessesDialog : public QDialog {
    Q_OBJECT

public:
    // `subject` names the programs, e.g. "Contoso Fixture".
    RunningProcessesDialog(std::vector<platform::ProcessInfo> processes, const QString& subject,
                           QWidget* parent = nullptr);
    ~RunningProcessesDialog() override;

    void accept() override;
    void reject() override;

    static QString outcomeText(const platform::StopResult& result);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void endProcesses();
    void onProcessStopped(std::size_t index, platform::StopResult result);
    void onAllStopped();

    std::vector<platform::ProcessInfo> processes_;
    std::vector<platform::StopResult> results_;
    std::thread worker_;
    bool running_ = false;
    bool ended_ = false;  // The processes were ended (or tried to be); accepting goes on

    QLabel* statusLabel_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QPushButton* endButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
};

}  // namespace avel::app
