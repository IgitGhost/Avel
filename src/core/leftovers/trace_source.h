// Where the leftover scan gets a program's install trace from. The tracker implements it
// (tracker::StoreTraceSource, over the trace store); tests fake it. leftovers never depends on the
// tracker.
#pragma once

#include "core/inventory/program.h"
#include "core/leftovers/scan_target.h"

namespace avel::leftovers {

class TraceSource {
public:
    virtual ~TraceSource() = default;

    // Adds the program's install trace to its target (made by make_scan_target): the trace's id,
    // its items that aren't noise, each tied to the program or not, and the values it changed, as
    // they are now (ScanTarget::captured_values). Returns false and leaves the target as it is when
    // the program has no trace. Tying reads the live system, so call this before the program's
    // uninstaller runs.
    virtual bool add_trace(const inventory::Program& program, ScanTarget& target) = 0;
};

}  // namespace avel::leftovers
