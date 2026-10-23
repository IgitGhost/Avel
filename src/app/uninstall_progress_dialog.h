#pragma once

#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/uninstall/batch.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace avel::app {

class ChangeStoreAccess;

// Runs an uninstall batch on a worker thread and shows its progress. Can't be closed while an
// uninstaller runs; "Stop" ends the batch after the current program, or terminates it. Then it
// looks for leftovers, if asked to, and opens the leftover review when it finds some. Programs
// that failed to uninstall, or couldn't, are offered to Avel Force.
class UninstallProgressDialog : public QDialog {
    Q_OBJECT

public:
    // `inventory` keeps the programs the items point to alive. `stores` must outlive the dialog.
    UninstallProgressDialog(std::shared_ptr<const inventory::Inventory> inventory,
                            std::vector<uninstall::BatchItem> items, uninstall::BatchOptions options,
                            bool scanLeftovers, ChangeStoreAccess* stores, QWidget* parent = nullptr);
    ~UninstallProgressDialog() override;

    // Whether any uninstaller ran, so the program list needs refreshing.
    bool changedAnything() const { return changedAnything_; }

    void reject() override;

Q_SIGNALS:
    // "Force removal…": the ids of the programs Avel Force should remove. The dialog closes
    // right after.
    void forceRequested(const std::vector<std::wstring>& programIds);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void start();
    void onRestorePointDone(uninstall::RestorePointStatus status, std::uint32_t error);
    bool askContinueWithoutRestorePoint(std::uint32_t error);
    void onItemStarted(std::size_t index);
    void onItemFinished(std::size_t index, const uninstall::UninstallResult& result);
    void onScanStarted();
    void onBatchFinished(std::shared_ptr<const leftovers::ScanResult> scan);
    void showLeftovers();
    void requestStop();
    void setStatus(int row, const QString& text);

    std::shared_ptr<const inventory::Inventory> inventory_;
    std::vector<uninstall::BatchItem> items_;
    uninstall::BatchOptions options_;
    bool scanLeftovers_ = false;
    ChangeStoreAccess* stores_ = nullptr;
    // Captured by the worker before the batch runs; read here once it's done.
    std::vector<leftovers::ScanTarget> targets_;
    std::shared_ptr<const leftovers::ScanResult> scan_;
    std::vector<std::wstring> forceIds_;
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
    QPushButton* leftoversButton_ = nullptr;
    QPushButton* forceButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

}  // namespace avel::app
