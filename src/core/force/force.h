// Avel Force: removing programs whose uninstaller is missing or broken, and folders that no
// Uninstall entry describes. Force never runs a program's uninstaller. It ends the program's
// running processes once the user agrees, then removes what the leftover scan finds for it, as a
// ChangeSet that undo can put back (see docs/force.md).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/protection.h"
#include "core/leftovers/scan_target.h"
#include "core/platform/processes.h"

namespace avel::force {

// Whether a folder may be offered as a whole: HardFloor, then ProtectedPolicy for the known
// folders of the environment's machine and users.
leftovers::ProtectionReason folder_protection(const leftovers::ScanEnvironment& environment,
                                              std::wstring_view folder);

enum class FolderError : std::uint8_t {
    None,
    NotAbsolute,
    NotFound,
    NotAFolder,
    ReparsePoint,  // A junction or symbolic link: what it points to isn't the program's
    Protected,     // See folder_protection
};

struct FolderTarget {
    leftovers::ScanTarget target;
    FolderError error = FolderError::None;
    leftovers::ProtectionReason protection = leftovers::ProtectionReason::None;
};

// A scan target for a folder: the folder itself, and whatever is named after `name` (the folder's
// own name when empty) and `publisher` elsewhere. Its program id is "folder:<path>".
FolderTarget folder_target(const leftovers::ScanEnvironment& environment, std::wstring_view folder,
                           std::wstring name = {}, std::wstring publisher = {});

// Who a program file (a running program's executable, say) belongs to.
struct FileOwner {
    const inventory::Program* program = nullptr;  // The installed program whose folder holds it
    // Otherwise the folder to remove: the file's folder, or the highest folder above it named after
    // the product (stopping below protected folders), and the names from its version resource.
    std::wstring folder;
    std::wstring name;
    std::wstring publisher;
};

// Who files belong to, for asking about many files: the programs' install and uninstaller folders
// and their protection are worked out once, when it is built. Borrows `programs`.
class OwnerIndex {
public:
    OwnerIndex(const leftovers::ScanEnvironment& environment, std::span<const inventory::Program> programs);

    // The program whose install (or uninstaller) folder holds `file`, the deepest such folder
    // winning; packaged apps match by their package folder. Protected folders (a program recording
    // C:\Program Files as its location) never match. Null if there is none.
    const inventory::Program* program_of(std::wstring_view file) const;

    // program_of, and without a program, the folder to remove and the names from the file's
    // version resource (see FileOwner).
    FileOwner owner_of(std::wstring_view file) const;

private:
    struct Folder {
        std::wstring path;  // Normalized
        const inventory::Program* program = nullptr;
        bool listed = false;  // inventory::is_listed_in_settings
    };
    leftovers::ProtectedPolicy policy_;
    std::vector<Folder> folders_;  // In the programs' order, each one's install folder first
};

// OwnerIndex::owner_of for one file.
FileOwner owner_of(const leftovers::ScanEnvironment& environment,
                   std::span<const inventory::Program> programs, std::wstring_view file);

// Running processes whose executable lies in one of the targets' folders (install or uninstaller
// folder, unless protected). Leaves out critical processes, the ones in `excluded` (service hosts:
// their services are stopped as leftovers) and this process.
std::vector<platform::ProcessInfo> processes_in(const leftovers::ScanEnvironment& environment,
                                                std::span<const leftovers::ScanTarget> targets,
                                                std::span<const platform::ProcessInfo> processes,
                                                std::span<const std::uint32_t> excluded);

std::string_view to_string(FolderError error);

}  // namespace avel::force
