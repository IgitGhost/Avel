// An NTFS volume's USN change journal (ADR 0002): where it stands, the records written since a
// position, and the current names of files and folders by their file reference numbers. Opening
// the volume needs administrator rights.
#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace avel::platform {

struct JournalState {
    std::uint64_t id = 0;  // Changes when the journal is deleted and created again
    std::int64_t first_usn = 0;
    std::int64_t next_usn = 0;  // Where the next record will be written
    std::int64_t lowest_valid_usn = 0;
    std::uint64_t max_size = 0;
};

// One journal record. A file reference number holds the MFT record's sequence number in its top
// 16 bits, so a reused MFT record has a different number.
struct UsnRecord {
    std::int64_t usn = 0;
    std::uint64_t frn = 0;
    std::uint64_t parent_frn = 0;
    std::uint32_t reason = 0;       // USN_REASON_*, accumulated since the file was opened
    std::uint32_t source_info = 0;  // USN_SOURCE_*
    std::uint32_t attributes = 0;   // FILE_ATTRIBUTE_*
    std::int64_t time = 0;          // FILETIME, UTC
    std::wstring_view name;         // Points into the buffer it was read from
};

class UsnVolume {
public:
    // Opens the volume `root` ("C:\") for reading its journal, and its root folder as the hint
    // OpenFileById needs. Returns nullopt and sets `error` (ERROR_ACCESS_DENIED without
    // administrator rights) on failure.
    static std::optional<UsnVolume> open(const std::wstring& root, std::uint32_t& error);

    const std::wstring& root() const { return root_; }
    // "\\?\Volume{...}\": stays the same when the drive letter changes.
    const std::wstring& guid_path() const { return guid_path_; }
    std::uint32_t serial() const { return serial_; }
    std::uint64_t root_frn() const { return root_frn_; }

    // FSCTL_QUERY_USN_JOURNAL. Returns a Win32 error: ERROR_JOURNAL_NOT_ACTIVE when the volume has
    // no journal, ERROR_JOURNAL_DELETE_IN_PROGRESS while it is being deleted.
    std::uint32_t query(JournalState& state) const;
    // FSCTL_CREATE_USN_JOURNAL; also resizes an existing journal. Returns a Win32 error.
    std::uint32_t create(std::uint64_t max_size, std::uint64_t allocation_delta) const;

    // Passes every record with from <= usn < until to `sink`, in order; `until` is a next_usn
    // queried before, so the read ends even while the volume stays busy. `reached` is where the
    // next read continues. Returns a Win32 error: ERROR_JOURNAL_ENTRY_DELETED when `from` was
    // overwritten (the journal wrapped), ERROR_JOURNAL_NOT_ACTIVE, ERROR_JOURNAL_DELETE_IN_PROGRESS
    // or ERROR_INVALID_PARAMETER when the journal was deleted or replaced. Records read before an
    // error have been passed on, and `reached` says how far.
    std::uint32_t read(std::uint64_t journal_id, std::int64_t from, std::int64_t until,
                       const std::function<void(const UsnRecord&)>& sink, std::int64_t& reached) const;

    // The item's parent folder and name now (FSCTL_READ_FILE_USN_DATA); for a file with several
    // hard links, one of them. Nothing is cached.
    std::optional<std::pair<std::uint64_t, std::wstring>> name_of(std::uint64_t frn) const;
    // The item's full path now ("C:\Users\x"), normalized.
    std::optional<std::wstring> path_of(std::uint64_t frn) const;

private:
    UsnVolume() = default;
    wil::unique_hfile open_by_id(std::uint64_t frn) const;

    std::wstring root_;
    std::wstring guid_path_;
    std::uint32_t serial_ = 0;
    std::uint64_t root_frn_ = 0;
    wil::unique_hfile volume_;
    wil::unique_hfile root_folder_;
};

// Walks an FSCTL_READ_USN_JOURNAL output buffer: a next USN, then records. Version 2 and 3
// records are passed to `sink` (a version 3 record whose 128-bit id doesn't fit in 64 bits is
// skipped, as are other versions); a zero length or a record running past the buffer ends the walk.
// Returns the leading next USN (0 if the buffer is too short to hold one).
std::int64_t parse_usn_buffer(std::span<const std::byte> buffer,
                              const std::function<void(const UsnRecord&)>& sink);

}  // namespace avel::platform
