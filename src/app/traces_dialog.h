#pragma once

#include <QDialog>
#include <QString>
#include <QThreadPool>

#include <memory>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_store.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace avel::app {

class TraceController;
class TraceStoreAccess;

// Trace > Traces…: every trace Avel keeps, newest first, with what to do with one: review it,
// link it to a program, export it to a file (and import one), delete it, finish or discard an open
// one, or revert its program by it with Avel Force.
class TracesDialog : public QDialog {
    Q_OBJECT

public:
    // `traces` and `controller` must outlive the dialog. `inventory` is what the program list
    // shows, for linking by hand; null to scan it when needed.
    TracesDialog(TraceStoreAccess* traces, TraceController* controller,
                 std::shared_ptr<const inventory::Inventory> inventory, QWidget* parent = nullptr);
    ~TracesDialog() override;

Q_SIGNALS:
    // "Revert by trace…": Avel Force should remove what the finished trace recorded, without the
    // program's uninstaller. The dialog closes right after.
    void revertRequested(const QString& traceId);

private:
    struct Listed {
        tracker::TraceInfo info;
        bool heldElsewhere = false;  // Another process drives it
    };

    void reload();
    void onLoaded(std::vector<Listed> traces);
    void showSelected();
    void updateButtons();
    const Listed* selected() const;
    void showReview();
    void link();
    void exportTrace();
    void importTrace();
    void deleteTrace();
    void finishTrace();
    void revert();
    void setBusy(bool busy);
    void showMessage(const QString& title, const QString& text, const QString& details = {},
                     bool warning = false);

    TraceStoreAccess* traces_ = nullptr;
    TraceController* controller_ = nullptr;
    std::shared_ptr<tracker::TraceStore> store_;
    std::shared_ptr<const inventory::Inventory> inventory_;
    std::vector<Listed> listed_;
    QString selectId_;  // Selected once the list is read again
    bool busy_ = false;
    bool loading_ = false;
    bool reloadAgain_ = false;

    QLabel* introLabel_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QLabel* detailsLabel_ = nullptr;
    QPushButton* showButton_ = nullptr;
    QPushButton* linkButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* finishButton_ = nullptr;
    QPushButton* revertButton_ = nullptr;
    // Declared last so it's destroyed first: its destructor waits for the worker.
    QThreadPool pool_;
};

}  // namespace avel::app
