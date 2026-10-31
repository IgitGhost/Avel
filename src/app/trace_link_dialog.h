#pragma once

#include <QDialog>
#include <QString>
#include <QThreadPool>

#include <memory>

#include "core/inventory/inventory.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace avel::app {

// "Link to program…": picks the installed program a trace is linked to by hand (`trace link`).
// Packaged apps aren't offered: Avel doesn't trace them.
class TraceLinkDialog : public QDialog {
    Q_OBJECT

public:
    // `inventory`: what is installed; null to scan it here, off the GUI thread.
    TraceLinkDialog(std::shared_ptr<const inventory::Inventory> inventory, const QString& traceName,
                    QWidget* parent = nullptr);
    ~TraceLinkDialog() override;

    // After accept(): the chosen program, which inventory() keeps alive.
    const inventory::Program* program() const { return program_; }
    std::shared_ptr<const inventory::Inventory> inventory() const { return inventory_; }

    void accept() override;

private:
    void setInventory(std::shared_ptr<const inventory::Inventory> inventory);
    void applyFilter(const QString& text);
    void updateButtons();

    std::shared_ptr<const inventory::Inventory> inventory_;
    const inventory::Program* program_ = nullptr;

    QLabel* statusLabel_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTreeWidget* programs_ = nullptr;
    QPushButton* linkButton_ = nullptr;
    // Declared last so it's destroyed first: its destructor waits for the scan.
    QThreadPool pool_;
};

}  // namespace avel::app
