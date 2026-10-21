#pragma once

#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/changes/executor.h"
#include "core/changes/store.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;

namespace avel::app {

// Removes a plan's items as one change set, or puts a set's items back, on a worker thread, and
// shows the progress and what didn't work. After a removal it offers to undo it right away.
// It can't be closed while it runs: every step is backed up and recorded, and stopping halfway
// would only leave a set to undo.
class ChangeSetProgressDialog : public QDialog {
    Q_OBJECT

public:
    // Removes the plan's items. Packages are only removed if `allowIrreversible`.
    ChangeSetProgressDialog(std::shared_ptr<changes::ChangeStore> store, changes::ChangePlan plan,
                            bool allowIrreversible, QWidget* parent = nullptr);
    // Puts back what set `setId` removed: everything, or only the items in `only`. `statuses` are
    // the items' states as the set's journal has them.
    ChangeSetProgressDialog(std::shared_ptr<changes::ChangeStore> store, std::wstring setId,
                            changes::ChangePlan plan, std::vector<changes::ItemStatus> statuses,
                            std::vector<std::size_t> only, QWidget* parent = nullptr);
    ~ChangeSetProgressDialog() override;

    // Whether anything was removed or restored.
    bool changedAnything() const { return changedAnything_; }

    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    ChangeSetProgressDialog(std::shared_ptr<changes::ChangeStore> store, changes::ChangePlan plan,
                            QWidget* parent);
    void startRemoval();
    void startRestore();
    void onItemFinished(std::size_t index, const changes::ItemResult& result);
    void onRemovalFinished(const changes::ExecuteResult& result);
    void onRestoreFinished(const changes::RestoreResult& result);
    void onBusy();
    void finishRun();
    void addProblem(std::size_t index, const changes::ItemResult& result);
    std::size_t restorableCount() const;

    std::shared_ptr<changes::ChangeStore> store_;
    changes::ChangePlan plan_;
    std::vector<changes::ItemStatus> statuses_;  // Each item's latest state
    std::wstring setId_;
    std::vector<std::size_t> only_;
    bool allowIrreversible_ = false;
    bool restoring_ = false;
    std::thread worker_;
    bool started_ = false;
    bool running_ = false;
    bool changedAnything_ = false;

    QLabel* statusLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* problemsLabel_ = nullptr;
    QTreeWidget* problems_ = nullptr;
    QPushButton* undoButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

}  // namespace avel::app
