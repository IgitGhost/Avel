// Turns a volume's USN journal records into the net file changes of a trace (ADR 0002): what was
// created, deleted, modified or renamed between its start and its end, with the paths items had at
// those two moments.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/platform/usn_journal.h"
#include "core/platform/volume_folders.h"
#include "core/tracker/trace.h"

namespace avel::tracker {

// Everything the journal said about each item of one volume during a trace, by file reference
// number. Records must be added in journal order, each once: a checkpoint is only consistent with
// the journal position it was saved at (see save_checkpoint in journal_trace.h).
class FileChangeAggregator {
public:
    struct Name {
        std::uint64_t parent = 0;
        std::wstring name;
        bool operator==(const Name&) const = default;
    };
    struct Entry {
        std::optional<Name> first;        // Its name before the trace changed it
        std::optional<Name> last;         // Its name after its last change
        std::uint32_t reasons = 0;        // USN_REASON_* of every record
        std::uint32_t first_reasons = 0;  // Of its first record
        std::uint32_t open_reasons = 0;   // Accumulated since its last close
        std::uint32_t attributes = 0;     // FILE_ATTRIBUTE_* of every record
        std::vector<Name> links;          // Names of hard links added or removed
        bool operator==(const Entry&) const = default;
    };

    void add(const platform::UsnRecord& record);

    const std::unordered_map<std::uint64_t, Entry>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

    // A binary checkpoint (versioned, with a checksum), kept in the trace's folder so a trace
    // survives a restart. load rejects anything damaged or from another version.
    std::vector<std::byte> save() const;
    static std::optional<FileChangeAggregator> load(std::span<const std::byte> data);

private:
    std::unordered_map<std::uint64_t, Entry> entries_;
};

// Where items are now, for the folders the journal said nothing about during the trace. Only
// folders are looked up by reference number; files take their names from the records.
class PathResolver {
public:
    virtual ~PathResolver() = default;
    // The folder's current full path.
    virtual std::optional<std::wstring> path_now(std::uint64_t folder) = 0;
    // The folder's current parent and name.
    virtual std::optional<std::pair<std::uint64_t, std::wstring>> name_now(std::uint64_t folder) = 0;
    // The reference number of the item at `path` now, to tell which hard links still exist. Read
    // from the disk by default.
    virtual std::optional<std::uint64_t> id_at(const std::wstring& path);
    // The creation time (FILETIME as an integer) of the item at `path` now. Read from the disk by
    // default.
    virtual std::optional<std::int64_t> created_at(const std::wstring& path);
    // The attributes (FILE_ATTRIBUTE_*) of the item at `path` now, as programs see them. Read from
    // the disk by default.
    virtual std::optional<std::uint32_t> attributes_at(const std::wstring& path);
};

// A PathResolver over a live volume: opens folders by id, and if that fails reads the volume's
// folder list once (VolumeFolders) and looks them up there. Caches every answer.
class VolumeResolver : public PathResolver {
public:
    explicit VolumeResolver(platform::UsnVolume volume);

    const platform::UsnVolume& volume() const { return volume_; }

    std::optional<std::wstring> path_now(std::uint64_t folder) override;
    std::optional<std::pair<std::uint64_t, std::wstring>> name_now(std::uint64_t folder) override;

private:
    std::optional<std::size_t> listed(std::uint64_t folder);

    platform::UsnVolume volume_;
    std::unordered_map<std::uint64_t, std::optional<std::wstring>> paths_;
    std::unordered_map<std::uint64_t, std::optional<std::pair<std::uint64_t, std::wstring>>> names_;
    bool folders_read_ = false;
    platform::VolumeFolders folders_;
    std::unordered_map<std::uint64_t, std::size_t> folder_index_;
};

// The net changes of one volume. `root` is the volume's root ("C:\") and `root_frn` its root
// folder's reference number; `trace_start` is when the trace started (FILETIME as an integer).
// Created items are listed as roots only: an item inside a created folder is counted in the
// folder's `contents` instead. Items whose path couldn't be determined have NoiseReason::Unresolved
// and a best-effort path with "?" for the unknown part. Sorted by path.
std::vector<TraceChange> resolve_file_changes(const FileChangeAggregator& changes, std::uint64_t root_frn,
                                              const std::wstring& root, PathResolver& resolver,
                                              std::int64_t trace_start);

}  // namespace avel::tracker
