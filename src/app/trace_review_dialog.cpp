#include "trace_review_dialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTreeView>
#include <QVBoxLayout>

#include "core/tracker/linking.h"
#include "core/tracker/trace_items.h"
#include "core/tracker/trace_source.h"
#include "program_text.h"
#include "right_elide_delegate.h"
#include "trace_link_dialog.h"
#include "trace_model.h"
#include "trace_store_access.h"
#include "trace_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

// Groups and kinds with up to this many items open by themselves.
constexpr int kExpandLimit = 50;

bool isFinished(tracker::TraceState state) {
    return state == tracker::TraceState::Complete || state == tracker::TraceState::Incomplete;
}

}  // namespace

// What the worker reads: the trace, its result and review, and the items tied to its program as
// it is now (or as the trace last knew it, once it's gone).
struct TraceReviewDialog::Loaded {
    std::optional<tracker::TraceInfo> info;
    std::shared_ptr<const tracker::TraceResult> result;
    std::optional<tracker::TraceReview> review;  // nullopt: review.json is damaged
    std::vector<tracker::TiedItem> tied;
    std::shared_ptr<const inventory::Inventory> inventory;
    std::wstring program;  // The installed program's name; empty when none answers to the links
};

TraceReviewDialog::TraceReviewDialog(TraceStoreAccess* traces, const QString& traceId, QWidget* parent)
    : QDialog(parent), traces_(traces), store_(traces->store()), id_(traceId) {
    setObjectName(u"traceReviewDialog"_s);
    setWindowTitle(tr("Trace review"));
    pool_.setMaxThreadCount(1);

    auto* intro = new QLabel(
        tr("What the installation changed. Checked items are the program's: when Avel removes the program "
           "by this trace, it offers them. Uncheck what isn't the program's, and Avel won't offer it. "
           "Changes to items that existed before, and noise, are shown only."),
        this);
    intro->setWordWrap(true);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName(u"traceSummary"_s);
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    noteLabel_ = new QLabel(this);
    noteLabel_->setWordWrap(true);
    noteLabel_->hide();

    noiseBox_ = new QCheckBox(tr("Show noise"), this);
    noiseBox_->setObjectName(u"showNoiseBox"_s);
    noiseBox_->setToolTip(
        tr("What Windows, the user or Avel changed while the trace ran. It's never offered for removal."));

    model_ = new TraceModel(this);
    tree_ = new QTreeView(this);
    tree_->setObjectName(u"traceReviewTree"_s);
    tree_->setAccessibleName(tr("Traced items"));
    tree_->setModel(model_);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setAllColumnsShowFocus(true);
    tree_->setTextElideMode(Qt::ElideMiddle);
    tree_->setItemDelegateForColumn(TraceModel::DetailsColumn, new RightElideDelegate(tree_));
    tree_->header()->setStretchLastSection(true);
    tree_->header()->resizeSection(TraceModel::ItemColumn, 480);
    tree_->header()->resizeSection(TraceModel::TypeColumn, 150);
    tree_->header()->resizeSection(TraceModel::ChangeColumn, 170);

    auto* copyAction = new QAction(tr("&Copy"), tree_);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setShortcutContext(Qt::WidgetShortcut);
    connect(copyAction, &QAction::triggered, this, &TraceReviewDialog::copySelection);
    tree_->addAction(copyAction);
    tree_->setContextMenuPolicy(Qt::ActionsContextMenu);

    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(u"traceCounts"_s);
    countLabel_->setWordWrap(true);
    saveLabel_ = new QLabel(this);
    saveLabel_->setObjectName(u"reviewSaveStatus"_s);
    saveLabel_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(this);
    linkButton_ = buttons->addButton(tr("&Link to program…"), QDialogButtonBox::ActionRole);
    linkButton_->setObjectName(u"linkToProgramButton"_s);
    linkButton_->setToolTip(tr("Choose the program that was installed during this trace"));
    QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
    close->setObjectName(u"closeReviewButton"_s);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(linkButton_, &QPushButton::clicked, this, &TraceReviewDialog::linkToProgram);

    auto* options = new QHBoxLayout;
    options->addWidget(countLabel_, 1);
    options->addWidget(noiseBox_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(summaryLabel_);
    layout->addWidget(noteLabel_);
    layout->addWidget(tree_, 1);
    layout->addLayout(options);
    layout->addWidget(saveLabel_);
    layout->addWidget(buttons);
    resize(1100, 700);

    connect(noiseBox_, &QCheckBox::toggled, this, [this](bool show) {
        model_->setShowNoise(show);
        expandDefault();
        updateCounts();
    });
    connect(model_, &TraceModel::reviewChanged, this, [this] {
        updateCounts();
        saveReview();
    });

    if (!store_) {
        summaryLabel_->setText(traces_->unavailableReason());
        tree_->setEnabled(false);
        noiseBox_->setEnabled(false);
        linkButton_->setEnabled(false);
        return;
    }
    load();
}

TraceReviewDialog::~TraceReviewDialog() { pool_.waitForDone(); }

void TraceReviewDialog::load() {
    setBusy(true);
    if (!loaded_) {
        summaryLabel_->setText(tr("Reading the trace…"));
    }
    pool_.start([this, store = store_, id = id_.toStdWString()] {
        auto loaded = std::make_shared<Loaded>();
        loaded->info = store->read(id);
        if (loaded->info && isFinished(loaded->info->state)) {
            if (auto result = store->read_result(id)) {
                loaded->result = std::make_shared<const tracker::TraceResult>(std::move(*result));
                loaded->review = store->read_review(id);
                inventory::ScanOptions scan;
                scan.appx = false;  // Avel doesn't trace packaged apps
                auto inventory =
                    std::make_shared<const inventory::Inventory>(inventory::scan_inventory(scan));
                // Tied as removing the program would tie them: to it while it's installed,
                // otherwise to what the trace's link last recorded of it.
                const auto resolved = tracker::resolve_trace_program(*loaded->info, *inventory);
                const tracker::ProgramPlaces places = resolved ? tracker::program_places(*resolved->program)
                                                               : tracker::recorded_places(*loaded->info);
                if (resolved) {
                    loaded->program = resolved->program->display_name;
                }
                loaded->tied = tracker::tie_trace_items(*loaded->result, places);
                loaded->inventory = std::move(inventory);
            }
        }
        QMetaObject::invokeMethod(
            this, [this, loaded = std::shared_ptr<const Loaded>(std::move(loaded))] { onLoaded(loaded); },
            Qt::QueuedConnection);
    });
}

void TraceReviewDialog::onLoaded(const std::shared_ptr<const Loaded>& loaded) {
    setBusy(false);
    if (!loaded->info) {
        summaryLabel_->setText(tr("This trace doesn't exist anymore."));
        tree_->setEnabled(false);
        linkButton_->setEnabled(false);
        return;
    }
    const auto& info = *loaded->info;
    traceName_ = TraceText::traceName(info);
    setWindowTitle(tr("Trace review: %1").arg(traceName_));
    summaryLabel_->setText(TraceText::summary(info).join(u'\n'));
    if (!isFinished(info.state)) {
        noteLabel_->setText(tr("The trace isn't finished yet. What it recorded is known once it's stopped."));
        noteLabel_->show();
        linkButton_->setEnabled(false);
        return;
    }
    if (!loaded->result) {
        noteLabel_->setText(tr("What the trace recorded can't be read: its files are damaged."));
        noteLabel_->show();
        linkButton_->setEnabled(false);
        return;
    }
    loaded_ = true;
    setBusy(false);
    result_ = loaded->result;
    inventory_ = loaded->inventory;
    QStringList notes;
    if (loaded->program.empty()) {
        notes << (info.links.empty()
                      ? tr("The trace isn't linked to a program, so items are tied by its description only. "
                           "Link "
                           "it to the program it installed if that program is still installed.")
                      : tr("No installed program answers to the trace's links, so items are tied to what the "
                           "trace last knew of its program."));
    }
    if (!loaded->review) {
        notes << tr(
            "The choices made earlier in this review can't be read, so everything starts checked. "
            "Until you change a check, removing the program doesn't use this trace.");
    }
    noteLabel_->setText(notes.join(u'\n'));
    noteLabel_->setVisible(!notes.isEmpty());
    const QString program = loaded->program.empty() ? traceName_ : toQString(loaded->program);
    model_->setTrace(result_, loaded->tied, loaded->review.value_or(tracker::TraceReview{}), program);
    model_->setShowNoise(noiseBox_->isChecked());
    noiseBox_->setText(tr("Show noise (%1)").arg(QLocale().toString(model_->noiseCount())));
    noiseBox_->setEnabled(model_->noiseCount() > 0);
    expandDefault();
    updateCounts();
}

void TraceReviewDialog::saveReview() {
    if (saving_) {
        savePending_ = true;
        return;
    }
    saving_ = true;
    saveLabel_->setText(tr("Saving your choices…"));
    pool_.start([this, store = store_, id = id_.toStdWString(), review = model_->review()] {
        tracker::TraceError error = tracker::TraceError::None;
        store->write_review(id, review, error);
        QMetaObject::invokeMethod(this, [this, error] { onSaved(error); }, Qt::QueuedConnection);
    });
}

void TraceReviewDialog::onSaved(tracker::TraceError error) {
    saving_ = false;
    if (savePending_) {
        savePending_ = false;
        saveReview();
        return;
    }
    if (error == tracker::TraceError::None) {
        saveLabel_->setText(tr("Your choices are saved."));
        traces_->notifyChanged();
    } else {
        saveLabel_->setText(tr("Your choices couldn't be saved: %1.").arg(TraceText::errorText(error)));
    }
}

void TraceReviewDialog::linkToProgram() {
    if (!loaded_ || busy_) {
        return;
    }
    auto* picker = new TraceLinkDialog(inventory_, traceName_, this);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &QDialog::accepted, this, [this, picker] {
        const auto* program = picker->program();
        if (program == nullptr) {
            return;
        }
        setBusy(true);
        pool_.start(
            [this, store = store_, id = id_.toStdWString(), inventory = picker->inventory(), program] {
                tracker::TraceError error = tracker::TraceError::None;
                store->update(
                    id,
                    [program](tracker::TraceInfo& info) {
                        tracker::add_manual_link(info, *program);
                        return true;
                    },
                    error);
                QMetaObject::invokeMethod(this, [this, error] { onLinked(error); }, Qt::QueuedConnection);
            });
    });
    picker->open();
}

void TraceReviewDialog::onLinked(tracker::TraceError error) {
    setBusy(false);
    if (error != tracker::TraceError::None) {
        auto* box = new QMessageBox(QMessageBox::Warning, tr("Link to program"),
                                    tr("The trace couldn't be linked: %1.").arg(TraceText::errorText(error)),
                                    QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
        return;
    }
    traces_->notifyChanged();
    load();  // Its items are tied to the new program now
}

void TraceReviewDialog::updateCounts() {
    if (!loaded_) {
        countLabel_->clear();
        return;
    }
    const int items = model_->itemCount();
    const int excluded = model_->excludedCount();
    QString text = tr("%n item(s) shown.", nullptr, items);
    text += u' ' + (excluded > 0 ? tr("%n excluded: removing the program by this trace won't offer them.",
                                      nullptr, excluded)
                                 : tr("Nothing is excluded."));
    countLabel_->setText(text);
}

void TraceReviewDialog::expandDefault() {
    tree_->collapseAll();
    for (int row = 0; row < model_->rowCount(); ++row) {
        const QModelIndex group = model_->index(row, 0);
        tree_->expand(group);
        for (int child = 0; child < model_->rowCount(group); ++child) {
            const QModelIndex kind = model_->index(child, 0, group);
            if (kind.data(TraceModel::ItemCountRole).toInt() <= kExpandLimit) {
                tree_->expand(kind);
            }
        }
    }
}

void TraceReviewDialog::copySelection() {
    QStringList lines;
    for (const QModelIndex& index : tree_->selectionModel()->selectedRows(TraceModel::ItemColumn)) {
        lines << index.data().toString();
    }
    if (!lines.isEmpty()) {
        QApplication::clipboard()->setText(lines.join(u'\n'));
    }
}

void TraceReviewDialog::setBusy(bool busy) {
    busy_ = busy;
    linkButton_->setEnabled(!busy && loaded_);
    // While it's read again (after linking), the checks wait.
    tree_->setEnabled(!busy || !loaded_);
}

}  // namespace avel::app
