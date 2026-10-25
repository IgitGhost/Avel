#include "core/tracker/registry_snapshot.h"

#include <wil/resource.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "core/platform/elevation.h"
#include "core/platform/hash.h"
#include "core/platform/nt_registry.h"
#include "core/platform/text.h"

namespace avel::tracker {

// Fills a RegistrySnapshot, whose members are private so nothing else makes an inconsistent one.
struct SnapshotAccess {
    static std::vector<RegistrySnapshot::Root>& roots(RegistrySnapshot& snapshot) { return snapshot.roots_; }
    static std::vector<SnapKey>& keys(RegistrySnapshot& snapshot) { return snapshot.keys_; }
    static std::vector<SnapValue>& values(RegistrySnapshot& snapshot) { return snapshot.values_; }
    static std::vector<std::uint32_t>& name_offsets(RegistrySnapshot& snapshot) {
        return snapshot.name_offsets_;
    }
    static const std::vector<std::uint32_t>& name_offsets(const RegistrySnapshot& snapshot) {
        return snapshot.name_offsets_;
    }
    static std::vector<wchar_t>& name_chars(RegistrySnapshot& snapshot) { return snapshot.name_chars_; }
    static const std::vector<wchar_t>& name_chars(const RegistrySnapshot& snapshot) {
        return snapshot.name_chars_;
    }
};

namespace {

using platform::Hive;
using platform::RegistryAddress;

constexpr std::uint32_t kNone = 0xFFFFFFFF;

// A key with this many children, this close to its root (Classes, CLSID, Interface,
// Installer\Components and the like), has them read by several jobs, in ranges of kSplitRange.
constexpr std::uint32_t kSplitChildren = 256;
constexpr std::uint32_t kSplitDepth = 4;
constexpr std::uint32_t kSplitRange = 128;

// ---- The native API, called directly: a trace opens and lists every key in the registry ----

constexpr LONG kStatusSuccess = 0;
constexpr LONG kStatusBufferOverflow = static_cast<LONG>(0x80000005);
constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001A);
constexpr LONG kStatusBufferTooSmall = static_cast<LONG>(0xC0000023);
constexpr LONG kStatusAccessDenied = static_cast<LONG>(0xC0000022);
constexpr ULONG kObjCaseInsensitive = 0x40;
constexpr ULONG kObjOpenLink = 0x100;
constexpr int kKeyBasicInformation = 0;
constexpr int kKeyFullInformation = 2;
constexpr int kKeyValueFullInformation = 1;

// Enough to read a key's values and list its children; asking for no more avoids needless denials.
constexpr ACCESS_MASK kReadAccess = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS;

struct CountedString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

struct ObjectAttributes {
    ULONG length;
    HANDLE root;
    CountedString* name;
    ULONG attributes;
    PVOID security_descriptor;
    PVOID security_quality_of_service;
};

struct KeyBasicInformation {
    LARGE_INTEGER last_write_time;
    ULONG title_index;
    ULONG name_length;
    WCHAR name[1];
};

struct KeyFullInformation {
    LARGE_INTEGER last_write_time;
    ULONG title_index;
    ULONG class_offset;
    ULONG class_length;
    ULONG subkeys;
    ULONG max_name_length;
    ULONG max_class_length;
    ULONG values;
    ULONG max_value_name_length;
    ULONG max_value_data_length;
    WCHAR class_name[1];
};

struct KeyValueFullInformation {
    ULONG title_index;
    ULONG type;
    ULONG data_offset;
    ULONG data_length;
    ULONG name_length;
    WCHAR name[1];
};

using NtOpenKeyExFunction = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, ObjectAttributes*, ULONG);
using NtEnumerateFunction = LONG(NTAPI*)(HANDLE, ULONG, int, PVOID, ULONG, PULONG);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(LONG);

struct NtApi {
    NtOpenKeyExFunction open_key;
    NtEnumerateFunction enumerate_key;
    NtEnumerateFunction enumerate_value;
    RtlNtStatusToDosErrorFunction to_win32;
};

const NtApi& nt() {
    static const NtApi api = [] {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        return NtApi{reinterpret_cast<NtOpenKeyExFunction>(::GetProcAddress(ntdll, "NtOpenKeyEx")),
                     reinterpret_cast<NtEnumerateFunction>(::GetProcAddress(ntdll, "NtEnumerateKey")),
                     reinterpret_cast<NtEnumerateFunction>(::GetProcAddress(ntdll, "NtEnumerateValueKey")),
                     reinterpret_cast<RtlNtStatusToDosErrorFunction>(
                         ::GetProcAddress(ntdll, "RtlNtStatusToDosError"))};
    }();
    return api;
}

LSTATUS win32_error(LONG status) { return static_cast<LSTATUS>(nt().to_win32(status)); }

bool too_small(LONG status) { return status == kStatusBufferOverflow || status == kStatusBufferTooSmall; }

std::int64_t ticks(const FILETIME& time) {
    return static_cast<std::int64_t>(std::uint64_t{time.dwHighDateTime} << 32 | time.dwLowDateTime);
}

// Opens `name` below `parent` as itself, never a link's target. With `backup`, a key that denies
// access is opened again with backup intent (SeBackupPrivilege must be enabled).
LSTATUS open_subkey(HKEY parent, std::wstring_view name, bool backup, wil::unique_hkey& key) {
    if (name.size() * sizeof(wchar_t) > 0xFFFE) {
        return ERROR_INVALID_PARAMETER;
    }
    const auto bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    CountedString counted{bytes, bytes, const_cast<PWSTR>(name.data())};
    ObjectAttributes attributes{sizeof(ObjectAttributes),           parent,  &counted,
                                kObjCaseInsensitive | kObjOpenLink, nullptr, nullptr};
    HANDLE handle = nullptr;
    LONG status = nt().open_key(&handle, kReadAccess, &attributes, 0);
    if (status == kStatusAccessDenied && backup) {
        status = nt().open_key(&handle, kReadAccess, &attributes, REG_OPTION_BACKUP_RESTORE);
    }
    if (status < 0) {
        return win32_error(status);
    }
    key.reset(static_cast<HKEY>(handle));
    return ERROR_SUCCESS;
}

// A root is opened normally: its own path may pass through a link (CurrentControlSet).
LSTATUS open_root(const SnapshotRoot& root, bool backup, wil::unique_hkey& key) {
    if (!root.parent) {
        return ERROR_INVALID_HANDLE;
    }
    HKEY handle = nullptr;
    LSTATUS status =
        ::RegOpenKeyExW(root.parent, root.subpath.c_str(), 0, kReadAccess | KEY_WOW64_64KEY, &handle);
    if (status == ERROR_ACCESS_DENIED && backup) {
        status = ::RegOpenKeyExW(root.parent, root.subpath.c_str(), REG_OPTION_BACKUP_RESTORE,
                                 kReadAccess | KEY_WOW64_64KEY, &handle);
    }
    if (status == ERROR_SUCCESS) {
        key.reset(handle);
    }
    return status;
}

// What a key's parent's listing says about it. A key with nothing in it is never opened, and its
// values are read with a buffer sized to them: every call costs in proportion to the buffer it is
// given (the kernel checks all of it), about 1 us for 1 KB but 14 us for 1 MB.
struct KeyCounts {
    std::uint32_t subkeys = 0;
    std::uint32_t values = 0;
    std::uint32_t value_room = 0;  // Bytes that hold any one of its values
    bool known = false;            // false: open the key and ask
};

constexpr ULONG kSmallRead = 1024;          // A key's name (at most 255 characters) and the fixed fields
constexpr ULONG kLargestFirstRead = 16384;  // Larger values are read with the size the first try reports

std::uint32_t value_room(std::uint64_t name_bytes, std::uint64_t data_bytes) {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(sizeof(KeyValueFullInformation) + name_bytes + data_bytes + 16, kNone));
}

KeyCounts counts_of(const platform::KeyInfo& info) {
    return {info.subkeys, info.values,
            value_room(std::uint64_t{info.max_value_name_length} * sizeof(wchar_t), info.max_value_data_size),
            true};
}

// Room for one enumeration result, reused for every key a worker reads.
class EnumBuffer {
public:
    const void* data() const { return storage_.data(); }

    // Calls `call(buffer, length, &needed)` with `wanted` bytes (at least kSmallRead), and again with
    // what it needs when that is too small.
    template <typename Call>
    LONG run(ULONG wanted, Call call) {
        ULONG length = std::max(wanted, kSmallRead);
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (storage_.size() * sizeof(std::uint64_t) < length) {
                storage_.resize(length / sizeof(std::uint64_t) + 1);
            }
            ULONG needed = 0;
            const LONG status = call(storage_.data(), length, &needed);
            if (!too_small(status) || needed <= length) {
                return status;
            }
            length = needed;
        }
        return kStatusBufferTooSmall;
    }

private:
    std::vector<std::uint64_t> storage_ = std::vector<std::uint64_t>(512);  // 4 KB, 8-byte aligned
};

// ---- Names ----

// Ordinal and ignoring case, like the registry itself; 0 only for names the registry takes as
// the same name.
int compare_ignoring_case(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) {
        return a.empty() ? (b.empty() ? 0 : -1) : 1;
    }
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                  TRUE) -
           CSTR_EQUAL;
}

int compare_names(std::wstring_view a, std::wstring_view b) { return compare_registry_names(a, b); }

// The registry lists stable keys in order, then volatile keys in order: two sorted runs to merge
// rather than sort.
template <typename Iterator, typename Less>
void sort_names(Iterator begin, Iterator end, Less less) {
    const Iterator split = std::is_sorted_until(begin, end, less);
    if (split == end) {
        return;
    }
    if (std::is_sorted(split, end, less)) {
        std::inplace_merge(begin, split, end, less);
    } else {
        std::sort(begin, end, less);
    }
}

// Deduplicated names, stored back to back.
class NamePool {
public:
    std::uint32_t intern(std::wstring_view name) {
        const std::uint64_t full = platform::fnv1a(name.data(), name.size() * sizeof(wchar_t));
        const auto hash = static_cast<std::uint32_t>(full ^ (full >> 32));
        if ((std::size_t{size()} + 1) * 2 > slots_.size()) {
            grow();
        }
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
            Slot& slot = slots_[i];
            if (slot.id == kNone) {
                slot = {hash, size()};
                chars_.insert(chars_.end(), name.begin(), name.end());
                offsets_.push_back(static_cast<std::uint32_t>(chars_.size()));
                return slot.id;
            }
            if (slot.hash == hash && get(slot.id) == name) {
                return slot.id;
            }
        }
    }

    std::wstring_view get(std::uint32_t id) const {
        return {chars_.data() + offsets_[id], offsets_[id + 1] - offsets_[id]};
    }
    std::uint32_t size() const { return static_cast<std::uint32_t>(offsets_.size() - 1); }

    std::vector<std::uint32_t> take_offsets() { return std::move(offsets_); }
    std::vector<wchar_t> take_chars() { return std::move(chars_); }

private:
    struct Slot {
        std::uint32_t hash = 0;
        std::uint32_t id = kNone;
    };

    void grow() {
        std::vector<Slot> slots(slots_.size() * 2);
        const std::size_t mask = slots.size() - 1;
        for (const Slot& slot : slots_) {
            if (slot.id != kNone) {
                std::size_t i = slot.hash & mask;
                while (slots[i].id != kNone) {
                    i = (i + 1) & mask;
                }
                slots[i] = slot;
            }
        }
        slots_ = std::move(slots);
    }

    std::vector<wchar_t> chars_;
    std::vector<std::uint32_t> offsets_{0};
    std::vector<Slot> slots_ = std::vector<Slot>(64);
};

// A key's children as the registry lists them, sorted by name. Reused for every key.
struct ChildList {
    struct Child {
        std::uint32_t offset;
        std::uint32_t length;
        std::int64_t lwt;
        KeyCounts counts;
    };

    std::vector<wchar_t> chars;
    std::vector<Child> items;

    std::wstring_view name(const Child& child) const { return {chars.data() + child.offset, child.length}; }
};

// Lists `key`'s `expected` subkeys (kNone: as many as there are), each with its LastWriteTime and
// counts, sorted by name.
LSTATUS list_children(HKEY key, std::uint32_t expected, EnumBuffer& buffer, ChildList& list) {
    list.chars.clear();
    list.items.clear();
    for (ULONG index = 0; expected == kNone || index < expected; ++index) {
        LONG status = buffer.run(kSmallRead, [&](void* data, ULONG length, ULONG* needed) {
            return nt().enumerate_key(key, index, kKeyBasicInformation, data, length, needed);
        });
        if (status == kStatusNoMoreEntries) {
            break;
        }
        if (status != kStatusSuccess) {
            return win32_error(status);
        }
        const auto* basic = static_cast<const KeyBasicInformation*>(buffer.data());
        ChildList::Child child{static_cast<std::uint32_t>(list.chars.size()),
                               static_cast<std::uint32_t>(basic->name_length / sizeof(wchar_t)),
                               basic->last_write_time.QuadPart,
                               {}};
        list.chars.insert(list.chars.end(), basic->name, basic->name + child.length);
        // Its counts, from the same listing. If the key changed in between (another LastWriteTime),
        // they're left unknown and the key is opened to ask.
        status = buffer.run(kSmallRead, [&](void* data, ULONG length, ULONG* needed) {
            return nt().enumerate_key(key, index, kKeyFullInformation, data, length, needed);
        });
        if (status == kStatusSuccess) {
            const auto* full = static_cast<const KeyFullInformation*>(buffer.data());
            if (full->last_write_time.QuadPart == child.lwt) {
                child.counts = {full->subkeys, full->values,
                                value_room(full->max_value_name_length, full->max_value_data_length), true};
            }
        }
        list.items.push_back(child);
    }
    sort_names(list.items.begin(), list.items.end(),
               [&](const ChildList::Child& a, const ChildList::Child& b) {
                   return compare_names(list.name(a), list.name(b)) < 0;
               });
    return ERROR_SUCCESS;
}

// Calls `visit(name, type, data)` for each of `key`'s `count` values.
template <typename Visit>
LONG for_each_value(HKEY key, const KeyCounts& counts, EnumBuffer& buffer, Visit visit) {
    const ULONG wanted = std::min<ULONG>(counts.value_room, kLargestFirstRead);
    for (ULONG index = 0; index < counts.values; ++index) {
        const LONG status = buffer.run(wanted, [&](void* data, ULONG length, ULONG* needed) {
            return nt().enumerate_value(key, index, kKeyValueFullInformation, data, length, needed);
        });
        if (status == kStatusNoMoreEntries) {
            break;
        }
        if (status != kStatusSuccess) {
            return status;
        }
        const auto* value = static_cast<const KeyValueFullInformation*>(buffer.data());
        const auto* bytes = reinterpret_cast<const std::byte*>(value) + value->data_offset;
        visit(std::wstring_view(value->name, value->name_length / sizeof(wchar_t)), value->type,
              std::span<const std::byte>(bytes, value->data_length));
    }
    return kStatusSuccess;
}

// ---- Places with rules of their own ----

enum class Zone : std::uint8_t {
    None,
    Excluded,     // Only Windows changes it: neither read nor compared
    Services,     // SYSTEM\CurrentControlSet\Services: a created child is a service
    TaskTree,     // TaskCache\Tree and below: a created key with an Id value is a scheduled task
    TaskData,     // TaskCache\Tasks, Plain, ...: created keys belong to the tasks in Tree
    Firewall,     // FirewallPolicy\FirewallRules: its values are firewall rules
    KnownValues,  // SharedDLLs: its values are left to the known values
};

struct Place {
    Hive hive;
    std::wstring_view path;
    Zone zone;
};

constexpr std::wstring_view kTaskTree =
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree)";

constexpr Place kPlaces[] = {
    // Large, and churned by Windows alone. Packages' state (Appx, AppModel) and the Start menu's
    // (CloudStore) are read: Windows writes them for what an installer registers, and the noise
    // filter sets them aside.
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing)",
     Zone::Excluded},
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows Defender)", Zone::Excluded},
    {Hive::Machine, LR"(SYSTEM\CurrentControlSet\Services\bam)", Zone::Excluded},
    // Named the way the rest of Avel removes them.
    {Hive::Machine, LR"(SYSTEM\CurrentControlSet\Services)", Zone::Services},
    {Hive::Machine, kTaskTree, Zone::TaskTree},
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tasks)",
     Zone::TaskData},
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Plain)",
     Zone::TaskData},
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Logon)",
     Zone::TaskData},
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Boot)",
     Zone::TaskData},
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Maintenance)",
     Zone::TaskData},
    {Hive::Machine,
     LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)",
     Zone::Firewall},
    // Saved exactly by the known values.
    {Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)", Zone::KnownValues},
    {Hive::Machine, LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)",
     Zone::KnownValues},
};

// `path` relative to `base` when it lies strictly below it.
std::optional<std::wstring_view> below(std::wstring_view path, std::wstring_view base) {
    if (base.empty()) {
        return path.empty() ? std::nullopt : std::optional(path);
    }
    if (path.size() > base.size() + 1 && path[base.size()] == L'\\' &&
        platform::iequals(path.substr(0, base.size()), base)) {
        return path.substr(base.size() + 1);
    }
    return std::nullopt;
}

// The places below one root as a tree of key names, so the walk compares a name with a rule only
// on the way to one.
class Rules {
public:
    static constexpr std::uint32_t kNoNode = kNone;

    Rules(const RegistryAddress& root, std::span<const RegistryAddress> all_roots) {
        nodes_.emplace_back();
        for (const Place& place : kPlaces) {
            if (place.hive != root.hive) {
                continue;
            }
            if (platform::iequals(place.path, root.path)) {
                nodes_[0].zone = place.zone;
            } else if (const auto relative = below(place.path, root.path)) {
                add(*relative, place.zone);
            } else if (below(root.path, place.path).has_value() &&
                       (place.zone == Zone::TaskTree || place.zone == Zone::TaskData)) {
                nodes_[0].zone = place.zone;  // The root lies inside one
            }
        }
        // Another root inside this one (a user's Software\Classes inside their Software) is read
        // as that root only.
        for (const RegistryAddress& other : all_roots) {
            if (other.hive == root.hive && platform::iequals(other.sid, root.sid)) {
                if (const auto relative = below(other.path, root.path)) {
                    add(*relative, Zone::Excluded);
                }
            }
        }
    }

    std::uint32_t top() const { return 0; }
    Zone top_zone() const { return nodes_[0].zone; }

    // The rule node of `node`'s child `name`; kNoNode when no rule lies at or below it.
    std::uint32_t find(std::uint32_t node, std::wstring_view name) const {
        if (node == kNoNode) {
            return kNoNode;
        }
        for (const std::uint32_t candidate : nodes_[node].children) {
            if (platform::iequals(nodes_[candidate].name, name)) {
                return candidate;
            }
        }
        return kNoNode;
    }

    // The zone of a key in `parent`'s zone whose rule node is `node`.
    Zone zone_of(Zone parent, std::uint32_t node) const {
        if (node != kNoNode && nodes_[node].zone != Zone::None) {
            return nodes_[node].zone;
        }
        return parent == Zone::TaskTree || parent == Zone::TaskData ? parent : Zone::None;
    }

private:
    struct Node {
        std::wstring name;
        Zone zone = Zone::None;
        std::vector<std::uint32_t> children;
    };

    void add(std::wstring_view relative, Zone rule) {
        std::uint32_t node = 0;
        while (!relative.empty()) {
            const std::size_t separator = relative.find(L'\\');
            const std::wstring_view name = relative.substr(0, separator);
            relative =
                separator == std::wstring_view::npos ? std::wstring_view() : relative.substr(separator + 1);
            std::uint32_t next = find(node, name);
            if (next == kNoNode) {
                next = static_cast<std::uint32_t>(nodes_.size());
                nodes_.push_back({std::wstring(name), Zone::None, {}});
                nodes_[node].children.push_back(next);
            }
            node = next;
        }
        nodes_[node].zone = rule;
    }

    std::vector<Node> nodes_;  // nodes_[0] is the root's top key
};

std::vector<Rules> rules_for(std::span<const RegistryAddress> roots) {
    std::vector<Rules> rules;
    rules.reserve(roots.size());
    for (const RegistryAddress& root : roots) {
        rules.emplace_back(root, roots);
    }
    return rules;
}

// ---- Work spread over threads ----

unsigned thread_count(const SnapshotOptions& options) {
    if (options.threads > 0) {
        return options.threads;
    }
    return std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
}

template <typename Job>
class JobQueue {
public:
    // Before the workers start, or from a running job.
    void push(Job job) {
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
    }

    // Waits for a job; false once there are none left and none running that could add one.
    bool pop(Job& job) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return !jobs_.empty() || running_ == 0; });
        if (jobs_.empty()) {
            return false;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
        ++running_;
        return true;
    }

    void finished() {
        std::lock_guard lock(mutex_);
        if (--running_ == 0 && jobs_.empty()) {
            ready_.notify_all();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    int running_ = 0;
};

// Runs `run(worker, job)` for every job, including the jobs that jobs push, on `threads` threads.
template <typename Job, typename Run>
void run_jobs(JobQueue<Job>& queue, unsigned threads, Run run) {
    const auto work = [&](unsigned worker) {
        Job job;
        while (queue.pop(job)) {
            run(worker, job);
            job = {};  // Lets go of shared parent keys as soon as possible
            queue.finished();
        }
    };
    std::vector<std::thread> workers;
    for (unsigned worker = 1; worker < threads; ++worker) {
        workers.emplace_back(work, worker);
    }
    work(0);
    for (auto& thread : workers) {
        thread.join();
    }
}

void sort_diagnostics(std::vector<SnapshotDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const SnapshotDiagnostic& a, const SnapshotDiagnostic& b) {
                  if (const int order =
                          compare_names(platform::display_path(a.key), platform::display_path(b.key))) {
                      return order < 0;
                  }
                  return a.code < b.code;
              });
}

// ---- Taking a snapshot ----

// A key as its parent's listing found it.
struct WorkKey {
    std::uint32_t name;                  // In the pool of the segment holding this record
    std::uint32_t data_segment = kNone;  // The segment holding what reading the key found
    std::int64_t lwt = 0;
    std::uint32_t data = kNone;  // Its index there
    KeyCounts counts;
};

// What reading a key found.
struct WorkData {
    std::uint32_t flags = 0;
    std::uint32_t first_child = 0;  // WorkKeys in the same segment
    std::uint32_t child_count = 0;
    std::uint32_t first_value = 0;  // Values in the same segment
    std::uint32_t value_count = 0;
};

// One job's output. Jobs write only to their own segment, so they need no locks; the final layout
// pass walks the segments in key order.
struct Segment {
    NamePool names;
    std::vector<WorkKey> keys;
    std::vector<WorkData> data;
    std::vector<SnapValue> values;  // Names in this segment's pool
};

// A root, or a range of a large key's children.
struct SnapJob {
    std::uint32_t root = 0;
    std::uint32_t segment = 0;
    Segment* output = nullptr;
    std::shared_ptr<const wil::unique_hkey> parent;  // The children's parent; null for a root's job
    std::vector<wchar_t> names;                      // The children's names, back to back
    std::vector<std::uint32_t> name_ends;
    std::vector<KeyCounts> counts;
    std::wstring parent_path;
    std::uint32_t parent_node = kNone;
    Zone parent_zone = Zone::None;
    std::uint32_t depth = 0;  // The children's depth below the root
};

class SnapshotBuilder {
public:
    SnapshotBuilder(std::span<const SnapshotRoot> roots, unsigned threads)
        : roots_(roots), threads_(threads), workers_(threads), root_segments_(roots.size(), kNone) {
        std::vector<RegistryAddress> addresses;
        for (const SnapshotRoot& root : roots) {
            addresses.push_back(root.address);
        }
        rules_ = rules_for(addresses);
        backup_ = platform::enable_privilege(SE_BACKUP_NAME);
    }

    RegistrySnapshot build(std::vector<SnapshotDiagnostic>& diagnostics) {
        for (std::uint32_t root = 0; root < roots_.size(); ++root) {
            SnapJob job;
            job.root = root;
            job.segment = new_segment(job.output);
            root_segments_[root] = job.segment;
            queue_.push(std::move(job));
        }
        run_jobs(queue_, threads_, [this](unsigned worker, SnapJob& job) { run(workers_[worker], job); });
        RegistrySnapshot snapshot = layout();
        for (Worker& worker : workers_) {
            diagnostics.insert(diagnostics.end(), worker.diagnostics.begin(), worker.diagnostics.end());
        }
        sort_diagnostics(diagnostics);
        return snapshot;
    }

private:
    struct Frame {
        wil::unique_hkey key;
        std::uint32_t next;  // Its children still to read: WorkKeys [next, end) of the job's segment
        std::uint32_t end;
        std::uint32_t node;
        Zone zone;
        std::uint32_t depth;      // The children's depth
        std::size_t path_length;  // Restored when this frame is done
    };

    struct Worker {
        EnumBuffer buffer;
        ChildList children;
        std::vector<Frame> stack;
        std::wstring path;  // The address path of the key being read
        std::vector<SnapshotDiagnostic> diagnostics;
    };

    std::uint32_t new_segment(Segment*& output) {
        std::lock_guard lock(segments_mutex_);
        segments_.push_back(std::make_unique<Segment>());
        output = segments_.back().get();
        return static_cast<std::uint32_t>(segments_.size() - 1);
    }

    void unreadable(Worker& worker, const SnapJob& job, LSTATUS error) {
        const RegistryAddress& root = roots_[job.root].address;
        worker.diagnostics.push_back({SnapshotDiagnosticCode::KeyUnreadable,
                                      {root.hive, root.sid, worker.path},
                                      static_cast<std::int32_t>(error)});
    }

    void run(Worker& worker, SnapJob& job) {
        Segment& out = *job.output;
        const Rules& rules = rules_[job.root];
        if (!job.parent) {
            const SnapshotRoot& root = roots_[job.root];
            wil::unique_hkey key;
            platform::KeyInfo info;
            LSTATUS status = open_root(root, backup_, key);
            if (status == ERROR_SUCCESS) {
                status = platform::query_key_info(key.get(), info);
            }
            if (status != ERROR_SUCCESS) {
                root_segments_[job.root] = kNone;
                worker.diagnostics.push_back({SnapshotDiagnosticCode::RootUnavailable, root.address,
                                              static_cast<std::int32_t>(status)});
                return;
            }
            out.keys.push_back(
                {out.names.intern(L""), job.segment, ticks(info.last_write), 0, counts_of(info)});
            out.data.emplace_back();
            worker.path = root.address.path;
            if (visit(worker, job, std::move(key), 0, counts_of(info), rules.top(), rules.top_zone(), 0, 0)) {
                walk(worker, job);
            }
            return;
        }
        out.data.resize(job.name_ends.size());
        for (std::uint32_t head = 0; head < job.name_ends.size(); ++head) {
            const std::uint32_t start = head == 0 ? 0 : job.name_ends[head - 1];
            const std::wstring_view name(job.names.data() + start, job.name_ends[head] - start);
            const std::uint32_t node = rules.find(job.parent_node, name);
            worker.path.assign(job.parent_path).append(1, L'\\').append(name);
            if (open_and_visit(worker, job, job.parent->get(), name, head, job.counts[head], node,
                               rules.zone_of(job.parent_zone, node), job.depth, job.parent_path.size())) {
                walk(worker, job);
            }
        }
    }

    // Reads the rest of the job's keys, depth first.
    void walk(Worker& worker, const SnapJob& job) {
        Segment& out = *job.output;
        const Rules& rules = rules_[job.root];
        while (!worker.stack.empty()) {
            Frame& frame = worker.stack.back();
            if (frame.next == frame.end) {
                worker.path.resize(frame.path_length);
                worker.stack.pop_back();
                continue;
            }
            const std::uint32_t index = frame.next++;
            const WorkKey record = out.keys[index];
            const std::wstring_view name = out.names.get(record.name);
            const std::uint32_t node = rules.find(frame.node, name);
            const Zone zone = rules.zone_of(frame.zone, node);
            const std::uint32_t depth = frame.depth;
            const HKEY parent = frame.key.get();
            const std::size_t path_length = worker.path.size();
            worker.path.append(1, L'\\').append(name);
            const auto data = static_cast<std::uint32_t>(out.data.size());
            out.data.emplace_back();
            out.keys[index].data_segment = job.segment;
            out.keys[index].data = data;
            if (!open_and_visit(worker, job, parent, name, data, record.counts, node, zone, depth,
                                path_length)) {
                worker.path.resize(path_length);
            }
        }
    }

    // Reads `parent`'s child `name` (worker.path is its path) into data[`data`] unless its parent
    // listed nothing in it. Returns true when it pushed a frame for the child's own children.
    bool open_and_visit(Worker& worker, const SnapJob& job, HKEY parent, std::wstring_view name,
                        std::uint32_t data, const KeyCounts& counts, std::uint32_t node, Zone zone,
                        std::uint32_t depth, std::size_t path_length) {
        if (counts.known && counts.subkeys == 0 && counts.values == 0) {
            return false;  // Nothing to read
        }
        wil::unique_hkey key;
        if (const LSTATUS status = open_subkey(parent, name, backup_, key); status != ERROR_SUCCESS) {
            job.output->data[data].flags = kKeyUnreadable;
            if (status != ERROR_FILE_NOT_FOUND) {  // Not deleted in the meantime
                unreadable(worker, job, status);
            }
            return false;
        }
        return visit(worker, job, std::move(key), data, counts, node, zone, depth, path_length);
    }

    // Reads the open key's values and children into data[`data`], and queues the children: as a
    // frame on the worker's stack (returns true), or as new jobs when there are many.
    bool visit(Worker& worker, const SnapJob& job, wil::unique_hkey key, std::uint32_t data, KeyCounts counts,
               std::uint32_t node, Zone zone, std::uint32_t depth, std::size_t path_length) {
        Segment& out = *job.output;
        if (const LSTATUS status =
                read_key(worker, out, rules_[job.root], key.get(), counts, data, node, zone);
            status != ERROR_SUCCESS) {
            unreadable(worker, job, status);
            return false;
        }
        const WorkData read = out.data[data];
        if (read.child_count == 0) {
            return false;
        }
        if (read.child_count < kSplitChildren || depth > kSplitDepth) {
            worker.stack.push_back({std::move(key), read.first_child, read.first_child + read.child_count,
                                    node, zone, depth + 1, path_length});
            return true;
        }
        const auto parent = std::make_shared<const wil::unique_hkey>(std::move(key));
        for (std::uint32_t first = 0; first < read.child_count; first += kSplitRange) {
            SnapJob part;
            part.root = job.root;
            part.segment = new_segment(part.output);
            part.parent = parent;
            part.parent_path = worker.path;
            part.parent_node = node;
            part.parent_zone = zone;
            part.depth = depth + 1;
            const std::uint32_t last = std::min(read.child_count, first + kSplitRange);
            for (std::uint32_t child = first; child < last; ++child) {
                WorkKey& record = out.keys[read.first_child + child];
                record.data_segment = part.segment;
                record.data = child - first;
                const std::wstring_view name = out.names.get(record.name);
                part.names.insert(part.names.end(), name.begin(), name.end());
                part.name_ends.push_back(static_cast<std::uint32_t>(part.names.size()));
                part.counts.push_back(record.counts);
            }
            queue_.push(std::move(part));
        }
        return false;
    }

    // Reads the key's values and lists its children (leaving out excluded ones) into the segment.
    LSTATUS read_key(Worker& worker, Segment& out, const Rules& rules, HKEY key, KeyCounts counts,
                     std::uint32_t data, std::uint32_t node, Zone zone) {
        if (!counts.known) {
            platform::KeyInfo info;
            if (const LSTATUS status = platform::query_key_info(key, info); status != ERROR_SUCCESS) {
                out.data[data].flags = kKeyUnreadable;
                return status;
            }
            counts = counts_of(info);
        }
        const auto first_value = static_cast<std::uint32_t>(out.values.size());
        bool link = false;
        const LONG values =
            for_each_value(key, counts, worker.buffer,
                           [&](std::wstring_view name, DWORD type, std::span<const std::byte> bytes) {
                               if (type == REG_LINK && platform::iequals(name, L"SymbolicLinkValue")) {
                                   link = true;
                               }
                               if (zone != Zone::KnownValues) {
                                   out.values.push_back(
                                       {out.names.intern(name), type, platform::hash_raw_value(type, bytes)});
                               }
                           });
        if (values != kStatusSuccess) {
            out.values.resize(first_value);
            out.data[data] = {kKeyUnreadable};
            return win32_error(values);
        }
        if (link) {
            out.values.resize(first_value);
            out.data[data] = {kKeyLink};
            return ERROR_SUCCESS;
        }
        sort_names(out.values.begin() + first_value, out.values.end(),
                   [&](const SnapValue& a, const SnapValue& b) {
                       return compare_names(out.names.get(a.name), out.names.get(b.name)) < 0;
                   });
        const auto first_child = static_cast<std::uint32_t>(out.keys.size());
        if (counts.subkeys > 0) {
            if (const LSTATUS status = list_children(key, counts.subkeys, worker.buffer, worker.children);
                status != ERROR_SUCCESS) {
                out.values.resize(first_value);
                out.data[data] = {kKeyUnreadable};
                return status;
            }
            for (const auto& child : worker.children.items) {
                const std::wstring_view name = worker.children.name(child);
                if (node != Rules::kNoNode && rules.zone_of(zone, rules.find(node, name)) == Zone::Excluded) {
                    continue;
                }
                out.keys.push_back({out.names.intern(name), kNone, child.lwt, kNone, child.counts});
            }
        }
        out.data[data] = {0, first_child, static_cast<std::uint32_t>(out.keys.size()) - first_child,
                          first_value, static_cast<std::uint32_t>(out.values.size()) - first_value};
        return ERROR_SUCCESS;
    }

    // Lays the segments out as one snapshot, depth first from each root, with each key's children
    // in one block: the result doesn't depend on how the work was split or scheduled.
    RegistrySnapshot layout() {
        RegistrySnapshot snapshot;
        auto& keys = SnapshotAccess::keys(snapshot);
        auto& values = SnapshotAccess::values(snapshot);
        NamePool names;
        std::vector<std::vector<std::uint32_t>> remap(segments_.size());
        const auto global = [&](std::uint32_t segment, std::uint32_t local) {
            auto& map = remap[segment];
            if (map.empty()) {
                map.assign(segments_[segment]->names.size(), kNone);
            }
            if (map[local] == kNone) {
                map[local] = names.intern(segments_[segment]->names.get(local));
            }
            return map[local];
        };
        struct Pending {
            std::uint32_t key;
            std::uint32_t segment;
            std::uint32_t data;
        };
        std::vector<Pending> pending;
        for (std::uint32_t root = 0; root < roots_.size(); ++root) {
            RegistrySnapshot::Root& entry = SnapshotAccess::roots(snapshot).emplace_back();
            entry.address = roots_[root].address;
            const std::uint32_t top_segment = root_segments_[root];
            if (top_segment == kNone) {
                continue;
            }
            const WorkKey& top = segments_[top_segment]->keys[0];
            entry.key = static_cast<std::uint32_t>(keys.size());
            keys.push_back({global(top_segment, top.name), 0, top.lwt, 0, 0, 0, 0});
            pending.push_back({entry.key, top_segment, 0});
            while (!pending.empty()) {
                const Pending item = pending.back();
                pending.pop_back();
                const Segment& segment = *segments_[item.segment];
                const WorkData& read = segment.data[item.data];
                const auto first_child = static_cast<std::uint32_t>(keys.size());
                const auto first_value = static_cast<std::uint32_t>(values.size());
                SnapKey& key = keys[item.key];
                key.flags = read.flags;
                key.first_child = first_child;
                key.child_count = read.child_count;
                key.first_value = first_value;
                key.value_count = read.value_count;
                for (std::uint32_t i = 0; i < read.value_count; ++i) {
                    const SnapValue& value = segment.values[read.first_value + i];
                    values.push_back({global(item.segment, value.name), value.type, value.hash});
                }
                for (std::uint32_t i = 0; i < read.child_count; ++i) {
                    const WorkKey& child = segment.keys[read.first_child + i];
                    keys.push_back({global(item.segment, child.name), 0, child.lwt, 0, 0, 0, 0});
                }
                for (std::uint32_t i = read.child_count; i-- > 0;) {
                    const WorkKey& child = segment.keys[read.first_child + i];
                    if (child.data_segment == kNone) {
                        keys[first_child + i].flags = kKeyUnreadable;
                    } else {
                        pending.push_back({first_child + i, child.data_segment, child.data});
                    }
                }
            }
        }
        SnapshotAccess::name_offsets(snapshot) = names.take_offsets();
        SnapshotAccess::name_chars(snapshot) = names.take_chars();
        segments_.clear();
        return snapshot;
    }

    std::span<const SnapshotRoot> roots_;
    unsigned threads_;
    std::vector<Worker> workers_;
    std::vector<Rules> rules_;
    bool backup_ = false;
    JobQueue<SnapJob> queue_;
    std::mutex segments_mutex_;
    std::vector<std::unique_ptr<Segment>> segments_;
    std::vector<std::uint32_t> root_segments_;  // Each root's job writes only its own entry
};

// ---- Comparing with the live registry ----

// A change found, with what orders the results.
struct Report {
    std::uint32_t root;
    std::wstring path;   // The key's address path
    std::wstring value;  // The value's name, for value changes
    bool is_value;
    TraceChange change;
};

// A child that exists both in the snapshot and now.
struct Matched {
    std::uint32_t snap;  // Its key in the snapshot
    std::int64_t lwt;    // Its LastWriteTime now
    KeyCounts counts;    // And what it holds now
};

struct DiffJob {
    std::uint32_t root = 0;                          // In the snapshot
    std::uint32_t live_root = 0;                     // In the roots given
    std::shared_ptr<const wil::unique_hkey> parent;  // Null for a root's job
    std::vector<Matched> children;
    std::wstring parent_path;
    std::uint32_t parent_node = kNone;
    Zone parent_zone = Zone::None;
    std::uint32_t depth = 0;  // The children's depth below the root
};

bool is_driver(HKEY service) {
    platform::RawValue type;
    if (platform::query_raw_value(service, L"Type", type) != ERROR_SUCCESS ||
        type.data.size() != sizeof(DWORD)) {
        return false;
    }
    DWORD value = 0;
    std::memcpy(&value, type.data.data(), sizeof(value));
    // SERVICE_KERNEL_DRIVER, SERVICE_FILE_SYSTEM_DRIVER, SERVICE_RECOGNIZER_DRIVER
    return value == 1 || value == 2 || value == 8;
}

// The number of subkeys and values inside `key`, all the way down.
std::uint64_t count_contents(HKEY key, bool backup) {
    platform::KeyInfo info;
    if (platform::query_key_info(key, info) != ERROR_SUCCESS) {
        return 0;
    }
    std::uint64_t count = info.values;
    if (info.subkeys == 0) {
        return count;
    }
    std::vector<platform::SubkeyInfo> subkeys;
    platform::enum_subkeys(key, subkeys);
    for (const auto& subkey : subkeys) {
        ++count;
        wil::unique_hkey child;
        if (open_subkey(key, subkey.name, backup, child) == ERROR_SUCCESS) {
            count += count_contents(child.get(), backup);
        }
    }
    return count;
}

class Differ {
public:
    Differ(const RegistrySnapshot& snapshot, std::span<const SnapshotRoot> roots, unsigned threads)
        : snapshot_(snapshot), live_roots_(roots), threads_(threads), workers_(threads) {
        std::vector<RegistryAddress> addresses;
        for (const auto& root : snapshot.roots()) {
            addresses.push_back(root.address);
        }
        rules_ = rules_for(addresses);
        backup_ = platform::enable_privilege(SE_BACKUP_NAME);
    }

    std::vector<TraceChange> run(std::vector<SnapshotDiagnostic>& diagnostics) {
        for (std::uint32_t root = 0; root < snapshot_.roots().size(); ++root) {
            const auto& entry = snapshot_.roots()[root];
            const auto live =
                std::find_if(live_roots_.begin(), live_roots_.end(), [&](const SnapshotRoot& candidate) {
                    return platform::same_address(candidate.address, entry.address);
                });
            if (entry.key == RegistrySnapshot::kNoKey || live == live_roots_.end()) {
                const LSTATUS error =
                    entry.key == RegistrySnapshot::kNoKey ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
                diagnostics.push_back({SnapshotDiagnosticCode::RootUnavailable, entry.address,
                                       static_cast<std::int32_t>(error)});
                continue;
            }
            DiffJob job;
            job.root = root;
            job.live_root = static_cast<std::uint32_t>(live - live_roots_.begin());
            queue_.push(std::move(job));
        }
        run_jobs(queue_, threads_, [this](unsigned worker, DiffJob& job) { run(workers_[worker], job); });

        std::vector<Report> reports;
        for (Worker& worker : workers_) {
            std::move(worker.reports.begin(), worker.reports.end(), std::back_inserter(reports));
            diagnostics.insert(diagnostics.end(), worker.diagnostics.begin(), worker.diagnostics.end());
        }
        sort_diagnostics(diagnostics);
        std::sort(reports.begin(), reports.end(), [](const Report& a, const Report& b) {
            if (a.root != b.root) {
                return a.root < b.root;
            }
            if (const int order = compare_names(a.path, b.path)) {
                return order < 0;
            }
            if (a.is_value != b.is_value) {
                return !a.is_value;
            }
            if (const int order = compare_names(a.value, b.value)) {
                return order < 0;
            }
            return a.change.change < b.change.change;
        });
        std::vector<TraceChange> changes;
        changes.reserve(reports.size());
        for (Report& item : reports) {
            changes.push_back(std::move(item.change));
        }
        return changes;
    }

private:
    struct Frame {
        wil::unique_hkey key;
        std::size_t begin;  // Its matched children: worker.matched[next, end); begin restores the arena
        std::size_t next;
        std::size_t end;
        std::uint32_t node;
        Zone zone;
        std::uint32_t depth;  // The children's depth
        std::size_t path_length;
    };

    struct Worker {
        EnumBuffer buffer;
        ChildList children;
        std::vector<Matched> matched;  // A stack of the frames' matched children
        std::vector<Frame> stack;
        std::wstring path;
        std::vector<platform::RawValue> values;
        std::vector<Report> reports;
        std::vector<SnapshotDiagnostic> diagnostics;
    };

    RegistryAddress address(std::uint32_t root, std::wstring path) const {
        const RegistryAddress& base = snapshot_.roots()[root].address;
        return {base.hive, base.sid, std::move(path)};
    }

    void diagnose(Worker& worker, std::uint32_t root, SnapshotDiagnosticCode code, std::wstring path,
                  LSTATUS error) {
        worker.diagnostics.push_back(
            {code, address(root, std::move(path)), static_cast<std::int32_t>(error)});
    }

    void report(Worker& worker, std::uint32_t root, std::wstring path, std::wstring value, bool is_value,
                ChangeKind change, leftovers::Location location, std::uint64_t contents = 0) {
        TraceChange item;
        item.change = change;
        item.location = std::move(location);
        item.contents = contents;
        worker.reports.push_back({root, std::move(path), std::move(value), is_value, std::move(item)});
    }

    void run(Worker& worker, DiffJob& job) {
        const Rules& rules = rules_[job.root];
        if (!job.parent) {
            const auto& entry = snapshot_.roots()[job.root];
            wil::unique_hkey key;
            platform::KeyInfo info;
            LSTATUS status = open_root(live_roots_[job.live_root], backup_, key);
            if (status == ERROR_SUCCESS) {
                status = platform::query_key_info(key.get(), info);
            }
            if (status != ERROR_SUCCESS) {
                diagnose(worker, job.root, SnapshotDiagnosticCode::RootUnavailable, entry.address.path,
                         status);
                return;
            }
            worker.path = entry.address.path;
            const SnapKey& top = snapshot_.keys()[entry.key];
            if (top.flags & kKeyUnreadable) {
                diagnose(worker, job.root, SnapshotDiagnosticCode::KeyUnreadableAtStart, worker.path, 0);
                return;
            }
            if (top.flags & kKeyLink) {
                return;
            }
            if (compare(worker, job, std::move(key), entry.key, ticks(info.last_write), counts_of(info),
                        rules.top(), rules.top_zone(), 0, 0)) {
                walk(worker, job);
            }
            return;
        }
        for (const Matched& child : job.children) {
            const std::wstring_view name = snapshot_.name(snapshot_.keys()[child.snap].name);
            const std::uint32_t node = rules.find(job.parent_node, name);
            worker.path.assign(job.parent_path).append(1, L'\\').append(name);
            if (visit_matched(worker, job, job.parent->get(), child, node,
                              rules.zone_of(job.parent_zone, node), job.depth, job.parent_path.size())) {
                walk(worker, job);
            }
        }
    }

    // Compares the rest of the job's keys, depth first.
    void walk(Worker& worker, const DiffJob& job) {
        const Rules& rules = rules_[job.root];
        while (!worker.stack.empty()) {
            Frame& frame = worker.stack.back();
            if (frame.next == frame.end) {
                worker.path.resize(frame.path_length);
                worker.matched.resize(frame.begin);
                worker.stack.pop_back();
                continue;
            }
            const Matched child = worker.matched[frame.next++];
            const std::wstring_view name = snapshot_.name(snapshot_.keys()[child.snap].name);
            const std::uint32_t node = rules.find(frame.node, name);
            const Zone zone = rules.zone_of(frame.zone, node);
            const std::uint32_t depth = frame.depth;
            const HKEY parent = frame.key.get();
            const std::size_t path_length = worker.path.size();
            worker.path.append(1, L'\\').append(name);
            if (!visit_matched(worker, job, parent, child, node, zone, depth, path_length)) {
                worker.path.resize(path_length);
            }
        }
    }

    // A child found both in the snapshot and now (worker.path is its path). It is opened only to
    // list subkeys it has now, or to read values when its LastWriteTime changed. Returns true when
    // it pushed a frame for its own children.
    bool visit_matched(Worker& worker, const DiffJob& job, HKEY parent, const Matched& child,
                       std::uint32_t node, Zone zone, std::uint32_t depth, std::size_t path_length) {
        const SnapKey& before = snapshot_.keys()[child.snap];
        if (before.flags & kKeyUnreadable) {
            diagnose(worker, job.root, SnapshotDiagnosticCode::KeyUnreadableAtStart, worker.path, 0);
            return false;
        }
        if (before.flags & kKeyLink) {
            return false;
        }
        const bool values_changed = before.lwt != child.lwt && zone != Zone::KnownValues;
        if (child.counts.known && child.counts.subkeys == 0 &&
            !(values_changed && (child.counts.values > 0 || before.value_count > 0))) {
            // No subkeys now: the ones it had are gone.
            const Rules& rules = rules_[job.root];
            for (const SnapKey& gone : snapshot_.children(before)) {
                const std::wstring_view name = snapshot_.name(gone.name);
                if (rules.zone_of(zone, rules.find(node, name)) != Zone::Excluded) {
                    deleted(worker, job.root, name);
                }
            }
            return false;
        }
        wil::unique_hkey key;
        if (const LSTATUS status = open_subkey(parent, snapshot_.name(before.name), backup_, key);
            status != ERROR_SUCCESS) {
            diagnose(worker, job.root, SnapshotDiagnosticCode::KeyUnreadable, worker.path, status);
            return false;
        }
        return compare(worker, job, std::move(key), child.snap, child.lwt, child.counts, node, zone, depth,
                       path_length);
    }

    void deleted(Worker& worker, std::uint32_t root, std::wstring_view name) {
        std::wstring path = worker.path + L"\\" + std::wstring(name);
        leftovers::RegistryKeyLocation location{address(root, path)};
        report(worker, root, std::move(path), {}, false, ChangeKind::Deleted, std::move(location));
    }

    // Compares the open key's values (when its LastWriteTime changed) and children, reporting what
    // was created and deleted, and queues the children found at both ends: as a frame (returns
    // true) or as new jobs when there are many.
    bool compare(Worker& worker, const DiffJob& job, wil::unique_hkey key, std::uint32_t snap,
                 std::int64_t lwt, KeyCounts counts, std::uint32_t node, Zone zone, std::uint32_t depth,
                 std::size_t path_length) {
        const Rules& rules = rules_[job.root];
        const SnapKey& before = snapshot_.keys()[snap];
        if (!counts.known) {
            platform::KeyInfo info;
            if (const LSTATUS status = platform::query_key_info(key.get(), info); status != ERROR_SUCCESS) {
                diagnose(worker, job.root, SnapshotDiagnosticCode::KeyUnreadable, worker.path, status);
                return false;
            }
            counts = counts_of(info);
        }
        if (before.lwt != lwt && zone != Zone::KnownValues) {
            compare_values(worker, job.root, key.get(), before, zone);
        }
        auto& live = worker.children;
        live.items.clear();
        if (counts.subkeys > 0) {
            if (const LSTATUS status = list_children(key.get(), counts.subkeys, worker.buffer, live);
                status != ERROR_SUCCESS) {
                diagnose(worker, job.root, SnapshotDiagnosticCode::KeyUnreadable, worker.path, status);
                return false;
            }
        }
        const auto snap_children = snapshot_.children(before);
        const std::size_t begin = worker.matched.size();
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < live.items.size() || j < snap_children.size()) {
            const int order =
                i == live.items.size() ? 1
                : j == snap_children.size()
                    ? -1
                    : compare_ignoring_case(live.name(live.items[i]), snapshot_.name(snap_children[j].name));
            if (order == 0) {
                worker.matched.push_back({before.first_child + static_cast<std::uint32_t>(j),
                                          live.items[i].lwt, live.items[i].counts});
                ++i;
                ++j;
                continue;
            }
            const std::wstring_view name =
                order < 0 ? live.name(live.items[i]) : snapshot_.name(snap_children[j].name);
            const Zone child_zone = rules.zone_of(zone, rules.find(node, name));
            if (child_zone != Zone::Excluded) {
                if (order < 0) {
                    created(worker, job.root, key.get(), std::wstring(name), live.items[i].counts, zone,
                            child_zone);
                } else {
                    deleted(worker, job.root, name);
                }
            }
            (order < 0 ? i : j)++;
        }
        const std::size_t count = worker.matched.size() - begin;
        if (count == 0) {
            return false;
        }
        if (count < kSplitChildren || depth > kSplitDepth) {
            worker.stack.push_back(
                {std::move(key), begin, begin, worker.matched.size(), node, zone, depth + 1, path_length});
            return true;
        }
        const auto parent = std::make_shared<const wil::unique_hkey>(std::move(key));
        for (std::size_t first = begin; first < worker.matched.size(); first += kSplitRange) {
            DiffJob part;
            part.root = job.root;
            part.live_root = job.live_root;
            part.parent = parent;
            part.children.assign(
                worker.matched.begin() + first,
                worker.matched.begin() + std::min(worker.matched.size(), first + kSplitRange));
            part.parent_path = worker.path;
            part.parent_node = node;
            part.parent_zone = zone;
            part.depth = depth + 1;
            queue_.push(std::move(part));
        }
        worker.matched.resize(begin);
        return false;
    }

    void compare_values(Worker& worker, std::uint32_t root, HKEY key, const SnapKey& before, Zone zone) {
        auto& live = worker.values;
        live.clear();
        if (const LSTATUS status = platform::read_raw_values(key, live); status != ERROR_SUCCESS) {
            diagnose(worker, root, SnapshotDiagnosticCode::KeyUnreadable, worker.path, status);
            return;
        }
        std::sort(live.begin(), live.end(), [](const platform::RawValue& a, const platform::RawValue& b) {
            return compare_names(a.name, b.name) < 0;
        });
        const auto values = snapshot_.values(before);
        const auto changed = [&](ChangeKind change, const std::wstring& name) {
            leftovers::Location location;
            if (zone == Zone::Firewall) {
                location = leftovers::FirewallRuleLocation{name};
            } else {
                location = leftovers::RegistryValueLocation{address(root, worker.path), name};
            }
            report(worker, root, worker.path, name, true, change, std::move(location));
        };
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < live.size() || j < values.size()) {
            const int order = i == live.size() ? 1
                              : j == values.size()
                                  ? -1
                                  : compare_ignoring_case(live[i].name, snapshot_.name(values[j].name));
            if (order < 0) {
                changed(ChangeKind::Created, live[i++].name);
            } else if (order > 0) {
                changed(ChangeKind::Deleted, std::wstring(snapshot_.name(values[j++].name)));
            } else {
                if (live[i].type != values[j].type ||
                    platform::hash_raw_value(live[i].type, live[i].data) != values[j].hash) {
                    changed(ChangeKind::Modified, live[i].name);
                }
                ++i;
                ++j;
            }
        }
    }

    // Reports a key that exists now but not in the snapshot: one item for all of it.
    void created(Worker& worker, std::uint32_t root, HKEY parent, std::wstring name, const KeyCounts& counts,
                 Zone parent_zone, Zone zone) {
        if (zone == Zone::TaskData) {
            return;  // Belongs to a task in TaskCache\Tree, reported there
        }
        std::wstring path = worker.path + L"\\" + name;
        const bool empty = counts.known && counts.subkeys == 0 && counts.values == 0;
        wil::unique_hkey key;
        if (!empty) {
            const LSTATUS status = open_subkey(parent, name, backup_, key);
            if (status == ERROR_FILE_NOT_FOUND) {
                return;  // Gone again already
            }
            if (status != ERROR_SUCCESS) {
                diagnose(worker, root, SnapshotDiagnosticCode::KeyUnreadable, path, status);
            }
        }
        if (zone == Zone::TaskTree) {
            if (key) {
                find_tasks(worker, root, key.get(), path);
            }
            return;
        }
        const std::uint64_t contents = key ? count_contents(key.get(), backup_) : 0;
        leftovers::Location location;
        if (parent_zone == Zone::Services) {
            location = leftovers::ServiceLocation{name, key && is_driver(key.get())};
        } else {
            location = leftovers::RegistryKeyLocation{address(root, path)};
        }
        report(worker, root, std::move(path), {}, false, ChangeKind::Created, std::move(location), contents);
    }

    // A created key in TaskCache\Tree: every key in it with an Id value is a task; folders are
    // left out.
    void find_tasks(Worker& worker, std::uint32_t root, HKEY key, const std::wstring& path) {
        platform::RawValue id;
        if (platform::query_raw_value(key, L"Id", id) == ERROR_SUCCESS) {
            const std::wstring task = below(path, kTaskTree).has_value()
                                          ? path.substr(kTaskTree.size())
                                          : path.substr(path.find_last_of(L'\\'));
            report(worker, root, path, {}, false, ChangeKind::Created, leftovers::TaskLocation{task},
                   count_contents(key, backup_));
        }
        std::vector<platform::SubkeyInfo> subkeys;
        platform::enum_subkeys(key, subkeys);
        for (const auto& subkey : subkeys) {
            wil::unique_hkey child;
            if (open_subkey(key, subkey.name, backup_, child) == ERROR_SUCCESS) {
                find_tasks(worker, root, child.get(), path + L"\\" + subkey.name);
            }
        }
    }

    const RegistrySnapshot& snapshot_;
    std::span<const SnapshotRoot> live_roots_;
    unsigned threads_;
    std::vector<Worker> workers_;
    std::vector<Rules> rules_;
    bool backup_ = false;
    JobQueue<DiffJob> queue_;
};

// ---- The snapshot file ----

constexpr char kMagic[8] = {'A', 'V', 'E', 'L', 'R', 'S', 'N', '1'};
constexpr std::uint32_t kVersion = 1;

struct FileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t reserved;
    std::uint64_t checksum;  // FNV-1a of everything after the header
};

struct FileCounts {
    std::uint64_t root_bytes;
    std::uint64_t keys;
    std::uint64_t values;
    std::uint64_t names;
    std::uint64_t name_chars;
};

static_assert(sizeof(SnapKey) == 32 && std::has_unique_object_representations_v<SnapKey>);
static_assert(sizeof(SnapValue) == 16 && std::has_unique_object_representations_v<SnapValue>);
static_assert(std::has_unique_object_representations_v<FileHeader> &&
              std::has_unique_object_representations_v<FileCounts>);

constexpr DWORD kChunk = 1u << 26;

// Writes and reads the file in large pieces, keeping the checksum of what passes through.
class FileWriter {
public:
    explicit FileWriter(HANDLE file) : file_(file) {}

    bool write(const void* data, std::size_t size, bool checked = true) {
        if (checked) {
            checksum_ = platform::fnv1a(data, size, checksum_);
        }
        const auto* bytes = static_cast<const char*>(data);
        while (size > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, kChunk));
            DWORD written = 0;
            if (!::WriteFile(file_, bytes, chunk, &written, nullptr) || written != chunk) {
                return false;
            }
            bytes += chunk;
            size -= chunk;
        }
        return true;
    }

    std::uint64_t checksum() const { return checksum_; }

private:
    HANDLE file_;
    std::uint64_t checksum_ = platform::kFnvOffset;
};

class FileReader {
public:
    explicit FileReader(HANDLE file) : file_(file) {}

    bool read(void* data, std::size_t size, bool checked = true) {
        auto* bytes = static_cast<char*>(data);
        const std::size_t total = size;
        while (size > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, kChunk));
            DWORD done = 0;
            if (!::ReadFile(file_, bytes, chunk, &done, nullptr) || done != chunk) {
                return false;
            }
            bytes += chunk;
            size -= chunk;
        }
        if (checked) {
            checksum_ = platform::fnv1a(data, total, checksum_);
        }
        return true;
    }

    template <typename T>
    bool read_vector(std::vector<T>& items, std::uint64_t count) {
        items.resize(static_cast<std::size_t>(count));
        return read(items.data(), items.size() * sizeof(T));
    }

    std::uint64_t checksum() const { return checksum_; }

private:
    HANDLE file_;
    std::uint64_t checksum_ = platform::kFnvOffset;
};

void put_u32(std::vector<char>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

void put_text(std::vector<char>& out, const std::wstring& text) {
    const auto* bytes = reinterpret_cast<const char*>(text.data());
    out.insert(out.end(), bytes, bytes + text.size() * sizeof(wchar_t));
}

// The roots: hive, top key, the sid's and path's lengths, then their characters.
std::vector<char> encode_roots(const std::vector<RegistrySnapshot::Root>& roots) {
    std::vector<char> out;
    for (const auto& root : roots) {
        put_u32(out, static_cast<std::uint32_t>(root.address.hive));
        put_u32(out, root.key);
        put_u32(out, static_cast<std::uint32_t>(root.address.sid.size()));
        put_u32(out, static_cast<std::uint32_t>(root.address.path.size()));
        put_text(out, root.address.sid);
        put_text(out, root.address.path);
    }
    return out;
}

bool decode_roots(const std::vector<char>& data, std::vector<RegistrySnapshot::Root>& roots) {
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t size) {
        if (data.size() - at < size) {
            return false;
        }
        std::memcpy(out, data.data() + at, size);
        at += size;
        return true;
    };
    const auto take_text = [&](std::wstring& text, std::uint32_t length) {
        if ((data.size() - at) / sizeof(wchar_t) < length) {
            return false;
        }
        text.resize(length);
        return take(text.data(), std::size_t{length} * sizeof(wchar_t));
    };
    while (at < data.size()) {
        std::uint32_t fields[4] = {};
        RegistrySnapshot::Root root;
        if (!take(fields, sizeof(fields)) || fields[0] > static_cast<std::uint32_t>(Hive::User) ||
            !take_text(root.address.sid, fields[2]) || !take_text(root.address.path, fields[3])) {
            return false;
        }
        root.address.hive = static_cast<Hive>(fields[0]);
        root.key = fields[1];
        roots.push_back(std::move(root));
    }
    return true;
}

// Every index points inside the snapshot, and children come after their parent, so walking it
// always ends.
bool consistent(RegistrySnapshot& snapshot) {
    const auto& offsets = SnapshotAccess::name_offsets(snapshot);
    const std::uint64_t chars = SnapshotAccess::name_chars(snapshot).size();
    if (offsets.empty() || offsets.front() != 0 || offsets.back() != chars ||
        !std::is_sorted(offsets.begin(), offsets.end())) {
        return false;
    }
    const std::uint64_t names = offsets.size() - 1;
    const auto& keys = SnapshotAccess::keys(snapshot);
    const auto& values = SnapshotAccess::values(snapshot);
    for (std::uint64_t index = 0; index < keys.size(); ++index) {
        const SnapKey& key = keys[index];
        if (key.name >= names || std::uint64_t{key.first_child} + key.child_count > keys.size() ||
            (key.child_count > 0 && key.first_child <= index) ||
            std::uint64_t{key.first_value} + key.value_count > values.size()) {
            return false;
        }
    }
    for (const SnapValue& value : values) {
        if (value.name >= names) {
            return false;
        }
    }
    for (const auto& root : SnapshotAccess::roots(snapshot)) {
        if (root.key != RegistrySnapshot::kNoKey && root.key >= keys.size()) {
            return false;
        }
    }
    return true;
}

}  // namespace

int compare_registry_names(std::wstring_view a, std::wstring_view b) {
    if (const int result = compare_ignoring_case(a, b); result != 0) {
        return result < 0 ? -1 : 1;
    }
    const int exact = a.compare(b);
    return exact < 0 ? -1 : (exact > 0 ? 1 : 0);
}

std::string_view to_string(SnapshotDiagnosticCode code) {
    switch (code) {
        case SnapshotDiagnosticCode::RootUnavailable:
            return "root-unavailable";
        case SnapshotDiagnosticCode::KeyUnreadable:
            return "key-unreadable";
        case SnapshotDiagnosticCode::KeyUnreadableAtStart:
            return "key-unreadable-at-start";
    }
    return "unknown";
}

std::shared_ptr<const platform::UserHive> UserHiveCache::open(const std::wstring& sid, bool classes) {
    const std::wstring id = platform::to_lower(sid) + (classes ? L"|classes" : L"");
    if (const auto found = hives_.find(id); found != hives_.end()) {
        return found->second;
    }
    if (!profiles_) {
        profiles_ = platform::enumerate_user_profiles();
    }
    std::shared_ptr<platform::UserHive> hive;
    const auto profile = std::find_if(
        profiles_->begin(), profiles_->end(),
        [&](const platform::UserProfile& candidate) { return platform::iequals(candidate.sid, sid); });
    if (profile != profiles_->end()) {
        hive = std::make_shared<platform::UserHive>();
        const REGSAM access = KEY_READ | KEY_WOW64_64KEY;
        const LSTATUS status = classes ? platform::open_user_classes_hive(*profile, access, *hive)
                                       : platform::open_user_hive(*profile, access, *hive);
        if (status != ERROR_SUCCESS) {
            hive.reset();
        }
    }
    hives_.emplace(id, hive);
    return hive;
}

std::vector<SnapshotRoot> live_snapshot_roots() {
    std::vector<SnapshotRoot> roots;
    for (const wchar_t* path : {L"SOFTWARE", LR"(SYSTEM\CurrentControlSet\Services)"}) {
        roots.push_back({{Hive::Machine, L"", path}, HKEY_LOCAL_MACHINE, path, nullptr});
    }
    for (const auto& profile : platform::enumerate_user_profiles()) {
        if (profile.hive_loaded) {
            roots.push_back(
                {{Hive::User, profile.sid, L"Software"}, HKEY_USERS, profile.sid + LR"(\Software)", nullptr});
            roots.push_back({{Hive::User, profile.sid, LR"(Software\Classes)"},
                             HKEY_USERS,
                             profile.sid + L"_Classes",
                             nullptr});
        }
    }
    return roots;
}

std::vector<SnapshotRoot> live_snapshot_roots(const RegistrySnapshot& started, UserHiveCache& hives) {
    std::vector<SnapshotRoot> roots;
    for (const auto& root : started.roots()) {
        const RegistryAddress& address = root.address;
        if (address.hive == Hive::Machine) {
            roots.push_back({address, HKEY_LOCAL_MACHINE, address.path, nullptr});
            continue;
        }
        // A user's Software\Classes is their classes hive, the rest of Software their main one.
        constexpr std::wstring_view kClasses = LR"(Software\Classes)";
        const bool classes =
            platform::iequals(address.path, kClasses) || below(address.path, kClasses).has_value();
        auto hive = hives.open(address.sid, classes);
        if (!hive) {
            continue;
        }
        std::wstring subpath =
            classes ? address.path.substr(std::min(address.path.size(), kClasses.size() + 1)) : address.path;
        const HKEY parent = hive->key.get();
        roots.push_back({address, parent, std::move(subpath), std::move(hive)});
    }
    return roots;
}

RegistrySnapshot take_registry_snapshot(std::span<const SnapshotRoot> roots, const SnapshotOptions& options,
                                        std::vector<SnapshotDiagnostic>& diagnostics) {
    SnapshotBuilder builder(roots, thread_count(options));
    return builder.build(diagnostics);
}

bool write_snapshot(const RegistrySnapshot& snapshot, const std::wstring& path) {
    wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }
    const std::vector<char> roots = encode_roots(snapshot.roots());
    const auto& offsets = SnapshotAccess::name_offsets(snapshot);
    const auto& chars = SnapshotAccess::name_chars(snapshot);
    const FileCounts counts{roots.size(), snapshot.keys().size(), snapshot.values().size(),
                            offsets.size() - 1, chars.size()};
    FileHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kVersion;
    // The checksum covers what follows the header, so the header is written again once it's known.
    FileWriter writer(file.get());
    bool ok = writer.write(&header, sizeof(header), false) && writer.write(&counts, sizeof(counts)) &&
              writer.write(roots.data(), roots.size()) &&
              writer.write(snapshot.keys().data(), snapshot.keys().size_bytes()) &&
              writer.write(snapshot.values().data(), snapshot.values().size_bytes()) &&
              writer.write(offsets.data(), offsets.size() * sizeof(std::uint32_t)) &&
              writer.write(chars.data(), chars.size() * sizeof(wchar_t));
    header.checksum = writer.checksum();
    const LARGE_INTEGER start{};
    ok = ok && ::SetFilePointerEx(file.get(), start, nullptr, FILE_BEGIN) &&
         writer.write(&header, sizeof(header), false) && ::FlushFileBuffers(file.get());
    if (!ok) {
        file.reset();
        ::DeleteFileW(path.c_str());
    }
    return ok;
}

std::optional<RegistrySnapshot> read_snapshot(const std::wstring& path) {
    wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (!file || !::GetFileSizeEx(file.get(), &size)) {
        return std::nullopt;
    }
    const auto file_size = static_cast<std::uint64_t>(size.QuadPart);
    FileReader body(file.get());
    FileHeader header{};
    if (file_size < sizeof(FileHeader) + sizeof(FileCounts) || !body.read(&header, sizeof(header), false) ||
        std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion) {
        return std::nullopt;
    }
    FileCounts counts{};
    if (!body.read(&counts, sizeof(counts))) {
        return std::nullopt;
    }
    // The counts must account for the file's size exactly; checked piece by piece so a damaged
    // count can't overflow the sum or ask for more memory than the file holds.
    std::uint64_t remaining = file_size - sizeof(FileHeader) - sizeof(FileCounts);
    const auto fits = [&](std::uint64_t count, std::uint64_t item_size) {
        if (count > remaining / item_size) {
            return false;
        }
        remaining -= count * item_size;
        return true;
    };
    if (!fits(counts.root_bytes, 1) || !fits(counts.keys, sizeof(SnapKey)) ||
        !fits(counts.values, sizeof(SnapValue)) ||
        counts.names >= std::numeric_limits<std::uint32_t>::max() ||
        !fits(counts.names + 1, sizeof(std::uint32_t)) || !fits(counts.name_chars, sizeof(wchar_t)) ||
        remaining != 0 || counts.keys >= kNone || counts.values > kNone) {
        return std::nullopt;
    }
    RegistrySnapshot snapshot;
    std::vector<char> roots;
    if (!body.read_vector(roots, counts.root_bytes) ||
        !body.read_vector(SnapshotAccess::keys(snapshot), counts.keys) ||
        !body.read_vector(SnapshotAccess::values(snapshot), counts.values) ||
        !body.read_vector(SnapshotAccess::name_offsets(snapshot), counts.names + 1) ||
        !body.read_vector(SnapshotAccess::name_chars(snapshot), counts.name_chars) ||
        body.checksum() != header.checksum || !decode_roots(roots, SnapshotAccess::roots(snapshot)) ||
        !consistent(snapshot)) {
        return std::nullopt;
    }
    return snapshot;
}

std::vector<TraceChange> diff_registry(const RegistrySnapshot& snapshot, std::span<const SnapshotRoot> roots,
                                       const SnapshotOptions& options,
                                       std::vector<SnapshotDiagnostic>& diagnostics) {
    Differ differ(snapshot, roots, thread_count(options));
    return differ.run(diagnostics);
}

}  // namespace avel::tracker
