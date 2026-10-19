#pragma once

#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/uninstall/batch.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace avel::app {

// Runs an uninstall batch on a worker thread and shows its progress. Can't be closed while an
// uninstaller runs; "Stop" ends the batch after the current program, or terminates it.
class UninstallProgressDialog : public QDialog {
    Q_OBJECT

public:
    // `inventory` keeps the programs the items point to alive.
    UninstallProgressDialog(std::shared_ptr<const inventory::Inventory> inventory,
                            std::vector<uninstall::BatchItem> items, uninstall::BatchOptions options,
                            QWidget* parent = nullptr);
    ~UninstallProgressDialog() override;

    // Whether any uninstaller ran, so the program list needs refreshing.
    bool changedAnything() const { return changedAnything_; }

    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void onRestorePointDone(uninstall::RestorePointStatus status, std::uint32_t error);
    bool askContinueWithoutRestorePoint(std::uint32_t error);
    void onItemStarted(std::size_t index);
    void onItemFinished(std::size_t index, const uninstall::UninstallResult& result);
    void onBatchFinished();
    void requestStop();
    void setStatus(int row, const QString& text);

    std::shared_ptr<const inventory::Inventory> inventory_;
    std::vector<uninstall::BatchItem> items_;
    uninstall::BatchOptions options_;
    uninstall::Cancellation cancellation_;
    std::thread worker_;
    bool started_ = false;
    bool running_ = false;
    bool changedAnything_ = false;
    std::vector<uninstall::UninstallResult> results_;

    QLabel* statusLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

}  // namespace avel::app
