#include "core/tracker/file_changes.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "core/platform/file_ops.h"
#include "core/platform/hash.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::tracker {

namespace {

using Name = FileChangeAggregator::Name;
using Entry = FileChangeAggregator::Entry;

constexpr std::uint32_t kContentReasons = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND |
                                          USN_REASON_DATA_TRUNCATION | USN_REASON_NAMED_DATA_OVERWRITE |
                                          USN_REASON_NAMED_DATA_EXTEND | USN_REASON_NAMED_DATA_TRUNCATION |
                                          USN_REASON_STREAM_CHANGE | USN_REASON_REPARSE_POINT_CHANGE;
// What leaves an item's content alone. Extended attributes are among them: Windows keeps its own
// state there (Code Integrity's signature cache on every file it checks, WSL's permissions).
constexpr std::uint32_t kMetadataReasons =
    USN_REASON_BASIC_INFO_CHANGE | USN_REASON_SECURITY_CHANGE | USN_REASON_EA_CHANGE |
    USN_REASON_OBJECT_ID_CHANGE | USN_REASON_INDEXABLE_CHANGE | USN_REASON_COMPRESSION_CHANGE |
    USN_REASON_ENCRYPTION_CHANGE | USN_REASON_INTEGRITY_CHANGE | USN_REASON_TRANSACTED_CHANGE |
    USN_REASON_DESIRED_STORAGE_CLASS_CHANGE;
constexpr std::uint32_t kRenameReasons = USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME;
// Where NTFS keeps a file deleted while still open (POSIX semantics) until its last handle closes.
constexpr std::wstring_view kPendingDeletes = LR"($Extend\$Deleted\)";

// Windows Container Isolation (wcifs: Windows Sandbox, Windows containers). The system's files and
// folders are placeholders for a read-only base layer, which NTFS gets as new items, with new
// reference numbers, the first time each is used: a folder when it is listed, a file when it is
// read, and a program also gets Code Integrity's catalog hint, an extended attribute. That changes
// nothing anyone sees. Below the filter a placeholder is an offline reparse point, which is what
// the journal's records say; above it, Avel included, neither the reparse point nor its tag shows.
constexpr std::uint32_t kPlaceholderReasons = USN_REASON_FILE_CREATE | USN_REASON_REPARSE_POINT_CHANGE |
                                              USN_REASON_DATA_EXTEND | USN_REASON_EA_CHANGE |
                                              USN_REASON_CLOSE;

// The records look like NTFS getting a placeholder: the first creates the item as an offline
// reparse point. Not a cloud file's placeholder (OneDrive's), which is new and is recalled on access.
bool looks_like_placeholder(std::uint32_t first_reasons, std::uint32_t attributes) {
    constexpr std::uint32_t kCreated = USN_REASON_FILE_CREATE | USN_REASON_REPARSE_POINT_CHANGE;
    constexpr std::uint32_t kPlaceholder = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE;
    constexpr std::uint32_t kCloud = FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    return (first_reasons & kCreated) == kCreated && (attributes & kPlaceholder) == kPlaceholder &&
           !(attributes & kCloud);
}

// The records look like a container's tombstone, which hides a base-layer item deleted or renamed
// away: a new hidden, system reparse point under the item's name, never offline. Programs don't
// see it; the item's own records already tell of the deletion or rename.
bool looks_like_tombstone(std::uint32_t first_reasons, std::uint32_t attributes) {
    constexpr std::uint32_t kCreated = USN_REASON_FILE_CREATE | USN_REASON_REPARSE_POINT_CHANGE;
    constexpr std::uint32_t kTombstone =
        FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    return (first_reasons & kCreated) == kCreated && (attributes & kTombstone) == kTombstone &&
           !(attributes & FILE_ATTRIBUTE_OFFLINE);
}
constexpr std::size_t kMaxDepth = 512;

// --- Checkpoint -------------------------------------------------------------------------------

constexpr std::uint32_t kCheckpointMagic = 0x43465641;  // "AVFC"
constexpr std::uint32_t kCheckpointVersion = 1;
constexpr std::uint8_t kHasFirst = 1;
constexpr std::uint8_t kHasLast = 2;
constexpr std::size_t kNameSize = sizeof(std::uint64_t) + sizeof(std::uint32_t);  // Smallest
constexpr std::size_t kEntrySize =
    sizeof(std::uint64_t) + 4 * sizeof(std::uint32_t) + 1 + sizeof(std::uint32_t);
constexpr std::uint32_t kMaxNameLength = 32767;

class Writer {
public:
    template <typename T>
    void put(const T& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        bytes_.insert(bytes_.end(), bytes, bytes + sizeof(T));
    }
    void put_name(const Name& name) {
        put(name.parent);
        put(static_cast<std::uint32_t>(name.name.size()));
        const auto* bytes = reinterpret_cast<const std::byte*>(name.name.data());
        bytes_.insert(bytes_.end(), bytes, bytes + name.name.size() * sizeof(wchar_t));
    }
    // The bytes followed by their checksum.
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
    bool get_name(Name& name) {
        std::uint32_t length = 0;
        if (!get(name.parent) || !get(length) || length > kMaxNameLength ||
            remaining() < length * sizeof(wchar_t)) {
            return false;
        }
        name.name.resize(length);
        std::memcpy(name.name.data(), data_.data() + offset_, length * sizeof(wchar_t));
        offset_ += length * sizeof(wchar_t);
        return true;
    }
    std::size_t remaining() const { return data_.size() - offset_; }

private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

// --- Paths ------------------------------------------------------------------------------------

struct Path {
    std::wstring text;
    bool resolved = true;
};

Path join(const Path& folder, std::wstring_view name) {
    Path path = folder;
    if (!path.text.ends_with(L'\\')) {
        path.text += L'\\';
    }
    path.text += name;
    return path;
}

bool same_name(const Name& a, const Name& b) {
    return a.parent == b.parent && platform::iequals(a.name, b.name);
}

// The name an item had at the end of the trace, and at its start, as far as its records tell.
const Name* name_at_stop(const Entry& entry) {
    return entry.last ? &*entry.last : entry.first ? &*entry.first : nullptr;
}
const Name* name_at_start(const Entry& entry) {
    return entry.first ? &*entry.first : entry.last ? &*entry.last : nullptr;
}

// Paths of items at the trace's start and end. A folder with records takes its name from them;
// one without records hasn't changed itself, so the resolver says where it is (at the end) or
// what it's called (at the start, when an ancestor may have been renamed since).
class Paths {
public:
    Paths(const FileChangeAggregator& changes, std::uint64_t root_frn, const std::wstring& root,
          PathResolver& resolver)
        : entries_(changes.entries()),
          root_frn_(root_frn),
          root_(platform::normalize_path(root)),
          resolver_(resolver) {
        for (const auto& [frn, entry] : entries_) {
            if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) && (entry.reasons & kRenameReasons)) {
                folder_renamed_ = true;
                break;
            }
        }
    }

    Path at_stop(const Name& name) { return join(folder_at_stop(name.parent, 0), name.name); }
    Path at_start(const Name& name) { return join(folder_at_start(name.parent, 0), name.name); }

private:
    // "C:\?": a folder that couldn't be found.
    Path unknown() const { return {root_ + L"?", false}; }

    Path folder_at_stop(std::uint64_t folder, std::size_t depth) {
        if (folder == root_frn_) {
            return {root_, true};
        }
        if (const auto cached = at_stop_.find(folder); cached != at_stop_.end()) {
            return cached->second;
        }
        if (depth > kMaxDepth) {
            return unknown();
        }
        Path path;
        const auto entry = entries_.find(folder);
        if (const Name* name = entry != entries_.end() ? name_at_stop(entry->second) : nullptr) {
            path = join(folder_at_stop(name->parent, depth + 1), name->name);
        } else if (const auto now = resolver_.path_now(folder)) {
            path = {platform::normalize_path(*now), true};
        } else {
            path = unknown();
        }
        at_stop_.emplace(folder, path);
        return path;
    }

    Path folder_at_start(std::uint64_t folder, std::size_t depth) {
        if (folder == root_frn_) {
            return {root_, true};
        }
        if (const auto cached = at_start_.find(folder); cached != at_start_.end()) {
            return cached->second;
        }
        if (depth > kMaxDepth) {
            return unknown();
        }
        Path path;
        const auto entry = entries_.find(folder);
        if (const Name* name = entry != entries_.end() ? name_at_start(entry->second) : nullptr) {
            path = join(folder_at_start(name->parent, depth + 1), name->name);
        } else if (!folder_renamed_) {
            // No folder moved during the trace, so the path now is the path it had then.
            path = folder_at_stop(folder, depth);
        } else if (const auto now = resolver_.name_now(folder)) {
            path = join(folder_at_start(now->first, depth + 1), now->second);
        } else {
            path = unknown();
        }
        at_start_.emplace(folder, path);
        return path;
    }

    const std::unordered_map<std::uint64_t, Entry>& entries_;
    std::uint64_t root_frn_;
    std::wstring root_;
    PathResolver& resolver_;
    bool folder_renamed_ = false;
    std::unordered_map<std::uint64_t, Path> at_stop_;
    std::unordered_map<std::uint64_t, Path> at_start_;
};

// --- Net changes ------------------------------------------------------------------------------

struct Item {
    std::uint64_t frn = 0;
    std::uint64_t parent = 0;  // The folder it is in: at the end, or for Deleted at the start
    ChangeKind change = ChangeKind::Created;
    Path path;
    Path old_path;                 // Renamed
    std::uint64_t old_parent = 0;  // Renamed: the folder it was in at the start
    bool directory = false;
    bool replaced = false;
    bool link = false;           // A hard link added to an item that existed before
    bool metadata_only = false;  // Modified: only its attributes, times, security or extended attributes
    std::uint32_t first_reasons = 0;
    std::uint64_t contents = 0;
    std::vector<std::size_t> inside;  // A created root: the items `contents` counts
    bool merged = false;              // The Deleted half of a replacement
    bool hidden = false;  // Inside a created root, or a created folder that holds older items
};

std::wstring path_key(const Path& path) { return platform::to_lower(path.text); }

// Marks the created items that are inside another created item, counting them in its contents,
// and hides created folders that hold items which existed before the trace (moved in, or older
// than the folder's records suggest): those can't be offered whole, so their created children are
// listed one by one instead.
void assign_roots(std::vector<Item>& items) {
    std::unordered_map<std::uint64_t, std::size_t> created_folders;
    for (std::size_t i = 0; i < items.size(); ++i) {
        items[i].hidden = false;
        items[i].contents = 0;
        items[i].inside.clear();
        if (items[i].change == ChangeKind::Created && items[i].directory && !items[i].merged) {
            created_folders.emplace(items[i].frn, i);
        }
    }
    std::unordered_set<std::uint64_t> mixed;
    for (const auto& item : items) {
        if (item.merged || (item.change != ChangeKind::Renamed && item.change != ChangeKind::Modified)) {
            continue;
        }
        std::uint64_t parent = item.parent;
        for (std::size_t depth = 0; depth < kMaxDepth; ++depth) {
            const auto folder = created_folders.find(parent);
            if (folder == created_folders.end() || !mixed.insert(parent).second) {
                break;
            }
            parent = items[folder->second].parent;
        }
    }
    for (std::size_t index = 0; index < items.size(); ++index) {
        auto& item = items[index];
        if (item.change != ChangeKind::Created || item.merged) {
            continue;
        }
        std::optional<std::size_t> root;
        std::uint64_t parent = item.parent;
        for (std::size_t depth = 0; depth < kMaxDepth; ++depth) {
            const auto folder = created_folders.find(parent);
            if (folder == created_folders.end() || mixed.contains(parent)) {
                break;
            }
            root = folder->second;
            parent = items[folder->second].parent;
        }
        if (root) {
            item.hidden = true;
            ++items[*root].contents;
            items[*root].inside.push_back(index);
        }
    }
    for (const auto frn : mixed) {
        items[created_folders.at(frn)].hidden = true;
    }
}

// A created root folder's inventory: the items assign_roots counted in it, relative to it. Nullopt
// when one's path is unknown or there are more than kMaxContainedItems.
std::optional<std::vector<leftovers::ContainedItem>> inventory_of(const std::vector<Item>& items,
                                                                  const Item& root) {
    if (!root.path.resolved || root.inside.size() > leftovers::kMaxContainedItems) {
        return std::nullopt;
    }
    const std::wstring base = platform::normalize_path(root.path.text);
    std::vector<leftovers::ContainedItem> inside;
    inside.reserve(root.inside.size());
    for (const std::size_t index : root.inside) {
        const Item& item = items[index];
        const std::wstring path = platform::normalize_path(item.path.text);
        if (!item.path.resolved || path.size() <= base.size() + 1 || path[base.size()] != L'\\' ||
            !platform::istarts_with(path, base)) {
            return std::nullopt;
        }
        inside.push_back({path.substr(base.size() + 1), std::nullopt, item.directory});
    }
    std::sort(inside.begin(), inside.end());
    return inside;
}

}  // namespace

// --- FileChangeAggregator -----------------------------------------------------------------------

void FileChangeAggregator::add(const platform::UsnRecord& record) {
    auto [it, inserted] = entries_.try_emplace(record.frn);
    Entry& entry = it->second;
    if (inserted) {
        entry.first_reasons = record.reason;
    }
    // Reasons accumulate from an item's first open to its last close; the new ones are this
    // record's own change.
    const std::uint32_t added = record.reason & ~entry.open_reasons;
    entry.open_reasons = (record.reason & USN_REASON_CLOSE) ? 0 : entry.open_reasons | record.reason;
    entry.reasons |= record.reason;
    entry.attributes |= record.attributes;

    Name name{record.parent_frn, std::wstring(record.name)};
    const bool touched_link = std::any_of(entry.links.begin(), entry.links.end(),
                                          [&](const Name& link) { return same_name(link, name); });
    if (added & USN_REASON_HARD_LINK_CHANGE) {
        // Names the link added or removed (one record, with CLOSE), not the name the item is
        // otherwise known by.
        if (!touched_link) {
            entry.links.push_back(std::move(name));
        }
        return;
    }
    // Later records name the link the item was opened through. A link the trace added, or a
    // record carrying a link change along, can't tell the name the item had before the trace.
    const bool old_name = (record.reason & USN_REASON_RENAME_OLD_NAME) != 0;
    if (!entry.first && !touched_link && (old_name || !(record.reason & USN_REASON_HARD_LINK_CHANGE))) {
        entry.first = name;
    }
    if (!old_name) {
        entry.last = std::move(name);
    }
}

std::vector<std::byte> FileChangeAggregator::save() const {
    std::vector<std::uint64_t> order;
    order.reserve(entries_.size());
    for (const auto& [frn, entry] : entries_) {
        order.push_back(frn);
    }
    std::sort(order.begin(), order.end());

    Writer writer;
    writer.put(kCheckpointMagic);
    writer.put(kCheckpointVersion);
    writer.put(static_cast<std::uint64_t>(order.size()));
    for (const auto frn : order) {
        const Entry& entry = entries_.at(frn);
        writer.put(frn);
        writer.put(entry.reasons);
        writer.put(entry.first_reasons);
        writer.put(entry.open_reasons);
        writer.put(entry.attributes);
        writer.put(static_cast<std::uint8_t>((entry.first ? kHasFirst : 0) | (entry.last ? kHasLast : 0)));
        if (entry.first) {
            writer.put_name(*entry.first);
        }
        if (entry.last) {
            writer.put_name(*entry.last);
        }
        writer.put(static_cast<std::uint32_t>(entry.links.size()));
        for (const auto& link : entry.links) {
            writer.put_name(link);
        }
    }
    return writer.finish();
}

std::optional<FileChangeAggregator> FileChangeAggregator::load(std::span<const std::byte> data) {
    if (data.size() < sizeof(std::uint64_t)) {
        return std::nullopt;
    }
    const auto payload = data.first(data.size() - sizeof(std::uint64_t));
    std::uint64_t checksum = 0;
    std::memcpy(&checksum, data.data() + payload.size(), sizeof(checksum));
    if (checksum != platform::fnv1a(payload.data(), payload.size())) {
        return std::nullopt;
    }
    Reader reader(payload);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t count = 0;
    if (!reader.get(magic) || !reader.get(version) || !reader.get(count) || magic != kCheckpointMagic ||
        version != kCheckpointVersion || count > reader.remaining() / kEntrySize) {
        return std::nullopt;
    }
    FileChangeAggregator result;
    result.entries_.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t frn = 0;
        Entry entry;
        std::uint8_t flags = 0;
        std::uint32_t links = 0;
        if (!reader.get(frn) || !reader.get(entry.reasons) || !reader.get(entry.first_reasons) ||
            !reader.get(entry.open_reasons) || !reader.get(entry.attributes) || !reader.get(flags)) {
            return std::nullopt;
        }
        if (flags & kHasFirst) {
            if (!reader.get_name(entry.first.emplace())) {
                return std::nullopt;
            }
        }
        if (flags & kHasLast) {
            if (!reader.get_name(entry.last.emplace())) {
                return std::nullopt;
            }
        }
        if (!reader.get(links) || links > reader.remaining() / kNameSize) {
            return std::nullopt;
        }
        entry.links.resize(links);
        for (auto& link : entry.links) {
            if (!reader.get_name(link)) {
                return std::nullopt;
            }
        }
        if (!result.entries_.emplace(frn, std::move(entry)).second) {
            return std::nullopt;
        }
    }
    if (reader.remaining() != 0) {
        return std::nullopt;
    }
    return result;
}

// --- Resolvers ----------------------------------------------------------------------------------

std::optional<std::uint64_t> PathResolver::id_at(const std::wstring& path) {
    const auto item = platform::open_item(path, FILE_READ_ATTRIBUTES);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!item || !::GetFileInformationByHandle(item.get(), &info)) {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

std::optional<std::int64_t> PathResolver::created_at(const std::wstring& path) {
    const auto info = platform::item_info(path);
    if (!info) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(info->created.dwHighDateTime) << 32) |
                                     info->created.dwLowDateTime);
}

std::optional<std::uint32_t> PathResolver::attributes_at(const std::wstring& path) {
    const auto info = platform::item_info(path);
    return info ? std::optional<std::uint32_t>(info->attributes) : std::nullopt;
}

VolumeResolver::VolumeResolver(platform::UsnVolume volume) : volume_(std::move(volume)) {}

std::optional<std::wstring> VolumeResolver::path_now(std::uint64_t folder) {
    if (const auto cached = paths_.find(folder); cached != paths_.end()) {
        return cached->second;
    }
    auto path = volume_.path_of(folder);
    if (!path) {
        if (const auto index = listed(folder)) {
            if (auto listed_path = folders_.path_of(*index); !listed_path.empty()) {
                path = std::move(listed_path);
            }
        }
    }
    paths_.emplace(folder, path);
    return path;
}

std::optional<std::pair<std::uint64_t, std::wstring>> VolumeResolver::name_now(std::uint64_t folder) {
    if (const auto cached = names_.find(folder); cached != names_.end()) {
        return cached->second;
    }
    auto name = volume_.name_of(folder);
    if (!name) {
        if (const auto index = listed(folder)) {
            const auto& listed_folder = folders_.folders()[*index];
            name.emplace(listed_folder.parent, listed_folder.name);
        }
    }
    names_.emplace(folder, name);
    return name;
}

std::optional<std::size_t> VolumeResolver::listed(std::uint64_t folder) {
    // Folders that can't be opened by id (rare) are looked up in one read of the whole volume.
    if (!folders_read_) {
        folders_read_ = true;
        if (platform::VolumeFolders::read(volume_.root(), folders_) == ERROR_SUCCESS) {
            folder_index_.reserve(folders_.folders().size());
            for (std::size_t i = 0; i < folders_.folders().size(); ++i) {
                folder_index_.emplace(folders_.folders()[i].id, i);
            }
        }
    }
    const auto found = folder_index_.find(folder);
    return found != folder_index_.end() ? std::optional<std::size_t>(found->second) : std::nullopt;
}

// --- Resolution ---------------------------------------------------------------------------------

std::vector<TraceChange> resolve_file_changes(const FileChangeAggregator& changes, std::uint64_t root_frn,
                                              const std::wstring& root, PathResolver& resolver,
                                              std::int64_t trace_start) {
    Paths paths(changes, root_frn, root, resolver);
    std::wstring pending_deletes = platform::normalize_path(root);
    if (!pending_deletes.ends_with(L'\\')) {
        pending_deletes += L'\\';
    }
    pending_deletes += kPendingDeletes;
    std::vector<Item> items;
    const auto add = [&](std::uint64_t frn, const Entry& entry, ChangeKind change, Path path,
                         std::uint64_t parent) {
        Item item;
        item.frn = frn;
        item.parent = parent;
        item.change = change;
        item.path = std::move(path);
        item.directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.first_reasons = entry.first_reasons;
        items.push_back(std::move(item));
        return items.size() - 1;
    };

    // Each item's net effect.
    for (const auto& [frn, entry] : changes.entries()) {
        std::uint32_t reasons = entry.reasons;
        bool created = (reasons & USN_REASON_FILE_CREATE) != 0;
        // Its creation was recorded during the trace: FILE_CREATE alone in its first record. One
        // that comes with other reasons was left over from an open that began before the trace
        // (reasons accumulate until the last handle closes), or it is a container's placeholder.
        const bool creation_seen = entry.first_reasons == USN_REASON_FILE_CREATE;
        const Name* start = name_at_start(entry);
        const Name* stop = name_at_stop(entry);
        // Deleted before the trace (already in $Extend\$Deleted), its last handle closed during it.
        // $Deleted is never renamed, so where it is now says where it was.
        if (start && platform::istarts_with(paths.at_stop(*start).text, pending_deletes)) {
            continue;
        }
        // Deleted, or still waiting in $Extend\$Deleted for its last handle to close.
        const bool deleted = (reasons & USN_REASON_FILE_DELETE) != 0 ||
                             (stop && platform::istarts_with(paths.at_stop(*stop).text, pending_deletes));
        // A container's tombstone, gone again or not where programs look: nothing of its own.
        if (created && looks_like_tombstone(entry.first_reasons, entry.attributes) &&
            (deleted || !stop || resolver.id_at(paths.at_stop(*stop).text) != frn)) {
            continue;
        }
        if (created && deleted) {
            // Gone again; but an item that was there before the trace is deleted.
            if (!creation_seen && start) {
                add(frn, entry, ChangeKind::Deleted, paths.at_start(*start), start->parent);
            }
            continue;
        }
        if (created && stop && looks_like_placeholder(entry.first_reasons, entry.attributes)) {
            // A placeholder NTFS got from a container's base layer: it was there before (it is
            // older than the trace), and programs see no reparse point on it. Only what changed
            // besides is a change.
            const Path path = paths.at_stop(*stop);
            const auto created_at = path.resolved ? resolver.created_at(path.text) : std::nullopt;
            const auto attributes = path.resolved ? resolver.attributes_at(path.text) : std::nullopt;
            if (created_at && *created_at < trace_start && attributes &&
                !(*attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                reasons &= ~kPlaceholderReasons;
                created = false;
            }
        }
        if (created && !creation_seen && entry.links.empty() && start && stop && (reasons & kRenameReasons)) {
            // Open since before the trace and renamed during it (see "items open across the start"
            // below): if it is older than the trace, it had its name at the start, and its rename
            // is one like any other's.
            const Path path = paths.at_stop(*stop);
            const auto created_at = path.resolved ? resolver.created_at(path.text) : std::nullopt;
            if (created_at && *created_at < trace_start) {
                reasons &= ~USN_REASON_FILE_CREATE;
                created = false;
            }
        }
        if (created) {
            if (entry.links.empty()) {
                if (stop) {
                    add(frn, entry, ChangeKind::Created, paths.at_stop(*stop), stop->parent);
                }
                continue;
            }
            // Hard links were added or removed: every name it had that still leads to it.
            std::vector<const Name*> names;
            for (const Name* name : {start, stop}) {
                if (name) {
                    names.push_back(name);
                }
            }
            for (const auto& link : entry.links) {
                names.push_back(&link);
            }
            std::unordered_set<std::wstring> seen;
            bool found = false;
            for (const Name* name : names) {
                Path path = paths.at_stop(*name);
                if (path.resolved && seen.insert(path_key(path)).second && resolver.id_at(path.text) == frn) {
                    add(frn, entry, ChangeKind::Created, std::move(path), name->parent);
                    found = true;
                }
            }
            if (!found && stop) {
                add(frn, entry, ChangeKind::Created, paths.at_stop(*stop), stop->parent);
            }
            continue;
        }
        if (deleted) {
            if (start) {
                add(frn, entry, ChangeKind::Deleted, paths.at_start(*start), start->parent);
            }
            continue;
        }
        // Hard links added to an item that existed before are new names of their own.
        std::unordered_set<std::wstring> new_links;
        for (const auto& link : entry.links) {
            if (entry.first && same_name(link, *entry.first)) {
                continue;  // A name it had at the start, removed and added again
            }
            Path path = paths.at_stop(link);
            if (path.resolved && resolver.id_at(path.text) == frn &&
                new_links.insert(path_key(path)).second) {
                const auto index = add(frn, entry, ChangeKind::Created, std::move(path), link.parent);
                items[index].directory = false;
                items[index].link = true;
            }
        }
        if (start && stop) {
            Path before = paths.at_start(*start);
            Path after = paths.at_stop(*stop);
            if ((reasons & kRenameReasons) && before.text != after.text) {
                const auto index = add(frn, entry, ChangeKind::Renamed, std::move(after), stop->parent);
                items[index].old_path = std::move(before);
                items[index].old_parent = start->parent;
            } else if (reasons & (kContentReasons | kMetadataReasons)) {
                // Written through a link the trace added: the change belongs to the name it had.
                std::optional<std::size_t> index;
                if (!new_links.contains(path_key(after))) {
                    index = add(frn, entry, ChangeKind::Modified, std::move(after), stop->parent);
                } else if (entry.first) {
                    index = add(frn, entry, ChangeKind::Modified, std::move(before), start->parent);
                }
                if (index) {
                    items[*index].metadata_only = !(reasons & kContentReasons);
                }
            }
        }
    }

    // A created item where another was at the start replaced it (temp-then-rename, MoveFileEx
    // with REPLACE_EXISTING, Windows Installer moving the old file aside): one modified item. An
    // item that existed before is never offered for removal as created. So does an older file
    // renamed there (WMI's performance adapter renames WmiApRpl_new.ini over WmiApRpl.ini, which
    // it may have kept open since before the trace); its old name is gone then.
    std::unordered_map<std::wstring, std::size_t> previous;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].change == ChangeKind::Deleted && items[i].path.resolved) {
            previous.emplace(path_key(items[i].path), i);
        } else if (items[i].change == ChangeKind::Renamed && items[i].old_path.resolved) {
            previous.emplace(path_key(items[i].old_path), i);
        }
    }
    std::vector<std::size_t> vacated;  // Renamed files that replaced another
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        const bool renamed = item.change == ChangeKind::Renamed && !item.directory;
        if ((item.change != ChangeKind::Created && !renamed) || !item.path.resolved) {
            continue;
        }
        const auto found = previous.find(path_key(item.path));
        if (found == previous.end() || items[found->second].directory != item.directory) {
            continue;
        }
        item.change = ChangeKind::Modified;
        item.replaced = true;
        if (renamed) {
            vacated.push_back(i);
        }
        // The item moved aside still exists, so it stays listed as renamed.
        if (items[found->second].change == ChangeKind::Deleted) {
            items[found->second].merged = true;
        }
        previous.erase(found);
    }
    // A renamed file's old name is deleted, unless another item took its place in turn.
    for (const auto i : vacated) {
        const auto found = previous.find(path_key(items[i].old_path));
        if (found == previous.end() || found->second != i) {
            continue;
        }
        Item gone = items[i];
        gone.change = ChangeKind::Deleted;
        gone.path = std::move(gone.old_path);
        gone.old_path = {};
        gone.parent = gone.old_parent;
        gone.replaced = false;
        items.push_back(std::move(gone));
    }

    // Roots only, and items that were already there when the trace started: a FILE_CREATE reason
    // can be left over from an open that began before the trace, so a created root that's older
    // than the trace and whose own creation record wasn't seen is only modified. Its children may
    // be roots then, so this repeats until nothing changes.
    std::vector<bool> checked(items.size(), false);
    for (bool changed = true; changed;) {
        changed = false;
        assign_roots(items);
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto& item = items[i];
            if (item.change != ChangeKind::Created || item.hidden || item.merged || checked[i]) {
                continue;
            }
            checked[i] = true;
            if (item.first_reasons == USN_REASON_FILE_CREATE || item.link || !item.path.resolved) {
                continue;  // Its creation was recorded during the trace, or it's a new name of an old file
            }
            const auto created = resolver.created_at(item.path.text);
            if (created && *created < trace_start) {
                item.change = ChangeKind::Modified;
                changed = true;
            }
        }
    }

    std::vector<std::pair<std::wstring, TraceChange>> sorted;
    for (const auto& item : items) {
        if (item.merged || item.hidden) {
            continue;
        }
        TraceChange change;
        change.change = item.change;
        change.location = leftovers::FileLocation{platform::normalize_path(item.path.text), item.directory};
        if (item.change == ChangeKind::Renamed) {
            change.old_path = platform::normalize_path(item.old_path.text);
        }
        change.contents = item.contents;
        if (item.change == ChangeKind::Created && item.directory) {
            change.inside = inventory_of(items, item);
        }
        change.replaced = item.replaced;
        if (!item.path.resolved || (item.change == ChangeKind::Renamed && !item.old_path.resolved)) {
            change.noise = NoiseReason::Unresolved;
        } else if (item.metadata_only) {
            change.noise = NoiseReason::MetadataOnly;
        }
        sorted.emplace_back(path_key(item.path), std::move(change));
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.change < b.second.change;
    });
    std::vector<TraceChange> result;
    result.reserve(sorted.size());
    for (auto& [key, change] : sorted) {
        result.push_back(std::move(change));
    }
    return result;
}

}  // namespace avel::tracker
