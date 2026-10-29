// The trace store as the leftover scan's TraceSource (docs/trace.md, "Uninstalling by the trace"):
// a program's newest finished trace, its items tied to the program, and the values the trace saved
// with what they are now. Also a target for one trace whatever became of its program (`force
// --trace`).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/leftovers/trace_source.h"
#include "core/tracker/trace_items.h"
#include "core/tracker/trace_record.h"
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

    // The program's newest finished trace whose result can be read (traces_of), its items tied to
    // the program as it is now.
    bool add_trace(const inventory::Program& program, leftovers::ScanTarget& target) override;

    // The trace add_trace tries first; nullopt when the program has none.
    std::optional<std::wstring> trace_of(const inventory::Program& program) const;

    // A target for one finished trace (`force --trace`): its program's (make_scan_target) while one
    // of its links leads to an installed program; otherwise recorded_places'. Its items are tied to
    // that. nullopt with NotFound, NotFinished, or Damaged when its result can't be read.
    std::optional<leftovers::ScanTarget> trace_target(const std::wstring& id, TraceError& error);

private:
    void add(const TraceInfo& info, const TraceResult& result, const ProgramPlaces& places,
             leftovers::ScanTarget& target);

    const TraceStore& store_;
    const inventory::Inventory& inventory_;
    StoreTraceSourceOptions options_;
    std::vector<TraceInfo> traces_;
};

// The places of a trace's program once no installed program answers to its links, from what the
// primary link (or failing that the first) last recorded: the name and publisher, the install
// folder, the Uninstall key (and so the owner of a per-user program) and the product code. An
// unlinked trace has only its description for a name. The program id is "trace:<id>".
ProgramPlaces recorded_places(const TraceInfo& info);

}  // namespace avel::tracker
