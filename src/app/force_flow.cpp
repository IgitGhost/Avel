#include "force_flow.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStringList>
#include <QWidget>

#include <algorithm>

#include "change_set_progress_dialog.h"
#include "change_store_access.h"
#include "core/tracker/trace_source.h"
#include "leftover_dialog.h"
#include "program_text.h"
#include "program_traces.h"
#include "running_processes_dialog.h"
#include "trace_store_access.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

// Up to this many programs are named in the messages.
constexpr std::size_t kNamedPrograms = 3;

}  // namespace

bool forceCanHelp(uninstall::PlanError error) {
    using uninstall::PlanError;
    return error == PlanError::NoUninstaller || error == PlanError::UnsupportedCommand ||
           error == PlanError::UninstallerMissing || error == PlanError::MsiPackageMissing;
}

ForceFlow::ForceFlow(Request request, ChangeStoreAccess* stores, TraceStoreAccess* traces, QWidget* window)
    : QObject(window), request_(std::move(request)), stores_(stores), traces_(traces), window_(window) {
    pool_.setMaxThreadCount(1);
}

ForceFlow::~ForceFlow() { pool_.waitForDone(); }

void ForceFlow::start() {
    Q_EMIT progress(tr("Looking for everything that belongs to the program…"));
    std::shared_ptr<const tracker::TraceStore> traces;
    if (traces_ != nullptr && (request_.useTraces || !request_.traceId.empty())) {
        traces = traces_->store();
    }
    pool_.start([this, request = request_, traces = std::move(traces)] {
        auto result = std::make_shared<Scan>(scan(request, traces));
        QMetaObject::invokeMethod(
            this, [this, result = std::move(result)] { onScanned(result); }, Qt::QueuedConnection);
    });
}

ForceFlow::Scan ForceFlow::scan(const Request& request, std::shared_ptr<const tracker::TraceStore> traces) {
    Scan result;
    // Afresh: the program list the request came from may be older than an uninstall that failed.
    result.inventory = std::make_shared<const inventory::Inventory>(inventory::scan_inventory());
    const leftovers::LiveEnvironment live;
    const auto& environment = live.environment();
    if (!request.traceId.empty()) {
        // One trace, whatever became of its program.
        if (!traces) {
            result.traceFailed = true;
            return result;
        }
        tracker::StoreTraceSource source(*traces, *result.inventory);
        auto target = source.trace_target(request.traceId, result.traceError);
        if (!target) {
            result.traceFailed = true;
            return result;
        }
        result.programs.push_back({target->program_id, toQString(target->display_name)});
        result.targets.push_back(std::move(*target));
    } else if (!request.folder.empty()) {
        auto folder = force::folder_target(environment, request.folder, request.name, request.publisher);
        if (folder.error != force::FolderError::None) {
            result.folderError = folder.error;
            result.protection = folder.protection;
            return result;
        }
        result.programs.push_back({folder.target.program_id, toQString(folder.target.display_name)});
        result.targets.push_back(std::move(folder.target));
    } else {
        const auto& programs = result.inventory->programs;
        std::vector<const inventory::Program*> found;
        for (const auto& id : request.programIds) {
            const auto program = std::find_if(programs.begin(), programs.end(),
                                              [&](const inventory::Program& p) { return p.id == id; });
            // Packaged apps are removed by Windows (Uninstall), never forced.
            if (program == programs.end() || program->source == inventory::Source::Appx) {
                continue;
            }
            result.programs.push_back({program->id, program->display_name.empty()
                                                        ? tr("(no name)")
                                                        : toQString(program->display_name)});
            found.push_back(&*program);
        }
        // Reverting by the trace: a program's install trace, tied to it as it is now.
        result.targets = captureTargets(found, *result.inventory, traces.get());
    }
    if (!result.targets.empty()) {
        result.scan = std::make_shared<const leftovers::ScanResult>(
            leftovers::scan_leftovers(environment, result.targets, result.inventory->programs));
    }
    return result;
}

void ForceFlow::onScanned(std::shared_ptr<Scan> scan) {
    Q_EMIT progress(QString());
    scan_ = std::move(scan);
    if (scan_->folderError != force::FolderError::None) {
        showError(tr("Avel can't remove %1: %2.")
                      .arg(QDir::toNativeSeparators(toQString(request_.folder)), folderErrorText(*scan_)),
                  false);
        return;
    }
    if (scan_->traceFailed) {
        showError(traceErrorText(*scan_), false);
        return;
    }
    if (!scan_->scan) {
        auto* box =
            new QMessageBox(QMessageBox::Information, tr("Force uninstall"),
                            tr("The selected programs are no longer installed."), QMessageBox::Ok, window_);
        box->setAttribute(Qt::WA_DeleteOnClose);
        connect(box, &QDialog::finished, this, [this] { finish(true); });
        box->open();
        return;
    }

    QStringList names;
    for (const auto& program : scan_->programs) {
        names << program.name;
    }
    subject_ = static_cast<std::size_t>(names.size()) <= kNamedPrograms
                   ? names.join(u", "_s)
                   : tr("%n program(s)", nullptr, static_cast<int>(names.size()));

    const auto store = stores_->store();
    auto* review = new LeftoverDialog(scan_->scan, scan_->programs, LeftoverDialog::Mode::Force,
                                      store ? QString() : stores_->unavailableReason(), window_);
    review->setAttribute(Qt::WA_DeleteOnClose);
    // The deep search starts from the targets as built, like the first scan. A program's trace can
    // be left out ("Revert by the install trace"); a trace asked for by its id is the point.
    review->setRescan(
        [inventory = scan_->inventory, targets = scan_->targets](const leftovers::ScanOptions& options,
                                                                 bool useTraces) {
            const leftovers::LiveEnvironment live;
            return leftovers::scan_leftovers(live.environment(),
                                             useTraces ? targets : LeftoverDialog::withoutTraces(targets),
                                             inventory->programs, options);
        },
        request_.traceId.empty());
    connect(review, &QDialog::accepted, this,
            [this, review] { onReviewed(review->plan(), review->allowIrreversible()); });
    connect(review, &QDialog::rejected, this, [this] { finish(false); });
    review->open();
}

void ForceFlow::onReviewed(const changes::ChangePlan& plan, bool allowIrreversible) {
    plan_ = plan;
    allowIrreversible_ = allowIrreversible;
    // Listed now rather than with the scan: the review may have taken a while.
    QApplication::setOverrideCursor(Qt::BusyCursor);
    pool_.start([this, targets = scan_->scan->targets] {
        const leftovers::LiveEnvironment live;
        auto processes = force::processes_in(live.environment(), targets, platform::running_processes(),
                                             platform::service_process_ids());
        QMetaObject::invokeMethod(
            this, [this, processes = std::move(processes)] { onProcessesListed(processes); },
            Qt::QueuedConnection);
    });
}

void ForceFlow::onProcessesListed(std::vector<platform::ProcessInfo> processes) {
    QApplication::restoreOverrideCursor();
    if (processes.empty()) {
        remove();
        return;
    }
    auto* running = new RunningProcessesDialog(std::move(processes), subject_, window_);
    running->setAttribute(Qt::WA_DeleteOnClose);
    connect(running, &QDialog::accepted, this, &ForceFlow::remove);
    connect(running, &QDialog::rejected, this, [this] { finish(false); });
    running->open();
}

void ForceFlow::remove() {
    auto* removal = new ChangeSetProgressDialog(stores_->store(), plan_, allowIrreversible_, window_);
    removal->setAttribute(Qt::WA_DeleteOnClose);
    removal->setWindowTitle(tr("Forced removal"));
    connect(removal, &QDialog::finished, this, [this, removal] {
        // The removed programs' traces are kept 30 days from now.
        if (removal->changedAnything() && traces_ != nullptr) {
            traces_->startMaintenance();
        }
        finish(removal->changedAnything());
    });
    removal->open();
}

void ForceFlow::showError(const QString& text, bool changedAnything) {
    auto* box = new QMessageBox(QMessageBox::Warning, tr("Force uninstall"), text, QMessageBox::Ok, window_);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QDialog::finished, this, [this, changedAnything] { finish(changedAnything); });
    box->open();
}

void ForceFlow::finish(bool changedAnything) {
    Q_EMIT finished(changedAnything);
    deleteLater();
}

QString ForceFlow::folderErrorText(const Scan& scan) const {
    using force::FolderError;
    switch (scan.folderError) {
        case FolderError::None:
            return {};
        case FolderError::NotAbsolute:
            return tr("it isn't a full path");
        case FolderError::NotFound:
            return tr("it doesn't exist");
        case FolderError::NotAFolder:
            return tr("it isn't a folder");
        case FolderError::ReparsePoint:
            return tr("it is a link to another place, which may not be the program's");
        case FolderError::Protected:
            return tr("Windows or other programs need it");
    }
    return {};
}

QString ForceFlow::traceErrorText(const Scan& scan) const {
    using tracker::TraceError;
    const QString id = toQString(request_.traceId);
    if (traces_ == nullptr || !traces_->store()) {
        return traces_ != nullptr ? traces_->unavailableReason() : tr("Avel can't use install traces.");
    }
    switch (scan.traceError) {
        case TraceError::NotFound:
            return tr("The install trace %1 no longer exists.").arg(id);
        case TraceError::NotFinished:
            return tr("The install trace %1 isn't finished yet. Stop it first, then revert by it.").arg(id);
        default:
            return tr("The install trace %1 can't be read: its files are damaged.").arg(id);
    }
}

}  // namespace avel::app
