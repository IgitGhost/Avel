#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "core/changes/change_set.h"
#include "core/leftovers/scan.h"
#include "leftover_model.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeView;

namespace avel::app {

// Shows what a leftover scan found, grouped by program and kind, with why each item matched, and
// lets the user choose what to remove. Accepting it yields the plan; the caller runs it.
class LeftoverDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode {
        Review,   // After an uninstall: choose what to remove
        Preview,  // For installed programs: look only
        Force,    // Avel Force: the program itself, whose uninstaller didn't (or can't) run
        Orphans,  // Folders no installed program owns
    };

    // Scans again with other options, on a worker thread: the deep search.
    using Rescan = std::function<leftovers::ScanResult(const leftovers::ScanOptions&)>;

    // `removalUnavailable` says why nothing can be removed (no administrator rights, an untrusted
    // backup folder); empty if removal works.
    LeftoverDialog(std::shared_ptr<const leftovers::ScanResult> scan,
                   std::vector<LeftoverModel::Program> programs, Mode mode, const QString& removalUnavailable,
                   QWidget* parent = nullptr);
    ~LeftoverDialog() override;

    // Offers "Search the whole disk", which replaces the scan with `rescan`'s deep one. Call
    // before the dialog opens.
    void setRescan(Rescan rescan);

    // After accept(): the checked items, and whether packages may be removed although they can't
    // be put back.
    const changes::ChangePlan& plan() const { return plan_; }
    bool allowIrreversible() const { return allowIrreversible_; }

    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void updateScanInfo();
    void updateCounts();
    void expandDefault();
    void expandSmall(const QModelIndex& parent);
    void copySelection();
    void finishAccept();
    void startDeepSearch();
    void onDeepSearchFinished(std::shared_ptr<const leftovers::ScanResult> scan);

    std::shared_ptr<const leftovers::ScanResult> scan_;
    std::vector<LeftoverModel::Program> programs_;
    Mode mode_;
    QString description_;
    changes::ChangePlan plan_;
    bool allowIrreversible_ = false;
    bool removalAvailable_ = false;
    Rescan rescan_;
    std::thread rescanWorker_;
    bool searching_ = false;

    LeftoverModel* model_ = nullptr;
    QTreeView* tree_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QCheckBox* unlikelyBox_ = nullptr;
    QLabel* diagnosticsLabel_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* deepButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;  // Keep all, or Close in a preview
};

}  // namespace avel::app
