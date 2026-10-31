#pragma once

#include <QDialog>
#include <QString>
#include <QThreadPool>

#include <memory>

#include "core/inventory/inventory.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_review.h"
#include "core/tracker/trace_store.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeView;

namespace avel::app {

class TraceModel;
class TraceStoreAccess;

// The review of a finished trace: what the installer changed, grouped by whether it is tied to
// the program, with the noise behind a toggle. Unchecking an item excludes it from removing the
// program by its trace; the exclusions are saved to the trace's review.json as they're made.
// "Link to program…" links the trace by hand.
class TraceReviewDialog : public QDialog {
    Q_OBJECT

public:
    // `traces` must outlive the dialog.
    TraceReviewDialog(TraceStoreAccess* traces, const QString& traceId, QWidget* parent = nullptr);
    ~TraceReviewDialog() override;

private:
    struct Loaded;

    void load();
    void onLoaded(const std::shared_ptr<const Loaded>& loaded);
    void saveReview();
    void onSaved(tracker::TraceError error);
    void linkToProgram();
    void onLinked(tracker::TraceError error);
    void updateCounts();
    void expandDefault();
    void copySelection();
    void setBusy(bool busy);

    TraceStoreAccess* traces_ = nullptr;
    std::shared_ptr<tracker::TraceStore> store_;
    QString id_;
    std::shared_ptr<const inventory::Inventory> inventory_;  // Scanned with the trace
    std::shared_ptr<const tracker::TraceResult> result_;
    QString traceName_;
    bool loaded_ = false;
    bool busy_ = false;
    bool saving_ = false;
    bool savePending_ = false;

    TraceModel* model_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* noteLabel_ = nullptr;
    QTreeView* tree_ = nullptr;
    QCheckBox* noiseBox_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* saveLabel_ = nullptr;
    QPushButton* linkButton_ = nullptr;
    // Declared last so it's destroyed first: its destructor waits for the worker.
    QThreadPool pool_;
};

}  // namespace avel::app
