// Removal of browser extensions (tools/browser_extensions): the ChangePlan that takes out every stubborn
// source of the chosen extensions, so the browser uninstalls them at its next start. Only registry
// sources are changed: a policy loses only the extension's own entry, a registration its key or value.
// Profiles' files and Firefox's policies.json are never written. Codes and data only; the front ends
// turn them into text. See docs/tools.md ("Browser extensions") and ADR 0009.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/changes/change_set.h"
#include "core/leftovers/protection.h"
#include "core/tools/browser_extensions.h"

namespace avel::tools {

enum class ExtensionPlanIssueCode : std::uint8_t {
    UnknownId,  // No entry has this id, and no entry this extension id
    // No stubborn source (ExtensionRemoval::NotRemovable): the user or the store installed it, and the
    // browser's extensions page removes it
    NotRemovable,
    ManagedByPoliciesJson,  // Firefox's policies.json installs it: never edited
    NotSupported,           // A policy's subkey names it with other ids ("id1,id2")
    Protected,              // A protection layer refuses one of its sources
};

struct ExtensionPlanIssue {
    std::wstring id;  // As asked
    ExtensionPlanIssueCode code;
};

// Something the front ends tell about an entry the plan takes sources from; not an error.
enum class ExtensionPlanNoteCode : std::uint8_t {
    // BrowserExtension::keeps_user_copy: the profile's own copy (from the store or by the user) stays;
    // the browser's extensions page removes it
    KeepsUserCopy,
};

struct ExtensionPlanNote {
    std::wstring id;  // The entry's id (one of `affected`)
    ExtensionPlanNoteCode code;
};

struct ExtensionsPlan {
    changes::ChangePlan plan;  // Its description is the caller's to set
    // The ids of the entries asked for that the plan removes, in order (an extension id asked for adds
    // every removable entry of that extension).
    std::vector<std::wstring> planned;
    // Every entry the plan takes a source from, the planned ones included: a machine-wide policy installs
    // an extension in every profile of its browser, for every user. Each is an id of the list's entries,
    // which give its browser, user, profile and name (the CLI's JSON writes them out).
    std::vector<std::wstring> affected;
    std::vector<ExtensionPlanIssue> issues;  // In the order of the ids
    std::vector<ExtensionPlanNote> notes;    // In the order of `affected`
};

// The plan for removing the entries with these ids (an entry's id, or an extension id for all its
// entries; compared ignoring case, each planned once). It takes every stubborn source of each:
//   - a numbered list (ExtensionInstallForcelist, Firefox's Extensions\Install and Locked) loses the
//     entry, and the values after it in the run from "1" move down one, so the list keeps no gap; each
//     value written or deleted is a ValueEditLocation (RestoreValue: compare-and-set, exact undo);
//   - a JSON ExtensionSettings value loses the id's key (or the id from a "id1,id2" key), re-serialized
//     compactly, or is deleted when nothing is left (a ValueEditLocation);
//   - an ExtensionSettings subkey and an external registration key go whole (RegistryKeyLocation), a
//     Firefox registry value as a value (RegistryValueLocation).
// Several extensions in one list or JSON value make one edit per value. An id with an issue plans
// nothing. `protection`, when set, is asked about each source first (HardFloor and ProtectedPolicy in
// the front ends; the executor asks them again anyway). Planned items carry the extension's name as
// their program and no confidence: the user chose them.
ExtensionsPlan plan_remove_extensions(
    const BrowserExtensionsList& list, std::span<const std::wstring> ids,
    const std::function<leftovers::ProtectionReason(const leftovers::Location&)>& protection = {});

std::string_view to_string(ExtensionPlanIssueCode code);
std::string_view to_string(ExtensionPlanNoteCode code);

}  // namespace avel::tools
