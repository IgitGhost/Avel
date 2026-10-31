// The trace store as the leftover scan's TraceSource (docs/trace.md, "Uninstalling by the trace"):
// a program's newest finished trace, its items tied to the program, and the values the trace saved
// with what they are now; what the user excluded in the trace's review (review.json) is left out.
// Also a target for one trace whatever became of its program (`force --trace`).
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/leftovers/trace_source.h"
#include "core/tracker/trace_items.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_review.h"
#include "core/tracker/trace_store.h"

namespace avel::tracker {

struct StoreTraceSourceOptions {
    // What tying reads; live_tie_sources() when unset.
    std::optional<TieSources> tie;
    // A value exactly as it is now (`read` false when it can't be read); when unset, the live
    // registry through the native API (HKLM, and HKU\<sid> while the user's hive is loaded).
    std::function<leftovers::CapturedValue(const platform::RegistryAddress& key, const std::wstring& name)>
        read_value;
};

class StoreTraceSource : public leftovers::TraceSource {
public:
    // Lists the store's traces once. `store` and `inventory` (what is installed now) must outlive it.
    StoreTraceSource(const TraceStore& store, const inventory::Inventory& inventory,
                     StoreTraceSourceOptions options = {});

    // The program's newest finished trace whose result and review can be read (traces_of): its
    // items that the review doesn't exclude, tied to the program as it is now, and what the review
    // excludes (ScanTarget::trace_excluded).
    bool add_trace(const inventory::Program& program, leftovers::ScanTarget& target) override;

    // The trace add_trace tries first; nullopt when the program has none.
    std::optional<std::wstring> trace_of(const inventory::Program& program) const;

    // A target for one finished trace (`force --trace`): its program's (make_scan_target) while one
    // of its links leads to an installed program; otherwise recorded_places'. Its items are tied to
    // that, except those its review excludes (ScanTarget::trace_excluded). nullopt with NotFound,
    // NotFinished, or Damaged when its result or review can't be read.
    std::optional<leftovers::ScanTarget> trace_target(const std::wstring& id, TraceError& error);

private:
    // The trace's items but what `review` excludes, tied to `places`, and the exclusions.
    void add(const TraceInfo& info, const TraceResult& recorded, const TraceReview& review,
             const ProgramPlaces& places, leftovers::ScanTarget& target);

    const TraceStore& store_;
    const inventory::Inventory& inventory_;
    StoreTraceSourceOptions options_;
    std::vector<TraceInfo> traces_;
};

// What uninstalling by a trace can bring, counted on its result alone, before any scan (noise left
// out): the items its installer created and the values it changed in the environment and
// SharedDLLs, which are what the scan offers at most (less what is gone or protected by then, and
// values it plans no edit for), and its changes to what existed before, which are only shown.
struct TraceOffer {
    std::size_t created = 0;
    std::size_t values = 0;
    std::size_t changed = 0;  // Deleted, modified and renamed items
};

TraceOffer trace_offer(const TraceResult& result);

// The places of a trace's program once no installed program answers to its links, from what the
// primary link (or failing that the first) last recorded: the name and publisher, the install
// folder, the Uninstall key (and so the owner of a per-user program) and the product code. An
// unlinked trace has only its description for a name. The program id is "trace:<id>".
ProgramPlaces recorded_places(const TraceInfo& info);

}  // namespace avel::tracker
