#include "core/tracker/journal_trace.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>

#include "core/platform/file_ops.h"
#include "core/platform/hash.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::tracker {

namespace {

constexpr std::uint64_t kSegmentMask = 0x0000FFFFFFFFFFFF;
constexpr std::uint64_t kFirstUserSegment = 24;  // MFT records 0-23 are NTFS's own ($MFT ... $Extend)

constexpr std::uint32_t kCheckpointMagic = 0x56545641;  // "AVTV"
constexpr std::uint32_t kCheckpointVersion = 1;
constexpr std::uint32_t kMaxTextLength = 32767;

std::optional<std::wstring> guid_path_of(const std::wstring& root) {
    wchar_t guid_path[MAX_PATH] = {};
    if (!::GetVolumeNameForVolumeMountPointW(root.c_str(), guid_path, MAX_PATH)) {
        return std::nullopt;
    }
    return std::wstring(guid_path);
}

std::vector<MountedVolume> mounted_volumes(const VolumeTrace& volume) {
    // Usually the volume still has its drive letter; only look further when it doesn't.
    if (const auto guid_path = guid_path_of(volume.root);
        guid_path && platform::iequals(*guid_path, volume.guid_path)) {
        return {{volume.root, *guid_path}};
    }
    std::vector<MountedVolume> mounted;
    for (const auto& root : platform::fixed_ntfs_volumes()) {
        if (auto guid_path = guid_path_of(root)) {
            mounted.push_back({root, std::move(*guid_path)});
        }
    }
    return mounted;
}

// Answers nothing: for a volume that can't be opened, paths come from the records alone.
class NoResolver : public PathResolver {
public:
    std::optional<std::wstring> path_now(std::uint64_t) override { return std::nullopt; }
    std::optional<std::pair<std::uint64_t, std::wstring>> name_now(std::uint64_t) override {
        return std::nullopt;
    }
};

class Writer {
public:
    template <typename T>
    void put(const T& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        bytes_.insert(bytes_.end(), bytes, bytes + sizeof(T));
    }
    void put_text(const std::wstring& text) {
        put(static_cast<std::uint32_t>(text.size()));
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        bytes_.insert(bytes_.end(), bytes, bytes + text.size() * sizeof(wchar_t));
    }
    void put_bytes(std::span<const std::byte> bytes) {
        put(static_cast<std::uint64_t>(bytes.size()));
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    std::vector<std::byte> finish() {
        put(platform::fnv1a(bytes_.data(), bytes_.size()));
        return std::move(bytes_);
    }

private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    template <typename T>
    bool get(T& value) {
        if (remaining() < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }
    bool get_text(std::wstring& text) {
        std::uint32_t length = 0;
        if (!get(length) || length > kMaxTextLength || remaining() < length * sizeof(wchar_t)) {
            return false;
        }
        text.resize(length);
        std::memcpy(text.data(), data_.data() + offset_, length * sizeof(wchar_t));
        offset_ += length * sizeof(wchar_t);
        return true;
    }
    bool get_bytes(std::span<const std::byte>& bytes) {
        std::uint64_t size = 0;
        if (!get(size) || remaining() < size) {
            return false;
        }
        bytes = data_.subspan(offset_, static_cast<std::size_t>(size));
        offset_ += static_cast<std::size_t>(size);
        return true;
    }
    std::size_t remaining() const { return data_.size() - offset_; }

private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

}  // namespace

VolumeTrace start_volume(const std::wstring& root) {
    VolumeTrace volume;
    volume.root = platform::normalize_path(root);
    std::uint32_t error = ERROR_SUCCESS;
    const auto usn = platform::UsnVolume::open(volume.root, error);
    if (!usn) {
        volume.status = JournalStatus::Unreadable;
        volume.error = error;
        return volume;
    }
    volume.guid_path = usn->guid_path();
    volume.serial = usn->serial();
    volume.root_frn = usn->root_frn();
    platform::JournalState journal;
    error = usn->query(journal);
    if (error == ERROR_JOURNAL_NOT_ACTIVE) {
        error = usn->create(kJournalMaxSize, kJournalAllocationDelta);
        if (error == ERROR_SUCCESS) {
            volume.created_journal = true;
            error = usn->query(journal);
        } else {
            volume.status = JournalStatus::Untracked;
            volume.error = error;
            return volume;
        }
    }
    if (error != ERROR_SUCCESS) {
        // A file system without a journal says ERROR_INVALID_FUNCTION.
        volume.status =
            error == ERROR_INVALID_FUNCTION ? JournalStatus::Untracked : JournalStatus::Unreadable;
        volume.error = error;
        return volume;
    }
    volume.journal_id = journal.id;
    volume.start_usn = journal.next_usn;
    volume.next_usn = journal.next_usn;
    return volume;
}

std::vector<VolumeTrace> start_volumes() {
    std::vector<VolumeTrace> volumes;
    for (const auto& root : platform::fixed_ntfs_volumes()) {
        volumes.push_back(start_volume(root));
    }
    return volumes;
}

void drain_volume(VolumeTrace& volume, FileChangeAggregator& changes,
                  const std::function<bool(const platform::UsnRecord&)>& keep) {
    if (volume.status != JournalStatus::Ok) {
        return;
    }
    const auto fail = [&](JournalStatus status, std::uint32_t error) {
        volume.status = status;
        volume.error = error;
    };
    const auto root = find_volume_root(volume, mounted_volumes(volume));
    if (!root) {
        fail(JournalStatus::VolumeMissing, ERROR_SUCCESS);
        return;
    }
    std::uint32_t error = ERROR_SUCCESS;
    const auto usn = platform::UsnVolume::open(*root, error);
    if (!usn) {
        fail(JournalStatus::Unreadable, error);
        return;
    }
    platform::JournalState journal;
    error = usn->query(journal);
    if (const auto status = check_journal(volume, usn->serial(), error, journal);
        status != JournalStatus::Ok) {
        fail(status, error);
        return;
    }
    volume.root = *root;
    const std::uint64_t root_frn = usn->root_frn();
    std::int64_t reached = volume.next_usn;
    error = usn->read(
        volume.journal_id, volume.next_usn, journal.next_usn,
        [&](const platform::UsnRecord& record) {
            if (!is_journal_noise(record, root_frn) && (!keep || keep(record))) {
                changes.add(record);
            }
        },
        reached);
    // Records read before a failure are already in `changes`.
    volume.next_usn = reached;
    if (error != ERROR_SUCCESS) {
        fail(read_failure(error), error);
    }
}

std::vector<TraceChange> resolve_volume(const VolumeTrace& volume, const FileChangeAggregator& changes,
                                        std::int64_t trace_start) {
    std::optional<platform::UsnVolume> usn;
    const auto root = find_volume_root(volume, mounted_volumes(volume));
    if (root) {
        std::uint32_t error = ERROR_SUCCESS;
        usn = platform::UsnVolume::open(*root, error);
    }
    if (!usn) {
        NoResolver resolver;
        return resolve_file_changes(changes, volume.root_frn, volume.root, resolver, trace_start);
    }
    const std::uint64_t root_frn = usn->root_frn();
    VolumeResolver resolver(std::move(*usn));
    return resolve_file_changes(changes, root_frn, *root, resolver, trace_start);
}

std::vector<std::byte> save_checkpoint(const VolumeTrace& volume, const FileChangeAggregator& changes) {
    Writer writer;
    writer.put(kCheckpointMagic);
    writer.put(kCheckpointVersion);
    writer.put_text(volume.root);
    writer.put_text(volume.guid_path);
    writer.put(volume.serial);
    writer.put(volume.root_frn);
    writer.put(volume.journal_id);
    writer.put(volume.start_usn);
    writer.put(volume.next_usn);
    writer.put(static_cast<std::uint8_t>(volume.created_journal ? 1 : 0));
    writer.put(static_cast<std::uint8_t>(volume.status));
    writer.put(volume.error);
    writer.put_bytes(changes.save());
    return writer.finish();
}

bool load_checkpoint(std::span<const std::byte> data, VolumeTrace& volume, FileChangeAggregator& changes) {
    if (data.size() < sizeof(std::uint64_t)) {
        return false;
    }
    const auto payload = data.first(data.size() - sizeof(std::uint64_t));
    std::uint64_t checksum = 0;
    std::memcpy(&checksum, data.data() + payload.size(), sizeof(checksum));
    if (checksum != platform::fnv1a(payload.data(), payload.size())) {
        return false;
    }
    Reader reader(payload);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    VolumeTrace loaded;
    std::uint8_t created_journal = 0;
    std::uint8_t status = 0;
    std::span<const std::byte> change_bytes;
    if (!reader.get(magic) || !reader.get(version) || magic != kCheckpointMagic ||
        version != kCheckpointVersion || !reader.get_text(loaded.root) ||
        !reader.get_text(loaded.guid_path) || !reader.get(loaded.serial) || !reader.get(loaded.root_frn) ||
        !reader.get(loaded.journal_id) || !reader.get(loaded.start_usn) || !reader.get(loaded.next_usn) ||
        !reader.get(created_journal) || !reader.get(status) || !reader.get(loaded.error) ||
        !reader.get_bytes(change_bytes) || reader.remaining() != 0 ||
        status > static_cast<std::uint8_t>(JournalStatus::Unreadable)) {
        return false;
    }
    auto loaded_changes = FileChangeAggregator::load(change_bytes);
    if (!loaded_changes) {
        return false;
    }
    loaded.created_journal = created_journal != 0;
    loaded.status = static_cast<JournalStatus>(status);
    volume = std::move(loaded);
    changes = std::move(*loaded_changes);
    return true;
}

std::optional<std::wstring> find_volume_root(const VolumeTrace& volume,
                                             std::span<const MountedVolume> mounted) {
    const MountedVolume* found = nullptr;
    for (const auto& candidate : mounted) {
        if (!platform::iequals(candidate.guid_path, volume.guid_path)) {
            continue;
        }
        if (platform::iequals(candidate.root, volume.root)) {
            return candidate.root;
        }
        if (!found) {
            found = &candidate;
        }
    }
    return found ? std::optional<std::wstring>(found->root) : std::nullopt;
}

JournalStatus check_journal(const VolumeTrace& volume, std::uint32_t serial, std::uint32_t query_error,
                            const platform::JournalState& journal) {
    if (serial != volume.serial) {
        return JournalStatus::SerialChanged;
    }
    if (query_error == ERROR_JOURNAL_NOT_ACTIVE || query_error == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
        return JournalStatus::Replaced;
    }
    if (query_error != ERROR_SUCCESS) {
        return JournalStatus::Unreadable;
    }
    // A new journal starts over, so a position past its end also means it was replaced.
    if (journal.id != volume.journal_id || volume.next_usn > journal.next_usn) {
        return JournalStatus::Replaced;
    }
    if (volume.next_usn < journal.first_usn || volume.next_usn < journal.lowest_valid_usn) {
        return JournalStatus::Wrapped;
    }
    return JournalStatus::Ok;
}

JournalStatus read_failure(std::uint32_t error) {
    switch (error) {
        case ERROR_JOURNAL_ENTRY_DELETED:
            return JournalStatus::Wrapped;
        case ERROR_JOURNAL_NOT_ACTIVE:
        case ERROR_JOURNAL_DELETE_IN_PROGRESS:
        case ERROR_INVALID_PARAMETER:  // The journal id no longer matches
            return JournalStatus::Replaced;
        default:
            return JournalStatus::Unreadable;
    }
}

bool is_journal_noise(const platform::UsnRecord& record, std::uint64_t root_frn) {
    if (record.source_info & USN_SOURCE_AUXILIARY_DATA) {
        return true;  // A private stream that leaves the data alone (a virus scanner's checksum)
    }
    if ((record.frn & kSegmentMask) < kFirstUserSegment) {
        return true;
    }
    if (record.parent_frn != root_frn && (record.parent_frn & kSegmentMask) < kFirstUserSegment) {
        return true;  // $Extend\$UsnJrnl, $Extend\$ObjId, ...
    }
    return record.parent_frn == root_frn && record.name.starts_with(L'$');
}

std::string_view to_string(JournalStatus status) {
    switch (status) {
        case JournalStatus::Ok:
            return "ok";
        case JournalStatus::Untracked:
            return "untracked";
        case JournalStatus::VolumeMissing:
            return "volume-missing";
        case JournalStatus::SerialChanged:
            return "serial-changed";
        case JournalStatus::Replaced:
            return "replaced";
        case JournalStatus::Wrapped:
            return "wrapped";
        case JournalStatus::Unreadable:
            return "unreadable";
    }
    return "unknown";
}

}  // namespace avel::tracker
