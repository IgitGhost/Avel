#include "program_traces.h"

#include <QDateTime>
#include <QLocale>
#include <QStringList>

#include <optional>

#include "core/leftovers/post_uninstall.h"
#include "core/tracker/linking.h"
#include "core/tracker/trace_review.h"
#include "trace_store_access.h"

using namespace Qt::StringLiterals;

namespace avel::app {

ProgramTraces::ProgramTraces(TraceStoreAccess* traces, QObject* parent) : QObject(parent), traces_(traces) {
    pool_.setMaxThreadCount(1);
    connect(traces_, &TraceStoreAccess::tracesChanged, this, [this] {
        // Read the traces' results again too: one may have been replaced.
        offers_.clear();
        ++generation_;
        start();
    });
}

ProgramTraces::~ProgramTraces() { pool_.waitForDone(); }

void ProgramTraces::refresh(std::shared_ptr<const inventory::Inventory> inventory) {
    inventory_ = std::move(inventory);
    start();
}

const ProgramTraces::Trace* ProgramTraces::find(const std::wstring& programId) const {
    const auto found = found_.find(programId);
    return found != found_.end() ? &found->second : nullptr;
}

void ProgramTraces::start() {
    const auto store = traces_->store();
    if (!store || !inventory_) {
        return;  // Nothing is ever traced without the store
    }
    if (running_) {
        pending_ = true;
        return;
    }
    running_ = true;
    pool_.start([this, store, inventory = inventory_, offers = offers_, generation = generation_]() mutable {
        const auto infos = store->list();
        Found found;
        for (const auto& [programId, index] : tracker::program_traces(infos, *inventory)) {
            const auto& info = infos[index];
            Trace trace;
            trace.id = info.id;
            trace.started = info.started;
            trace.incomplete = info.state == tracker::TraceState::Incomplete;
            trace.imported = info.imported.has_value();
            auto offer = offers.find(info.id);
            if (offer == offers.end()) {
                // Without what the user excluded, as StoreTraceSource offers it; a trace whose
                // review can't be read isn't used at all.
                const auto result = store->read_result(info.id);
                const auto review = result ? store->read_review(info.id) : std::nullopt;
                if (result && review) {
                    offer = offers.emplace(info.id, tracker::trace_offer(tracker::apply_review(*result, *review)))
                                .first;
                }
            }
            if (offer != offers.end()) {
                trace.offer = offer->second;
            }
            found.emplace(programId, std::move(trace));
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, found = std::move(found), offers = std::move(offers)]() mutable {
                onFinished(generation, std::move(found), std::move(offers));
            },
            Qt::QueuedConnection);
    });
}

void ProgramTraces::onFinished(std::uint64_t generation, Found found, Offers offers) {
    running_ = false;
    found_ = std::move(found);
    if (generation == generation_) {
        offers_ = std::move(offers);
    }
    Q_EMIT changed();
    if (pending_) {
        pending_ = false;
        start();
    }
}

std::size_t ProgramTraces::offeredCount(const Trace& trace) {
    return trace.offer ? trace.offer->created + trace.offer->values : 0;
}

QString ProgramTraces::markText(const Trace& trace) {
    return trace.incomplete ? tr("Traced, incomplete") : tr("Traced");
}

QString ProgramTraces::summaryText(const Trace& trace) {
    const auto started = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(trace.started.time_since_epoch()).count());
    const QString when = QLocale().toString(started, QLocale::ShortFormat);
    const QString traced =
        trace.imported ? tr("Traced on %1, imported").arg(when) : tr("Traced on %1").arg(when);
    if (!trace.offer) {
        return tr("%1. Its record can't be read.").arg(traced);
    }
    const auto& offer = *trace.offer;
    QStringList parts;
    parts << tr("%n item(s) created", nullptr, static_cast<int>(offer.created));
    if (offer.values > 0) {
        parts << tr("%n change(s) to environment variables or shared DLLs", nullptr,
                    static_cast<int>(offer.values));
    }
    if (offer.changed > 0) {
        parts << tr("%n existing item(s) changed", nullptr, static_cast<int>(offer.changed));
    }
    return tr("%1: %2.").arg(traced, parts.join(u", "_s));
}

QString ProgramTraces::incompleteText() {
    return tr(
        "The install trace is incomplete: a drive or part of the registry couldn't be compared, so it may "
        "have missed some of what the installer did.");
}

std::vector<leftovers::ScanTarget> captureTargets(std::span<const inventory::Program* const> programs,
                                                  const inventory::Inventory& inventory,
                                                  const tracker::TraceStore* traces) {
    std::optional<tracker::StoreTraceSource> source;
    if (traces != nullptr) {
        source.emplace(*traces, inventory);
    }
    return leftovers::capture_targets(programs, source ? &*source : nullptr);
}

}  // namespace avel::app
