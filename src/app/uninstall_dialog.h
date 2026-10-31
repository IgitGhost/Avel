#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "core/inventory/program.h"
#include "core/uninstall/batch.h"
#include "core/uninstall/uninstall_plan.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace avel::app {

class ProgramTraces;

// Confirms an uninstall: shows how each selected program will be removed and offers silent mode,
// a System Restore point, a leftover scan afterwards (by the programs' install traces, when they
// have one) and a dry run. Programs that can't uninstall themselves (their uninstaller is missing,
// say) can go to Avel Force from here instead.
class UninstallDialog : public QDialog {
    Q_OBJECT

public:
    // The programs must outlive the dialog and the batch it configures. `traces` (may be null)
    // tells which programs were installed with Avel Trace.
    UninstallDialog(std::vector<const inventory::Program*> programs, const ProgramTraces* traces,
                    QWidget* parent = nullptr);

    // The programs with their plans for the chosen mode.
    const std::vector<uninstall::BatchItem>& items() const { return items_; }
    uninstall::BatchOptions options() const;
    // Look for what the uninstallers left behind once they're done (never after a dry run).
    bool scanLeftovers() const;
    // The leftover scan uses the programs' install traces, as avel-cli uninstall does unless
    // --no-trace: "Use the install trace", on by default.
    bool useTraces() const;

    void accept() override;

Q_SIGNALS:
    // "Force uninstall…": the ids of the programs Avel Force should remove. The dialog closes
    // (rejected) right after.
    void forceRequested(const std::vector<std::wstring>& programIds);

private:
    void updatePlans();

    std::vector<const inventory::Program*> programs_;
    std::vector<uninstall::BatchItem> items_;
    std::vector<std::wstring> forceIds_;  // The programs that can't be planned, but forced
    uninstall::PlanContext context_;

    QLabel* intro_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QCheckBox* quietBox_ = nullptr;
    QCheckBox* restorePointBox_ = nullptr;
    QCheckBox* leftoverScanBox_ = nullptr;
    QCheckBox* traceBox_ = nullptr;  // Shown when a program has an install trace
    QCheckBox* simulateBox_ = nullptr;
    QPushButton* okButton_ = nullptr;
    QPushButton* forceButton_ = nullptr;
};

}  // namespace avel::app
