// Tying a trace's items to the program it installed (docs/trace.md, "Tying"). An item is tied when
// it is one of the program's entry's own locations, when it is a created root named after the
// program or its publisher, or when, over three rounds, it lies inside a tied item or holds a value
// that references one. Everything else the trace recorded is untied: it happened while the program
// was installed, but nothing ties it to the program. The leftover scan (package C) gives tied
// items its strongest evidence and untied ones a weak one, so noise is never pre-checked.
//
// The tracker may depend on leftovers; leftovers never depends on the tracker.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/scan_target.h"
#include "core/platform/paths.h"
#include "core/tracker/trace_record.h"

namespace avel::tracker {

enum class TieReason : std::uint8_t {
    None,                 // Untied
    InstallFolder,        // The entry's install folder, or inside it
    UninstallerFolder,    // The folder of the file its uninstall command runs, or inside it
    IconFolder,           // The folder of its icon, or inside it
    UninstallKey,         // Its Uninstall key, or in it
    MsiRegistration,      // Windows Installer's keys for its product code
    NamedAfterProgram,    // A created root named after the program
    NamedAfterPublisher,  // A created root named after its publisher
    InsideTied,           // Lies inside an item tied before (rounds 1 to 3)
    ReferencesTied,       // Holds a value that references an item tied before, or one of the entry's
                          // folders: a path (platform::referenced_paths), or a ProgID or CLSID name
};

std::string_view to_string(TieReason reason);

// One traced change that isn't noise, tied or not.
struct TiedItem {
    std::size_t change = 0;  // Index into TraceResult::changes: how it joins back to its TraceChange
    bool tied = false;
    TieReason reason = TieReason::None;
    int round =
        0;  // 0: by the entry's locations or a name; 1 to 3: inside or referencing what was tied before
    // InsideTied and ReferencesTied: the change it lies in or references (an index into
    // TraceResult::changes), or nullopt when it references one of the entry's folders.
    std::optional<std::size_t> via;
};

// The program's places the rules look at: its ScanTarget (install folder, uninstaller's folder,
// Uninstall key, product code, names, owner) and its icon's folder, which ScanTarget doesn't keep.
struct ProgramPlaces {
    leftovers::ScanTarget target;
    std::wstring icon_folder;  // Normalized; empty when there is none, or it is in the Windows folder
};

// make_scan_target, plus the icon's folder (outside the Windows folder). Reads the disk for long
// paths, like make_scan_target; capture it while the program is still installed.
ProgramPlaces program_places(const inventory::Program& program);

// What tying reads of the system, for the references.
struct TieSources {
    // The strings an item holds: a registry value's (REG_SZ, REG_EXPAND_SZ, and each string of a
    // REG_MULTI_SZ), every string value in a created key and its subkeys, a service's ImagePath and
    // ServiceDll, a task's commands, a firewall rule's program. Empty for files and folders.
    std::function<std::vector<std::wstring>(const leftovers::Location& location)> strings;
    platform::Expander expand;                 // For referenced_paths (expand_environment by default)
    platform::FileExistsFunction file_exists;  // For referenced_paths (the disk by default)
    // Folders too broad to be a program's own (known folders, drive roots are always left out):
    // an entry whose install folder is one of them ties nothing by it. Normalized.
    std::vector<std::wstring> broad_folders;
};

// This machine: the registry (the machine and signed-in users' hives), the Task Scheduler, and the
// machine's known folders and every user's profile as broad folders.
TieSources live_tie_sources();

// Ties a trace's changes that aren't noise to the program, in the order of `result.changes`. The
// registry is read live for the references, so tie before the program's uninstaller runs.
std::vector<TiedItem> tie_trace_items(const TraceResult& result, const ProgramPlaces& places,
                                      const TieSources& sources = live_tie_sources());

}  // namespace avel::tracker
