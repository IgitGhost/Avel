#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/leftovers/scan_target.h"
#include "core/tracker/trace_source.h"
#include "core/tracker/trace_store.h"

namespace avel::app {

class TraceStoreAccess;

// Which installed programs were installed with Avel Trace, for the "Traced" mark in the program
// list and the details pane, and for the uninstall dialog's "Use the install trace". Worked out on
// a worker thread after each inventory load (refresh) and whenever the traces change.
class ProgramTraces : public QObject {
    Q_OBJECT

public:
    // A program's trace: the one uninstalling and forcing it use (tracker::traces_of's first).
    struct Trace {
        std::wstring id;
        std::chrono::system_clock::time_point started{};
        bool incomplete = false;
        bool imported = false;
        // Counted on its result less what its review excludes (tracker::trace_offer); nullopt when
        // the result or the review can't be read, and then the trace isn't used.
        std::optional<tracker::TraceOffer> offer;
    };

    // `traces` must outlive this.
    explicit ProgramTraces(TraceStoreAccess* traces, QObject* parent = nullptr);
    ~ProgramTraces() override;

    // After each inventory load: works the programs' traces out anew. changed() follows.
    void refresh(std::shared_ptr<const inventory::Inventory> inventory);

    // The program's trace, or null. Valid until the next changed().
    const Trace* find(const std::wstring& programId) const;

    // What the trace can bring to the leftover review at most: the items its installer created
    // and the values it changed.
    static std::size_t offeredCount(const Trace& trace);
    // "Traced", or "Traced, incomplete": the mark in the program list.
    static QString markText(const Trace& trace);
    // When it was traced and what it recorded, in a sentence or two.
    static QString summaryText(const Trace& trace);
    static QString incompleteText();

Q_SIGNALS:
    void changed();

private:
    using Found = std::unordered_map<std::wstring, Trace>;
    using Offers = std::unordered_map<std::wstring, tracker::TraceOffer>;

    void start();
    void onFinished(std::uint64_t generation, Found found, Offers offers);

    TraceStoreAccess* traces_ = nullptr;
    std::shared_ptr<const inventory::Inventory> inventory_;
    Found found_;
    // What each trace offers, by its id: a finished trace's result doesn't change.
    Offers offers_;
    std::uint64_t generation_ = 0;  // Bumped when offers_ is cleared, so a running job's are dropped
    bool running_ = false;
    bool pending_ = false;
    // Declared last so it's destroyed first: its destructor waits for the worker.
    QThreadPool pool_;
};

// The programs' scan targets, captured before their uninstallers run, each with its install trace
// from `traces` when it has one (as avel-cli does); without traces when `traces` is null.
std::vector<leftovers::ScanTarget> captureTargets(std::span<const inventory::Program* const> programs,
                                                  const inventory::Inventory& inventory,
                                                  const tracker::TraceStore* traces);

}  // namespace avel::app
