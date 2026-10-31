#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/leftovers/confidence.h"
#include "core/leftovers/leftover.h"

namespace avel::changes {

// One thing a ChangeSet is to remove, as the user approved it.
struct PlannedItem {
    leftovers::Location location;
    leftovers::Category category = leftovers::Category::Files;
    std::vector<std::wstring> programs;  // Display names of the programs that left it
    leftovers::Confidence confidence = leftovers::Confidence::Unknown;
    int score = 0;
};

struct ChangePlan {
    std::wstring description;  // "Leftovers of Notepad++", shown in the Undo Center
    std::vector<PlannedItem> items;
};

PlannedItem make_planned_item(const leftovers::Leftover& leftover, std::vector<std::wstring> programs);

// The plan for removing the scan's items at or above `minimum`, leaving out what can't be removed
// (firewall rules) and what is never selected automatically (leftovers::held_back: what an install
// trace's review excludes, and what holds it). `leftovers` is a whole scan result, which
// Leftover::inside refers to. `program_name` turns an owner id into the name shown in the Undo Center.
ChangePlan make_plan(std::span<const leftovers::Leftover> leftovers, leftovers::Confidence minimum,
                     const std::function<std::wstring(const std::wstring& program_id)>& program_name,
                     std::wstring description);

enum class ItemState : std::uint8_t {
    Pending,         // Nothing done yet
    BackedUp,        // Its backup is on disk; whether it was removed is only known after recovery
    Done,            // Removed
    Partial,         // Folders only: some files were in use and stayed
    Skipped,         // Left alone; nothing changed (see the reason)
    Failed,          // Removing it failed; nothing changed
    Restored,        // Put back by undo
    RestoreSkipped,  // Not put back: something new is in its place (see the reason)
    RestoreFailed,
};

enum class Reason : std::uint8_t {
    None,
    Protected,          // A protection layer refused it when it came to removing it
    PathChanged,        // It resolves somewhere else now (a parent became a link)
    Missing,            // It was gone already
    AccessDenied,       // Windows refused; Avel never forces its way
    InUse,              // Files are open in running programs
    RegistryLink,       // A registry symbolic link, never followed
    UnsupportedVolume,  // Not on a fixed NTFS volume
    HasDependents,      // Other services depend on the service
    NeedsPassword,      // Runs under an account whose password undo couldn't restore
    MarkedForDeletion,  // The service is already being deleted
    OtherUser,          // A package registered for another user
    NotUndoable,        // Not removed because it couldn't be undone and that wasn't allowed
    ReportOnly,         // Firewall rules are shown, never removed
    Contained,          // Its container is removed too, which takes it along
    Interrupted,        // The run stopped before finishing; recovery found it untouched
    Exists,             // Undo: something new is in its place
    BackupMissing,      // Undo: the backup is gone or unreadable
    Error,              // See the Win32 code
    Changed,            // A value edit: the value changed since planning (for undo: since the edit)
    NotEmpty,           // A task folder that still holds tasks or folders
};

struct ItemStatus {
    ItemState state = ItemState::Pending;
    Reason reason = Reason::None;
    std::uint32_t code = 0;  // Win32 error or HRESULT, when there is one
};

enum class SetState : std::uint8_t {
    Running,         // In progress, or interrupted and not yet recovered
    Complete,        // Every item reached a final state
    Restored,        // Undone completely
    PartlyRestored,  // Undo restored some items
};

struct SetSummary {
    std::wstring id;
    std::chrono::system_clock::time_point created;
    std::wstring description;
    SetState state = SetState::Running;
    std::size_t items = 0;
    std::size_t done = 0;      // Done or Partial
    std::size_t skipped = 0;   // Skipped or Failed
    std::size_t restored = 0;
    std::uint64_t quarantined_bytes = 0;
};

std::string_view to_string(ItemState state);
std::string_view to_string(Reason reason);
std::string_view to_string(SetState state);
bool parse(std::string_view text, ItemState& state);
bool parse(std::string_view text, Reason& reason);
bool parse(std::string_view text, SetState& state);

// Whether an item kind can be removed at all, and undone after.
bool is_removable(const leftovers::Location& location);
bool is_undoable(const leftovers::Location& location);

}  // namespace avel::changes
