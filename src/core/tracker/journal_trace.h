// The file half of a trace (ADR 0002): each fixed NTFS volume's USN journal position when the
// trace starts, and reading the journal on from there while it runs, so records are collected
// before the journal can overwrite them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/usn_journal.h"
#include "core/tracker/file_changes.h"
#include "core/tracker/trace.h"

namespace avel::tracker {

enum class JournalStatus : std::uint8_t {
    Ok,
    Untracked,      // Not NTFS, or it had no journal and creating one failed
    VolumeMissing,  // No drive letter leads to the volume any more
    SerialChanged,  // The volume was formatted
    Replaced,       // The journal was deleted, or deleted and created again
    Wrapped,        // Records the trace needed were overwritten before they were read
    Unreadable,     // The volume or its journal couldn't be read (see `error`)
};

// One volume's part of a trace. Once its status isn't Ok it is no longer read; what was read
// before stays valid, and the other volumes are unaffected.
struct VolumeTrace {
    std::wstring root;       // "C:\"; follows the volume when its drive letter changes
    std::wstring guid_path;  // "\\?\Volume{...}\"
    std::uint32_t serial = 0;
    std::uint64_t root_frn = 0;  // The root folder's file reference number
    std::uint64_t journal_id = 0;
    std::int64_t start_usn = 0;
    std::int64_t next_usn = 0;     // Read up to here
    bool created_journal = false;  // The volume had no journal; the trace created one and leaves it
    JournalStatus status = JournalStatus::Ok;
    std::uint32_t error = 0;  // The Win32 error behind the status, if there was one
};

// The journal a volume without one gets.
constexpr std::uint64_t kJournalMaxSize = 64ull << 20;
constexpr std::uint64_t kJournalAllocationDelta = 8ull << 20;

// Records the volume's position, creating a journal if it has none. Needs administrator rights.
VolumeTrace start_volume(const std::wstring& root);
// start_volume for every fixed NTFS volume with a drive letter.
std::vector<VolumeTrace> start_volumes();

// Reads what the journal recorded since `volume.next_usn` into `changes` and moves next_usn on.
// Records of NTFS's own metadata and auxiliary records are dropped, and so is any record `keep`
// rejects. On a failure, sets the status and error and stops tracking the volume.
void drain_volume(VolumeTrace& volume, FileChangeAggregator& changes,
                  const std::function<bool(const platform::UsnRecord&)>& keep = {});

// The volume's net changes (resolve_file_changes over the live volume, found by its GUID path).
// Paths that need the disk are unresolved if the volume can't be opened.
std::vector<TraceChange> resolve_volume(const VolumeTrace& volume, const FileChangeAggregator& changes,
                                        std::int64_t trace_start);

// The volume's state and its changes together, so a checkpoint always matches the journal
// position it was read up to (versioned, with a checksum). load_checkpoint leaves its outputs
// alone and returns false for anything damaged or from another version.
std::vector<std::byte> save_checkpoint(const VolumeTrace& volume, const FileChangeAggregator& changes);
bool load_checkpoint(std::span<const std::byte> data, VolumeTrace& volume, FileChangeAggregator& changes);

// --- The decisions drain_volume makes, apart so they can be tested -----------------------------

struct MountedVolume {
    std::wstring root;  // "D:\"
    std::wstring guid_path;
};

// The drive root the traced volume has now, matched by its GUID path: drive letters can change
// across a restart. Prefers the root it had.
std::optional<std::wstring> find_volume_root(const VolumeTrace& volume,
                                             std::span<const MountedVolume> mounted);

// Whether reading can go on from `volume.next_usn`, given the volume's serial number now and what
// FSCTL_QUERY_USN_JOURNAL returned.
JournalStatus check_journal(const VolumeTrace& volume, std::uint32_t serial, std::uint32_t query_error,
                            const platform::JournalState& journal);

// What a failed FSCTL_READ_USN_JOURNAL means.
JournalStatus read_failure(std::uint32_t error);

// Records that say nothing about what programs see: NTFS's own metadata files (MFT records below
// 24, items directly in a metadata folder such as $Extend, '$' names in the root) and auxiliary
// records (USN_SOURCE_AUXILIARY_DATA).
bool is_journal_noise(const platform::UsnRecord& record, std::uint64_t root_frn);

std::string_view to_string(JournalStatus status);

}  // namespace avel::tracker
