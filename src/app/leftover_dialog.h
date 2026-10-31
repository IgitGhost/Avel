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

    // Scans again with other options, on a worker thread: the deep search, and with or without the
    // programs' install traces (`useTraces`; see withoutTraces).
    using Rescan =
        std::function<leftovers::ScanResult(const leftovers::ScanOptions& options, bool useTraces)>;

    // `removalUnavailable` says why nothing can be removed (no administrator rights, an untrusted
    // backup folder); empty if removal works.
    LeftoverDialog(std::shared_ptr<const leftovers::ScanResult> scan,
                   std::vector<LeftoverModel::Program> programs, Mode mode, const QString& removalUnavailable,
                   QWidget* parent = nullptr);
    ~LeftoverDialog() override;

    // Offers "Search the whole disk", which replaces the scan with `rescan`'s deep one. With
    // `traceToggle`, when the scan used an install trace, also "Use the install trace" ("Revert by
    // the install trace" in Force mode), on: turning it off scans again without the traces, as
    // avel-cli's --no-trace. Call before the dialog opens.
    void setRescan(Rescan rescan, bool traceToggle = false);

    // The targets as if their programs had no install trace: what `rescan` scans without them.
    static std::vector<leftovers::ScanTarget> withoutTraces(std::vector<leftovers::ScanTarget> targets);

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
    // `deep`: the deep search; otherwise again as before, with or without the traces.
    void startRescan(bool deep);
    void onRescanFinished(std::shared_ptr<const leftovers::ScanResult> scan);

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
    bool deep_ = false;  // The whole disk was searched

    LeftoverModel* model_ = nullptr;
    QTreeView* tree_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QCheckBox* unlikelyBox_ = nullptr;
    QCheckBox* traceBox_ = nullptr;
    QLabel* traceLabel_ = nullptr;  // What the install traces brought
    QLabel* diagnosticsLabel_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* deepButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;  // Keep all, or Close in a preview
};

}  // namespace avel::app
