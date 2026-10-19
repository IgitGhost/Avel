// Where ChangeSets live: one folder per set under %ProgramData%\Avel\Changes (ADR 0004).
#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/platform/secure_folder.h"

namespace avel::changes {

struct StoreOptions {
    std::wstring root;  // Holds one folder per set
    // Serializes every process that runs, restores or purges sets.
    std::wstring mutex_name = LR"(Global\Avel.ChangeStore)";
    // For the root and the quarantine roots on other volumes.
    platform::FolderPolicy policy = platform::admin_only_folder_policy();
    // Tests move time forward to check purging.
    std::function<std::chrono::system_clock::time_point()> clock = [] { return std::chrono::system_clock::now(); };
};

// %ProgramData%\Avel\Changes, SYSTEM and Administrators only.
StoreOptions default_store_options();

class ChangeSet;

class ChangeStore {
public:
    // Creates the root if needed and checks it (see platform::check_secure_folder). Returns null,
    // with the reason in `status`, if it can't be trusted.
    static std::unique_ptr<ChangeStore> open(StoreOptions options, platform::FolderStatus& status);

    // Held while running, restoring or purging, so the GUI and the CLI never work on sets at once.
    using Guard = wil::mutex_release_scope_exit;
    std::optional<Guard> lock(std::chrono::milliseconds timeout = std::chrono::minutes(5)) const;

    // Starts a set: writes plan.json (durably) and summary.json (running), opens the journal and
    // holds the set's lock file. Null if the plan can't be written.
    std::unique_ptr<ChangeSet> create(const ChangePlan& plan);

    // Opens an existing set for undo or recovery. Null if it doesn't exist, is unreadable, or a
    // live run holds it.
    std::unique_ptr<ChangeSet> open_set(const std::wstring& id);

    // Every set's summary, newest first.
    std::vector<SetSummary> list() const;

    // Deletes sets created more than `max_age` ago, with their quarantined files, except running
    // ones. Returns how many were deleted.
    std::size_t purge(std::chrono::hours max_age = std::chrono::days(30));

    // Deletes one set now, whatever its age (not while it runs).
    bool remove(const std::wstring& id);

    const StoreOptions& options() const { return options_; }

private:
    explicit ChangeStore(StoreOptions options);

    StoreOptions options_;
    wil::unique_mutex_nothrow mutex_;
};

class ChangeSet {
public:
    ~ChangeSet();

    const std::wstring& id() const { return id_; }
    const std::wstring& folder() const { return folder_; }
    const ChangePlan& plan() const { return plan_; }
    std::chrono::system_clock::time_point created() const { return created_; }

    // Each item's latest state, from the journal (a torn last line is ignored).
    const std::vector<ItemStatus>& statuses() const { return statuses_; }

    // Appends a journal line and flushes it before returning.
    bool record(std::size_t item, ItemStatus status);

    // items\NNNN, created on demand: the item's backup.
    std::wstring item_folder(std::size_t item);

    // Where item `item`'s files go when they're on the volume `volume_root`: files\NNNN in the
    // set folder on the store's own volume, X:\$Avel.Quarantine\<set>\NNNN on other volumes (its
    // root created and checked like the store's). Empty if that root can't be trusted.
    std::wstring quarantine_path(std::size_t item, const std::wstring& volume_root);

    // Rewrites summary.json (atomically) for the set's current statuses.
    bool write_summary(SetState state, std::uint64_t quarantined_bytes = 0);
    std::optional<SetSummary> summary() const;

private:
    friend class ChangeStore;
    ChangeSet() = default;
    bool load();

    const ChangeStore* store_ = nullptr;
    std::wstring id_;
    std::wstring folder_;
    ChangePlan plan_;
    std::chrono::system_clock::time_point created_;
    std::vector<ItemStatus> statuses_;
    wil::unique_hfile journal_;
    wil::unique_hfile lock_;
};

}  // namespace avel::changes
