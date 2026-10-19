// Moving leftover files and folders into quarantine and back. Every move is a rename on the same
// volume, so a file comes back with its contents, streams, security, timestamps and hard links.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/changes/serialization.h"

namespace avel::changes {

// What moving one item involves, written to its backup folder (file.json) before it moves, so
// undo and recovery can find everything again.
struct FileMove {
    std::wstring original;    // Normalized path
    std::wstring quarantine;  // Where it goes, on the same volume, under its own name
    bool directory = false;
    DWORD attributes = 0;     // The item's own attributes (a rename may set the archive bit)
    // Every folder in a folder's tree (the folder itself first, as ""), with what a recreated
    // folder needs to match: a folder is moved file by file when something in it is in use, and
    // the folders it empties are then removed.
    struct Folder {
        std::wstring relative;
        DWORD attributes = 0;
        FILETIME created{};
        FILETIME written{};
        std::wstring sddl;
    };
    std::vector<Folder> folders;
    // Files in the tree without the archive attribute: moving a file to another folder sets it,
    // so it's cleared again when the file comes back.
    std::vector<std::wstring> unarchived;
    std::uint64_t bytes = 0;  // Total size of its files
};

struct MoveResult {
    ItemStatus status;
    std::vector<std::wstring> left_behind;  // Files that stayed because they're in use
};

// Checks that `path` can be quarantined into the folder `quarantine` (which needn't exist yet) and
// describes the move. It must be on a fixed NTFS volume and resolve to itself (no parent folder
// may be a link). On failure returns nullopt with the status to record.
std::optional<FileMove> prepare_move(const std::wstring& path, bool directory, const std::wstring& quarantine,
                                     ItemStatus& failure);

// Moves the item: a single rename when possible, otherwise (a folder with files in use) file by
// file, leaving the files in use and their folders. Done, Partial (InUse) or Failed; Failed means
// nothing moved.
MoveResult move_to_quarantine(const FileMove& move);

// Moves everything in quarantine back, recreating removed folders as they were. `partial` says the
// move left files behind, so the folder is expected to exist and the rest is merged back into it;
// otherwise an existing item (a reinstalled program's folder, say) is never touched. Restored, or
// RestoreSkipped (Exists) when something new is in the way: nothing in the way is replaced, and
// what couldn't go back stays in quarantine.
MoveResult restore_from_quarantine(const FileMove& move, bool partial);

Json to_json(const FileMove& move);
bool from_json(const Json& json, FileMove& move);

}  // namespace avel::changes
