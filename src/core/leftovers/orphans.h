// Orphaned folders: folders at the top of the places programs are installed to (both Program
// Files, ProgramData, each user's AppData\Local\Programs) that no installed program owns. Avel's
// own; BCU has nothing like it.
#pragma once

#include <span>

#include "core/inventory/program.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/scan.h"

namespace avel::leftovers {

// A folder is owned, and left out, when:
//   - an installed program's install or uninstaller folder is it, lies in it or holds it;
//   - its name is like a program's or names its publisher (whole words count: "Code" for
//     Microsoft Visual Studio Code);
//   - a service, task, Run value, App Path, COM class, shortcut or firewall rule points into it
//     (MuiCache and SharedDLLs don't count: they outlive the programs they name);
//   - it is Windows' own (Packages, ssh, USOShared, regid.*, Microsoft..., Windows...);
//   - its name holds a publisher's name as a word ("Intel Package Cache" with Intel's drivers
//     installed).
// What is left is offered as Questionable (Evidence::NoOwner), so it is never pre-checked: a
// portable program, or data a program keeps on its own, looks the same. Folders of other users
// also get OtherUserData. Each item's owner id is "folder:<path>", like a forced folder.
ScanResult find_orphans(const ScanEnvironment& environment, std::span<const inventory::Program> installed);

}  // namespace avel::leftovers
