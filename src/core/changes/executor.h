// Runs a ChangePlan: every item is backed up before it changes, and every change is recorded, so
// it can be undone exactly and survives a crash (see docs/changes.md).
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/changes/locks.h"
#include "core/changes/store.h"
#include "core/leftovers/protection.h"

namespace avel::changes {

struct ExecuteOptions {
    // Packages can't be put back; they're only removed when this allows it.
    bool allow_irreversible = false;
    // The registry root for a hive: HKLM, or a user's hive. The default opens the live registry
    // (HKU\<sid>, or the profile's NTUSER.DAT loaded for writing while the user is signed out).
    std::function<HKEY(platform::Hive hive, const std::wstring& sid)> registry_root;
    // Checked again right before each item changes. The default is HardFloor, then
    // ProtectedPolicy for the known folders of the machine and every user.
    std::function<leftovers::ProtectionReason(const leftovers::Location&)> protection;
    // Tests and the Sandbox: behave as if the process died after this many journal records.
    std::optional<std::size_t> stop_after;
    // Called when stop_after is reached; the default just stops. The CLI ends the process here.
    std::function<void()> on_stop;
    // Called once at the end of a run that edited an environment variable (or undid or settled such
    // an edit). The default broadcasts WM_SETTINGCHANGE "Environment", so Explorer and the programs
    // started afterwards see the change. Tests replace it.
    std::function<void()> environment_changed;
};

struct ItemResult {
    ItemStatus status;
    std::vector<std::wstring> left_behind;  // Files in use that stayed
    std::vector<LockingProcess> holders;    // Who holds them, if Windows could tell
};

struct ExecuteResult {
    std::wstring set_id;  // Empty if the set couldn't be created
    std::vector<ItemResult> items;  // In plan order
    std::uint64_t quarantined_bytes = 0;
    bool stopped = false;  // stop_after was reached
};

struct ChangeEvents {
    std::function<void(std::size_t index, const ItemResult& result)> item_finished;
};

// Removes the plan's items, in a safe order: services and tasks first (so their files are free),
// then task folders (deepest first, once their tasks are gone), packages, registry values and value
// edits, state changes, registry keys, and files last. An item inside another planned item is left
// to go with it. A value edit or a state change (ADR 0007) is applied only while the item is still
// what it expects; otherwise it is skipped as `changed`. Hold store.lock() around this.
ExecuteResult execute(ChangeStore& store, const ChangePlan& plan, const ExecuteOptions& options = {},
                      const ChangeEvents& events = {});

struct RestoreResult {
    bool found = false;  // The set exists and no live run holds it
    std::vector<std::pair<std::size_t, ItemResult>> items;  // The items it tried, by plan index
    SetState state = SetState::Complete;
};

// Undoes a set, in the reverse order of removal: all of its removed items, or only `only`. Nothing
// that has appeared in an item's place since is ever replaced. A value edit is reverted exactly
// while the value is still what was written; otherwise its inverse is applied (the entries put back
// into the list, the count raised again) where there is one. A state change goes back only while
// the item is in the state it set (otherwise `restore-skipped (changed)`). events.item_finished is
// called for each item it tries.
RestoreResult restore(ChangeStore& store, const std::wstring& id, const std::vector<std::size_t>& only = {},
                      const ExecuteOptions& options = {}, const ChangeEvents& events = {});

// Settles sets a crash left running: each item backed up but not finished is checked against the
// system (still in place: untouched; gone: removed; a value edit or a state change: done if the
// item is in the state written), items never reached are marked skipped.
// Returns the ids of the sets it settled.
std::vector<std::wstring> recover(ChangeStore& store, const ExecuteOptions& options = {});

}  // namespace avel::changes
