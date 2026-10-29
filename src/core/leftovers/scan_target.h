#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/leftover.h"
#include "core/leftovers/product_name.h"
#include "core/leftovers/value_edits.h"
#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"

namespace avel::leftovers {

// How an install trace saw an item change (tracker::ChangeKind, in the same order).
enum class TracedChange : std::uint8_t {
    Created,   // New: the only kind the scan ever offers
    Deleted,   // The others existed before the install: shown only
    Modified,
    Renamed,
};

// An item a program's install trace recorded (see TraceSource), noise left out.
struct TracedItem {
    Location location;  // Where it was at the end of the trace (for Deleted, where it was before)
    Category category = Category::Files;
    TracedChange change = TracedChange::Created;
    bool tied = false;  // Tied to the program: the installer's own folders and keys (tracker/trace_items)
    std::wstring tie;   // Why, as the tracker names it ("install-folder", "references-tied", ...)
    // A created folder or key: how many items it held when the trace stopped, and which ones
    // (nullopt when the trace kept only the count: more than kMaxContainedItems, or some of them
    // couldn't be read). Anything more in it now was put there by someone else since.
    std::uint64_t contents = 0;
    std::optional<std::vector<ContainedItem>> inside;
};

// A value as it was at some moment; `read` is false when it couldn't be read then.
struct CapturedValue {
    bool read = false;
    std::optional<platform::RawValue> value;  // nullopt: it was absent
};

// What the leftover scan needs to know about a program. It is captured before the uninstaller
// runs: afterwards the Uninstall key is gone, and paths that no longer exist can't be resolved
// (8.3 names, for instance).
struct ScanTarget {
    std::wstring program_id;
    std::wstring display_name;
    ProductNames names;
    inventory::Scope scope = inventory::Scope::Machine;
    std::wstring user_sid;  // The owner of a per-user program

    // Normalized long path. When the entry records none, the uninstaller's or the icon's folder,
    // if it is named after the program; otherwise empty.
    std::wstring install_location;
    std::wstring uninstaller;         // The file the uninstall command runs, unless it is a Windows tool
    std::wstring uninstaller_folder;  // Its folder

    std::optional<platform::RegistryAddress> uninstall_key;
    std::wstring msi_product_code;  // "{GUID}" of a Windows Installer product, for its registration
    bool store_app = false;
    std::wstring package_full_name;

    // The program's install trace, when a TraceSource found one (Scanner::Trace; empty otherwise).
    std::wstring trace_id;
    std::vector<TracedItem> traced;
    // The values the installer added, changed or deleted in the environment and SharedDLLs.
    std::vector<TracedValue> traced_values;
    // traced_values[i] as it was when the target was captured, before the uninstaller ran. A
    // SharedDLLs count the uninstaller has changed since is its business: it isn't lowered again.
    std::vector<CapturedValue> captured_values;
};

ScanTarget make_scan_target(const inventory::Program& program);

}  // namespace avel::leftovers
