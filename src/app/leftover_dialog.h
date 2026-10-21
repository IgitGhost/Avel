#pragma once

#include <QDialog>
#include <QString>

#include <memory>
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
    };

    // `removalUnavailable` says why nothing can be removed (no administrator rights, an untrusted
    // backup folder); empty if removal works.
    LeftoverDialog(std::shared_ptr<const leftovers::ScanResult> scan,
                   std::vector<LeftoverModel::Program> programs, Mode mode, const QString& removalUnavailable,
                   QWidget* parent = nullptr);

    // After accept(): the checked items, and whether packages may be removed although they can't
    // be put back.
    const changes::ChangePlan& plan() const { return plan_; }
    bool allowIrreversible() const { return allowIrreversible_; }

    void accept() override;

private:
    void updateCounts();
    void expandDefault();
    void expandSmall(const QModelIndex& parent);
    void copySelection();
    void finishAccept();

    std::shared_ptr<const leftovers::ScanResult> scan_;
    Mode mode_;
    QString description_;
    changes::ChangePlan plan_;
    bool allowIrreversible_ = false;
    bool removalAvailable_ = false;

    LeftoverModel* model_ = nullptr;
    QTreeView* tree_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QCheckBox* unlikelyBox_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QPushButton* removeButton_ = nullptr;
};

}  // namespace avel::app
