#include "traces_dialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <chrono>
#include <optional>

#include "core/changes/serialization.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/linking.h"
#include "core/tracker/trace_export.h"
#include "program_text.h"
#include "trace_controller.h"
#include "trace_link_dialog.h"
#include "trace_review_dialog.h"
#include "trace_store_access.h"
#include "trace_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

using tracker::TraceError;
using tracker::TraceState;

enum Column { StartedColumn, DescriptionColumn, ProgramColumn, StateColumn, ModeColumn, ChangesColumn };

constexpr int kIdRole = Qt::UserRole;

bool isFinished(TraceState state) { return state == TraceState::Complete || state == TraceState::Incomplete; }

bool isActive(TraceState state) { return !isFinished(state); }

}  // namespace

TracesDialog::TracesDialog(TraceStoreAccess* traces, TraceController* controller,
                           std::shared_ptr<const inventory::Inventory> inventory, QWidget* parent)
    : QDialog(parent),
      traces_(traces),
      controller_(controller),
      store_(traces->store()),
      inventory_(std::move(inventory)) {
    setObjectName(u"tracesDialog"_s);
    setWindowTitle(tr("Traces"));
    pool_.setMaxThreadCount(1);

    introLabel_ = new QLabel(this);
    introLabel_->setWordWrap(true);

    list_ = new QTreeWidget(this);
    list_->setObjectName(u"tracesList"_s);
    list_->setAccessibleName(tr("Traces"));
    list_->setColumnCount(6);
    list_->setHeaderLabels(
        {tr("Started"), tr("Description"), tr("Program"), tr("State"), tr("Mode"), tr("Changes")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setAllColumnsShowFocus(true);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(DescriptionColumn, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(ProgramColumn, QHeaderView::Interactive);
    list_->setColumnWidth(ProgramColumn, 240);
    connect(list_, &QTreeWidget::currentItemChanged, this, &TracesDialog::showSelected);
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &TracesDialog::showSelected);
    connect(list_, &QTreeWidget::itemDoubleClicked, this, &TracesDialog::showReview);

    detailsLabel_ = new QLabel(this);
    detailsLabel_->setObjectName(u"traceDetails"_s);
    detailsLabel_->setWordWrap(true);
    detailsLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    detailsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(list_);
    splitter->addWidget(detailsLabel_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto* buttons = new QDialogButtonBox(this);
    const auto addButton = [buttons](const QString& text, const QString& name, const QString& tip) {
        QPushButton* button = buttons->addButton(text, QDialogButtonBox::ActionRole);
        button->setObjectName(name);
        button->setToolTip(tip);
        return button;
    };
    showButton_ = addButton(tr("&Show…"), u"showTraceButton"_s, tr("Review what the trace recorded"));
    linkButton_ =
        addButton(tr("&Link…"), u"linkTraceButton"_s, tr("Link the trace to the program it installed"));
    exportButton_ = addButton(tr("&Export…"), u"exportTraceButton"_s, tr("Write the trace to a file"));
    importButton_ = addButton(tr("&Import…"), u"importTraceButton"_s, tr("Add a trace exported elsewhere"));
    finishButton_ = addButton(tr("&Finish"), u"finishTraceButton"_s, tr("Stop the open trace and review it"));
    deleteButton_ = addButton(tr("&Delete…"), u"deleteTraceButton"_s, {});
    revertButton_ = addButton(tr("&Revert by trace…"), u"revertTraceButton"_s,
                              tr("Remove what the trace recorded with Avel Force, without the program's "
                                 "uninstaller"));
    QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
    close->setObjectName(u"closeTracesButton"_s);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(showButton_, &QPushButton::clicked, this, &TracesDialog::showReview);
    connect(linkButton_, &QPushButton::clicked, this, &TracesDialog::link);
    connect(exportButton_, &QPushButton::clicked, this, &TracesDialog::exportTrace);
    connect(importButton_, &QPushButton::clicked, this, &TracesDialog::importTrace);
    connect(finishButton_, &QPushButton::clicked, this, &TracesDialog::finishTrace);
    connect(deleteButton_, &QPushButton::clicked, this, &TracesDialog::deleteTrace);
    connect(revertButton_, &QPushButton::clicked, this, &TracesDialog::revert);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(introLabel_);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);
    resize(1100, 640);

    if (!store_) {
        introLabel_->setText(traces_->unavailableReason());
        splitter->setEnabled(false);
        updateButtons();
        return;
    }
    introLabel_->setText(
        tr("Avel keeps a trace for as long as the program it installed is installed, and 30 days "
           "after it's removed; a trace that isn't linked to a program, for 30 days."));
    connect(traces_, &TraceStoreAccess::tracesChanged, this, &TracesDialog::reload);
    connect(controller_, &TraceController::stateChanged, this, &TracesDialog::reload);
    reload();
}

TracesDialog::~TracesDialog() { pool_.waitForDone(); }

void TracesDialog::reload() {
    if (!store_) {
        return;
    }
    if (loading_) {
        reloadAgain_ = true;
        return;
    }
    loading_ = true;
    pool_.start([this, store = store_] {
        std::vector<Listed> traces;
        for (auto& info : store->list()) {
            const bool held = isActive(info.state) && store->held(info.id);
            traces.push_back({std::move(info), held});
        }
        QMetaObject::invokeMethod(
            this, [this, traces = std::move(traces)]() mutable { onLoaded(std::move(traces)); },
            Qt::QueuedConnection);
    });
}

void TracesDialog::onLoaded(std::vector<Listed> traces) {
    loading_ = false;
    if (selectId_.isEmpty()) {
        if (const Listed* current = selected()) {
            selectId_ = toQString(current->info.id);
        }
    }
    listed_ = std::move(traces);
    // The trace this process records isn't held by another process.
    const QString driven = controller_->drivenTraceId();
    for (auto& trace : listed_) {
        trace.heldElsewhere = trace.heldElsewhere && toQString(trace.info.id) != driven;
    }

    const QSignalBlocker blocker(list_);
    list_->clear();
    QTreeWidgetItem* select = nullptr;
    for (std::size_t i = 0; i < listed_.size(); ++i) {
        const auto& [info, heldElsewhere] = listed_[i];
        const QString id = toQString(info.id);
        auto* row = new QTreeWidgetItem(list_);
        row->setData(StartedColumn, kIdRole, static_cast<qulonglong>(i));
        row->setText(StartedColumn, TraceText::timeText(info.started));
        const QString description = toQString(info.description.empty() ? info.command : info.description);
        row->setText(DescriptionColumn, description);
        row->setToolTip(DescriptionColumn, description);
        if (!info.links.empty()) {
            QString program = TraceText::linkText(info.links.front());
            if (info.unresolved_since) {
                program = tr("%1 (uninstalled)").arg(program);
            }
            row->setText(ProgramColumn, program);
            row->setToolTip(ProgramColumn, program);
        }
        QString state = TraceText::stateText(info.state);
        if (isActive(info.state)) {
            if (id == driven) {
                state = tr("%1: being recorded").arg(state);
            } else if (heldElsewhere) {
                state = tr("%1: in use by another Avel process").arg(state);
            } else if (info.stop_reason == tracker::StopReason::RebootPending) {
                state = tr("%1: restart Windows, then finish it").arg(state);
            }
        } else if (info.imported) {
            state = tr("%1, imported").arg(state);
        }
        row->setText(StateColumn, state);
        if (info.state == TraceState::Incomplete) {
            QStringList reasons;
            for (const auto& reason : tracker::incomplete_reasons(info)) {
                reasons << TraceText::incompleteText(reason);
            }
            row->setToolTip(StateColumn, reasons.join(u'\n'));
            row->setForeground(StateColumn, QColor(0xC0, 0x70, 0x20));
        }
        row->setText(ModeColumn, TraceText::modeText(info.mode));
        if (isFinished(info.state)) {
            row->setText(ChangesColumn, QLocale().toString(static_cast<qulonglong>(info.counts.changes)));
            row->setToolTip(ChangesColumn,
                            tr("%1 set aside as noise")
                                .arg(QLocale().toString(static_cast<qulonglong>(info.counts.noise))));
            row->setTextAlignment(ChangesColumn, Qt::AlignRight | Qt::AlignVCenter);
        }
        if (id == selectId_ || (selectId_.isEmpty() && select == nullptr)) {
            select = row;
        }
    }
    selectId_.clear();
    if (select == nullptr && list_->topLevelItemCount() > 0) {
        select = list_->topLevelItem(0);
    }
    list_->setCurrentItem(select);
    if (listed_.empty()) {
        detailsLabel_->setText(
            tr("No traces yet. Trace > Install and trace… records what an installer changes."));
    }
    showSelected();
    if (reloadAgain_) {
        reloadAgain_ = false;
        reload();
    }
}

const TracesDialog::Listed* TracesDialog::selected() const {
    // The selected row: UI Automation selects a row without making it current.
    const auto items = list_->selectedItems();
    const QTreeWidgetItem* item = items.isEmpty() ? list_->currentItem() : items.front();
    if (item == nullptr) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(item->data(StartedColumn, kIdRole).toULongLong());
    return index < listed_.size() ? &listed_[index] : nullptr;
}

void TracesDialog::showSelected() {
    if (const Listed* trace = selected()) {
        QStringList lines = TraceText::summary(trace->info);
        lines.prepend(tr("Trace %1").arg(toQString(trace->info.id)));
        detailsLabel_->setText(lines.join(u'\n'));
    } else if (!listed_.empty()) {
        detailsLabel_->clear();
    }
    updateButtons();
}

void TracesDialog::updateButtons() {
    const Listed* trace = store_ ? selected() : nullptr;
    const bool finished = trace && isFinished(trace->info.state);
    const bool open = trace && !finished;
    const bool controllerFree = !controller_->busy();
    const QString id = trace ? toQString(trace->info.id) : QString();
    const bool ownTrace =
        controller_->state() == TraceController::State::Tracing && controller_->drivenTraceId() == id;
    const bool othersFree = controller_->state() == TraceController::State::Idle || ownTrace;
    const bool idle = !busy_;
    showButton_->setEnabled(idle && finished);
    linkButton_->setEnabled(idle && finished);
    exportButton_->setEnabled(idle && finished);
    revertButton_->setEnabled(idle && finished);
    importButton_->setEnabled(idle && store_ != nullptr);
    finishButton_->setEnabled(idle && open && controllerFree && othersFree &&
                              trace->info.state != TraceState::Starting && !trace->heldElsewhere);
    deleteButton_->setEnabled(idle && trace != nullptr &&
                              (finished || (controllerFree && othersFree && !trace->heldElsewhere)));
    deleteButton_->setText(open ? tr("&Discard…") : tr("&Delete…"));
    deleteButton_->setToolTip(open ? tr("Discard the open trace; what it recorded is lost")
                                   : tr("Delete the trace; removing its program can't use it afterwards"));
    if (trace && open && trace->heldElsewhere) {
        finishButton_->setToolTip(
            tr("Another Avel process is recording this trace (avel-cli or another Avel "
               "window). Finish it there."));
    } else {
        finishButton_->setToolTip(tr("Stop the open trace and review it"));
    }
}

void TracesDialog::showReview() {
    const Listed* trace = selected();
    if (busy_ || !trace || !isFinished(trace->info.state)) {
        return;
    }
    auto* review = new TraceReviewDialog(traces_, toQString(trace->info.id), this);
    review->setAttribute(Qt::WA_DeleteOnClose);
    review->open();
}

void TracesDialog::link() {
    const Listed* trace = selected();
    if (busy_ || !trace || !isFinished(trace->info.state)) {
        return;
    }
    const QString id = toQString(trace->info.id);
    auto* picker = new TraceLinkDialog(inventory_, TraceText::traceName(trace->info), this);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &QDialog::accepted, this, [this, picker, id] {
        const auto* program = picker->program();
        if (program == nullptr) {
            return;
        }
        inventory_ = picker->inventory();
        selectId_ = id;
        setBusy(true);
        pool_.start([this, store = store_, id = id.toStdWString(), inventory = inventory_, program] {
            TraceError error = TraceError::None;
            store->update(
                id,
                [program](tracker::TraceInfo& info) {
                    tracker::add_manual_link(info, *program);
                    return true;
                },
                error);
            QMetaObject::invokeMethod(
                this,
                [this, error] {
                    setBusy(false);
                    if (error != TraceError::None) {
                        showMessage(tr("Link the trace"),
                                    tr("The trace couldn't be linked: %1.").arg(TraceText::errorText(error)),
                                    {}, true);
                    }
                    traces_->notifyChanged();
                },
                Qt::QueuedConnection);
        });
    });
    picker->open();
}

void TracesDialog::exportTrace() {
    const Listed* trace = selected();
    if (busy_ || !trace || !isFinished(trace->info.state)) {
        return;
    }
    const QString id = toQString(trace->info.id);
    QString name = TraceText::traceName(trace->info);
    for (const QChar bad : u"\\/:*?\"<>|"_s) {
        name.replace(bad, u'_');
    }
    auto* dialog = new QFileDialog(this, tr("Export the trace"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setNameFilters({tr("Avel traces (*.json)"), tr("All files (*)")});
    dialog->setDefaultSuffix(u"json"_s);
    dialog->selectFile(name + u".trace.json"_s);
    connect(dialog, &QFileDialog::fileSelected, this, [this, id](const QString& selectedFile) {
        const QString file = QDir::toNativeSeparators(selectedFile);
        setBusy(true);
        pool_.start([this, store = store_, id = id.toStdWString(), file] {
            TraceError error = TraceError::None;
            const auto trace = tracker::read_export(*store, id, platform::enumerate_user_profiles(), error);
            bool written = false;
            std::size_t environment = 0;
            if (trace) {
                const std::string text =
                    tracker::to_json(*trace, std::chrono::system_clock::now()).dump(1, '\t');
                written = changes::write_file_durably(file.toStdWString(), text, true);
                environment = tracker::environment_values(*trace);
            }
            QMetaObject::invokeMethod(
                this,
                [this, file, error, exported = trace.has_value(), written, environment] {
                    setBusy(false);
                    const QString title = tr("Export the trace");
                    if (!exported) {
                        showMessage(title,
                                    tr("The trace can't be exported: %1.").arg(TraceText::errorText(error)),
                                    {}, true);
                    } else if (!written) {
                        showMessage(title, tr("%1 couldn't be written.").arg(file), {}, true);
                    } else if (environment > 0) {
                        showMessage(
                            title, tr("The trace was exported to %1.").arg(file),
                            tr("The file includes %n environment value(s) as they were on this machine: "
                               "PATH and the other variables the installer changed, before and after. "
                               "Share it only where those may be seen.",
                               nullptr, static_cast<int>(environment)),
                            true);
                    } else {
                        showMessage(title, tr("The trace was exported to %1.").arg(file));
                    }
                },
                Qt::QueuedConnection);
        });
    });
    dialog->open();
}

void TracesDialog::importTrace() {
    if (busy_ || !store_) {
        return;
    }
    auto* dialog = new QFileDialog(this, tr("Import a trace"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilters({tr("Avel traces (*.json)"), tr("All files (*)")});
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& selectedFile) {
        const QString file = QDir::toNativeSeparators(selectedFile);
        setBusy(true);
        pool_.start([this, store = store_, file] {
            struct Result {
                bool read = false;
                tracker::ImportError parsed = tracker::ImportError::None;
                int version = 0;
                std::optional<tracker::TraceInfo> info;
                TraceError error = TraceError::None;
            } result;
            if (const auto text = changes::read_file(file.toStdWString())) {
                result.read = true;
                tracker::TraceExport trace;
                result.parsed = tracker::parse_export(*text, trace, result.version);
                if (result.parsed == tracker::ImportError::None) {
                    inventory::ScanOptions scan;
                    scan.appx = false;  // Avel doesn't trace packaged apps
                    const auto inventory = inventory::scan_inventory(scan);
                    result.info = tracker::import_trace(*store, std::move(trace), inventory,
                                                        tracker::live_import_options(), result.error);
                    if (result.info && !inventory.programs.empty()) {
                        store->purge([&inventory](tracker::TraceInfo& info) {
                            return tracker::refresh_links(info, inventory);
                        });
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, file, result = std::move(result)] {
                    setBusy(false);
                    const QString title = tr("Import a trace");
                    if (!result.read) {
                        showMessage(title, tr("%1 can't be read. Nothing was imported.").arg(file), {}, true);
                        return;
                    }
                    if (result.parsed != tracker::ImportError::None) {
                        showMessage(title, tr("%1 can't be imported.").arg(file),
                                    TraceText::importErrorText(result.parsed, result.version), true);
                        return;
                    }
                    if (!result.info) {
                        showMessage(
                            title,
                            tr("The trace couldn't be added: %1.").arg(TraceText::errorText(result.error)),
                            {}, true);
                        return;
                    }
                    QStringList notes;
                    for (const auto& diagnostic : result.info->diagnostics) {
                        if (diagnostic.code == tracker::TraceDiagnosticCode::UserNotRemapped) {
                            notes << tr("The items of user %1 are left as they were recorded: only a trace "
                                        "of a "
                                        "single user whose profile folder is known is moved to you.")
                                         .arg(toQString(diagnostic.subject));
                        }
                    }
                    selectId_ = toQString(result.info->id);
                    traces_->notifyChanged();
                    showMessage(title,
                                tr("The trace of %1 was imported.").arg(TraceText::traceName(*result.info)),
                                notes.join(u'\n'));
                },
                Qt::QueuedConnection);
        });
    });
    dialog->open();
}

void TracesDialog::deleteTrace() {
    const Listed* trace = selected();
    if (busy_ || !trace) {
        return;
    }
    const QString id = toQString(trace->info.id);
    if (!isFinished(trace->info.state)) {
        controller_->discardTrace(id);
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Warning, tr("Delete the trace"),
                                tr("Delete the trace of %1? Removing the program can't use it afterwards.")
                                    .arg(TraceText::traceName(trace->info)),
                                QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(u"deleteTraceQuestion"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::finished, this, [this, id](int answer) {
        if (answer != QMessageBox::Yes || busy_) {
            return;
        }
        setBusy(true);
        pool_.start([this, store = store_, id = id.toStdWString()] {
            TraceError error = TraceError::None;
            const bool removed = store->remove(id, error);
            QMetaObject::invokeMethod(
                this,
                [this, removed, error] {
                    setBusy(false);
                    if (!removed) {
                        showMessage(tr("Delete the trace"),
                                    tr("The trace couldn't be deleted: %1.").arg(TraceText::errorText(error)),
                                    {}, true);
                    }
                    traces_->notifyChanged();
                },
                Qt::QueuedConnection);
        });
    });
    box->open();
}

void TracesDialog::finishTrace() {
    const Listed* trace = selected();
    if (busy_ || !trace || isFinished(trace->info.state)) {
        return;
    }
    if (trace->heldElsewhere) {
        showMessage(
            tr("Finish the trace"),
            tr("Another Avel process is recording this trace (avel-cli or another Avel window). Finish it "
               "there, or wait until it's done."));
        return;
    }
    controller_->finishTrace(toQString(trace->info.id));
}

void TracesDialog::revert() {
    const Listed* trace = selected();
    if (busy_ || !trace || !isFinished(trace->info.state)) {
        return;
    }
    Q_EMIT revertRequested(toQString(trace->info.id));
    accept();
}

void TracesDialog::setBusy(bool busy) {
    busy_ = busy;
    updateButtons();
}

void TracesDialog::showMessage(const QString& title, const QString& text, const QString& details,
                               bool warning) {
    auto* box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, title, text,
                                QMessageBox::Ok, this);
    box->setObjectName(u"tracesMessage"_s);
    box->setAttribute(Qt::WA_DeleteOnClose);
    if (!details.isEmpty()) {
        box->setInformativeText(details);
    }
    box->open();
}

}  // namespace avel::app
