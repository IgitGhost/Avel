// A made-up system for the trace session and Install and trace tests, so they run without
// administrator rights: a volume C: whose USN journal the test writes, registry roots in the
// volatile scratch key standing in for HKLM\SOFTWARE and a machine environment, and the noise
// rules of a made-up machine with one user. Also the store options tests use (a temporary root,
// a mutex of their own, a policy that admits the current user).
#pragma once

#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/leftovers/environment.h"
#include "core/platform/text.h"
#include "core/tracker/file_changes.h"
#include "core/tracker/noise_filter.h"
#include "core/tracker/trace_session.h"
#include "core/tracker/trace_store.h"
#include "file_tree.h"
#include "scratch_registry.h"

namespace avel::tracker::fakes {

using namespace avel::testing;

constexpr std::uint32_t kSerial = 0x7E570001;
constexpr std::uint64_t kRoot = 0x0005000000000005;
// Folders that exist on the made-up C: and have no records of their own.
constexpr std::uint64_t kProgramFiles = 0x0001000000001000;
constexpr std::uint64_t kTemp = 0x0001000000001001;      // The user's Temp folder (quiet)
constexpr std::uint64_t kAvelData = 0x0001000000001002;  // Stands for the store's parent folder

constexpr std::uint32_t kFolder = FILE_ATTRIBUTE_DIRECTORY;
constexpr std::uint32_t kFile = FILE_ATTRIBUTE_ARCHIVE;

constexpr wchar_t kUserSid[] = L"S-1-5-21-1-2-3-1001";
const platform::RegistryAddress kSoftware{platform::Hive::Machine, L"", L"SOFTWARE"};
const platform::RegistryAddress kMachineEnvironment{
    platform::Hive::Machine, L"", LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"};

inline TraceStoreOptions test_store_options(
    const std::wstring& root, std::function<std::chrono::system_clock::time_point()> clock = {}) {
    GUID guid{};
    ::CoCreateGuid(&guid);
    TraceStoreOptions options;
    options.root = root;
    options.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
    const std::wstring user = current_user_sid_text();
    options.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")",
                      {user, L"BA", L"SY"}};
    if (clock) {
        options.clock = std::move(clock);
    }
    return options;
}

// Answers for the made-up C:'s folders; never reads the disk.
class FakeResolver : public PathResolver {
public:
    explicit FakeResolver(std::unordered_map<std::uint64_t, std::wstring> folders)
        : folders_(std::move(folders)) {}

    std::optional<std::wstring> path_now(std::uint64_t folder) override {
        const auto found = folders_.find(folder);
        return found != folders_.end() ? std::optional(found->second) : std::nullopt;
    }
    std::optional<std::pair<std::uint64_t, std::wstring>> name_now(std::uint64_t) override {
        return std::nullopt;
    }
    std::optional<std::uint64_t> id_at(const std::wstring&) override { return std::nullopt; }
    std::optional<std::int64_t> created_at(const std::wstring&) override { return std::nullopt; }

private:
    std::unordered_map<std::uint64_t, std::wstring> folders_;
};

class FakeSystem {
public:
    // `avel_data` is what the made-up folder kAvelData resolves to: the store root's parent, so
    // files in it are Avel's.
    FakeSystem(const ScratchKey& scratch, std::wstring avel_data)
        : scratch_(scratch), avel_data_(std::move(avel_data)) {
        scratch_.create(L"Machine");
        scratch_.create(L"Environment");
    }

    // The records Windows writes when a file or folder is created.
    void create(std::uint64_t frn, std::uint64_t parent, std::wstring name,
                std::uint32_t attributes = kFile) {
        record(frn, parent, name, USN_REASON_FILE_CREATE, attributes);
        record(frn, parent, name,
               USN_REASON_FILE_CREATE | USN_REASON_CLOSE |
                   ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : USN_REASON_DATA_EXTEND),
               attributes);
    }

    void record(std::uint64_t frn, std::uint64_t parent, std::wstring name, std::uint32_t reason,
                std::uint32_t attributes) {
        std::lock_guard lock(mutex_);
        records_.push_back({frn, parent, std::move(name), reason, attributes});
    }

    // The next drain ends tracking the volume with this status.
    void fail_next_drain(JournalStatus status) {
        std::lock_guard lock(mutex_);
        fail_with_ = status;
    }

    // The journal overwrites every record written so far: a drain that still needs one of them
    // finds the journal wrapped, so only a checkpoint can bring them back.
    void wrap() {
        std::lock_guard lock(mutex_);
        first_usn_ = static_cast<std::int64_t>(records_.size()) + 1;
    }

    std::size_t drains() const {
        std::lock_guard lock(mutex_);
        return drains_;
    }

    // What the session last passed to the noise rules as Avel's own folders.
    std::vector<std::wstring> avel_paths() const {
        std::lock_guard lock(mutex_);
        return avel_paths_;
    }

    TraceSources sources() {
        TraceSources sources;
        sources.start_volumes = [this] {
            std::lock_guard lock(mutex_);
            VolumeTrace volume;
            volume.root = L"C:\\";
            volume.guid_path = LR"(\\?\Volume{7e570001-0000-0000-0000-000000000000}\)";
            volume.serial = kSerial;
            volume.root_frn = kRoot;
            volume.journal_id = 1;
            volume.start_usn = volume.next_usn = static_cast<std::int64_t>(records_.size()) + 1;
            return std::vector<VolumeTrace>{volume};
        };
        sources.drain_volume = [this](VolumeTrace& volume, FileChangeAggregator& changes,
                                      const TraceSources::Keep& keep) {
            std::lock_guard lock(mutex_);
            ++drains_;
            if (fail_with_ || volume.next_usn < first_usn_) {
                volume.status = fail_with_.value_or(JournalStatus::Wrapped);
                fail_with_.reset();
                return;
            }
            for (auto usn = volume.next_usn; usn <= static_cast<std::int64_t>(records_.size()); ++usn) {
                const Record& stored = records_[static_cast<std::size_t>(usn - 1)];
                platform::UsnRecord record;
                record.usn = usn;
                record.frn = stored.frn;
                record.parent_frn = stored.parent;
                record.reason = stored.reason;
                record.attributes = stored.attributes;
                record.name = stored.name;
                if (!keep || keep(record)) {
                    changes.add(record);
                }
            }
            volume.next_usn = static_cast<std::int64_t>(records_.size()) + 1;
        };
        sources.resolve_volume = [this](const VolumeTrace& volume, const FileChangeAggregator& changes,
                                        std::int64_t trace_start) {
            FakeResolver resolver({{kProgramFiles, LR"(C:\Program Files)"},
                                   {kTemp, LR"(C:\Users\Test\AppData\Local\Temp)"},
                                   {kAvelData, avel_data_}});
            return resolve_file_changes(changes, volume.root_frn, volume.root, resolver, trace_start);
        };
        sources.quiet_folders = [] { return std::vector<QuietFolder>{{kSerial, kTemp}}; };
        const auto snapshot_roots = [this] {
            return std::vector<SnapshotRoot>{{kSoftware, scratch_.get(), L"Machine", nullptr}};
        };
        sources.snapshot_roots = snapshot_roots;
        sources.snapshot_roots_at_stop = [snapshot_roots](const RegistrySnapshot&, UserHiveCache&) {
            return snapshot_roots();
        };
        const auto known_roots = [this] {
            return std::vector<KnownRoot>{{KnownSpot::MachineEnvironment, kMachineEnvironment, scratch_.get(),
                                           L"Environment", nullptr}};
        };
        sources.known_roots = known_roots;
        sources.known_roots_at_stop = [known_roots](const KnownValues&, UserHiveCache&) {
            return known_roots();
        };
        sources.noise_rules = [this](const std::vector<std::wstring>& avel_paths, UserHiveCache&) {
            {
                std::lock_guard lock(mutex_);
                avel_paths_ = avel_paths;
            }
            leftovers::ScanEnvironment environment;
            environment.machine_folders.windows = LR"(C:\Windows)";
            environment.machine_folders.program_data = LR"(C:\ProgramData)";
            leftovers::UserScope user;
            user.profile = {kUserSid, L"TEST\\Test", LR"(C:\Users\Test)", true};
            environment.users.push_back(user);
            return default_noise_rules(environment, avel_paths);
        };
        return sources;
    }

private:
    struct Record {
        std::uint64_t frn;
        std::uint64_t parent;
        std::wstring name;
        std::uint32_t reason;
        std::uint32_t attributes;
    };

    const ScratchKey& scratch_;
    std::wstring avel_data_;
    mutable std::mutex mutex_;
    std::vector<Record> records_;
    std::optional<JournalStatus> fail_with_;
    std::int64_t first_usn_ = 1;
    std::size_t drains_ = 0;
    std::vector<std::wstring> avel_paths_;
};

// "created folder C:\Program Files\Contoso [1]", "modified registry-value HKLM\SOFTWARE\X : Theme
// <temporary>": a change, compact and readable when a comparison fails.
inline std::string describe(const TraceChange& change) {
    std::string text = std::string(to_string(change.change)) + " ";
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::FileLocation>) {
                text += (item.directory ? "folder " : "file ") + platform::to_utf8(item.path);
            } else if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                text += "registry-key " + platform::to_utf8(platform::display_path(item.key));
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                text += "registry-value " + platform::to_utf8(platform::display_path(item.key)) + " : " +
                        platform::to_utf8(item.value_name);
            } else {
                text += "other";
            }
        },
        change.location);
    if (change.contents != 0) {
        text += " [" + std::to_string(change.contents) + "]";
    }
    if (change.noise != NoiseReason::None) {
        text += " <" + std::string(to_string(change.noise)) + ">";
    }
    return text;
}

inline std::vector<std::string> describe(const std::vector<TraceChange>& changes) {
    std::vector<std::string> lines;
    for (const auto& change : changes) {
        lines.push_back(describe(change));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

// LastWriteTime advances in ticks of up to 16 ms; a change in the tick the snapshot read its key
// in would keep the key's time (see registry_snapshot_test).
inline void next_clock_tick() { ::Sleep(32); }

inline bool exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline void write_text(const std::wstring& path, std::string_view text) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace avel::tracker::fakes
