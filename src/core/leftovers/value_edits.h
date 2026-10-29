// Edits to values that many programs share: the environment's variables and the SharedDLLs
// reference counts. An installer appends to Path or raises a DLL's count; removing the program must
// take back exactly that, not delete the value or put an old copy over the user's later edits.
//
// plan_value_edit turns what a trace saved about such a value (its exact bytes before and after
// the install) and the value as it is now into a ValueEditLocation. It is pure: nothing here reads
// or writes the registry. The executor applies the edit only while the value is still what was
// planned (see docs/changes.md).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/leftovers/leftover.h"
#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"

namespace avel::leftovers {

// The keys whose values can be edited (tracker::KnownSpot, in the same order).
enum class ValueSpot : std::uint8_t {
    MachineEnvironment,  // HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment
    UserEnvironment,     // HKU\<sid>\Environment
    SharedDlls,          // HKLM\SOFTWARE[\WOW6432Node]\Microsoft\Windows\CurrentVersion\SharedDLLs
};

// A value the installer added, changed or deleted in one of those keys, as its trace saved it
// (tracker::KnownValueChange, field for field).
struct TracedValue {
    ValueSpot spot = ValueSpot::MachineEnvironment;
    platform::RegistryAddress key;
    std::wstring name;
    std::optional<platform::RawValue> before;  // nullopt: the value didn't exist before the install
    std::optional<platform::RawValue> after;   // nullopt: the installer deleted it
};

// What the planned edit rests on, for the confidence it gets; or why nothing is planned.
enum class ValueEditBasis : std::uint8_t {
    // An edit is planned:
    Undo,     // Takes back exactly what the installer did: removes the entries it added to a list,
              // lowers a count by its increment, deletes a variable it created (unchanged since)
    Restore,  // Puts back the old value of a variable the installer changed or deleted. The new
              // value may be what the user wants now: Questionable, never checked by default
    // Nothing is planned:
    AlreadyUndone,      // What the installer added is gone, or the value is as before
    ChangedSince,       // The value changed after the install, and the edit would lose that
    ProtectedVariable,  // A variable Windows relies on (ComSpec, windir, TEMP, ...): shown only
    Unsupported,        // Nothing to take back: a count the installer didn't raise, unexpected data
};

struct ValueEditPlan {
    std::optional<ValueEditLocation> edit;  // Set for Undo and Restore
    ValueEditBasis basis = ValueEditBasis::Unsupported;
};

std::string_view to_string(ValueEditBasis basis);

// The edit that takes back `change`, given the value as it is now (`live`, nullopt if absent):
//   - A list (Path, PATHEXT, or any value the installer only added ;-separated entries to): the
//     installer's entries are removed from the live value, matched ignoring case, surrounding
//     quotes and a trailing backslash. Everything else stays byte for byte: the type, the user's
//     later edits, empty entries, the terminator. An entry that was there before the install stays
//     (only the installer's copy goes). Entries the installer took out stay out (an upgrade that
//     replaced its old entry), but a list it replaced whole is never left empty.
//   - A variable the installer created: deleted if unchanged since. A list keeps the entries
//     added since; it is deleted if none are left.
//   - Another variable it changed or deleted: restored, if unchanged since and not protected.
//   - A SharedDLLs count: lowered by the installer's increment; deleted at 0 if the installer
//     created it, otherwise kept at 0.
ValueEditPlan plan_value_edit(const TracedValue& change, const std::optional<platform::RawValue>& live);

// Which of the editable keys `key` is, if any.
std::optional<ValueSpot> value_spot(const platform::RegistryAddress& key);

// Variables Windows sets up and every program relies on (ComSpec, windir, TEMP, ...): never edited.
bool is_protected_variable(std::wstring_view name);
// Variables that hold ;-separated lists (Path, PATHEXT, PSModulePath, CLASSPATH, ...), so a change
// to them is entries added or taken out, never a value restored.
bool is_list_variable(std::wstring_view name);
// Whether Avel may make an edit of `kind` to the environment variable `name`. Never to a protected
// variable; to Path, PATHEXT and PSModulePath only taking entries out, or deleting a user's that the
// installer created. HardFloor allows nothing else.
bool may_edit_variable(ValueSpot spot, std::wstring_view name, ValueEditKind kind);

// The text of a REG_SZ or REG_EXPAND_SZ value, up to its terminator; nullopt for other types.
std::optional<std::wstring> string_value_text(const platform::RawValue& value);
// A SharedDLLs count: a REG_DWORD's number; nullopt for anything else.
std::optional<std::uint32_t> shared_dll_count(const platform::RawValue& value);
// `value` with its count set to `count` (a REG_DWORD).
platform::RawValue with_shared_dll_count(const platform::RawValue& value, std::uint32_t count);

// Undo of RemovePathEntries once the value changed since it was edited: the entries `original`
// had and `edited` lacks go back into `live`, each after the nearest entry before it in `original`
// that `live` has (first if none preceded it, last if those are all gone), unless `live` has them
// already; `live` itself when it has all of them. Empty entries aren't put back. `live` keeps its
// type and everything else. nullopt if a value isn't a string, or `edited` isn't `original` with
// entries taken out.
std::optional<platform::RawValue> reinsert_path_entries(const platform::RawValue& live,
                                                        const platform::RawValue& original,
                                                        const platform::RawValue& edited);

}  // namespace avel::leftovers
