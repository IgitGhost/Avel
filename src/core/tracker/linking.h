// Linking a trace to the programs it installed (docs/trace.md, "Linking"). A finished trace is
// linked to the inventory's entries whose Uninstall key it created (or changed, for an upgrade) or
// whose Windows Installer product it registered; the primary link is the entry Settings lists. A
// link keeps enough to find the program again after an upgrade, and to name it once it's gone.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/platform/registry_address.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_store.h"

namespace avel::tracker {

// What in a trace can name a program.
struct LinkCandidates {
    // Uninstall keys the trace created, and created keys that hold the Uninstall keys (a user's
    // first per-user install creates their whole Uninstall key).
    std::vector<platform::RegistryAddress> created_keys;
    // Uninstall keys that existed, whose values the trace changed (an upgrade).
    std::vector<platform::RegistryAddress> changed_keys;
    // "{GUID}" of the Windows Installer products it registered: its Products and Features keys,
    // Uninstall keys named after a product code. Upper case.
    std::vector<std::wstring> product_codes;
    // Folders it created, normalized: an entry whose install folder, uninstaller or icon is in one
    // of them is what the installer laid down, and comes first among entries Settings lists alike.
    std::vector<std::wstring> created_folders;
};

// The candidates among a trace's changes; noise is left out.
LinkCandidates link_candidates(const TraceResult& result);

// The Uninstall key of an inventory entry, as the trace reports keys (WOW6432Node spelled out);
// nullopt for entries without one. Built on uninstall::uninstall_key_address.
std::optional<platform::RegistryAddress> uninstall_key_of(const inventory::Program& program);

// The link that `program` would get, as it is now.
TraceLink make_link(const inventory::Program& program, LinkMatch matched_by);

// The inventory's entries the candidates name: first those whose Uninstall key is (in) a created
// key, then those whose key the trace changed, then those of a registered product code, each
// entry once. The primary link comes first: preferring entries Settings lists, then those whose
// files are in a folder the trace created (not one that only registered alongside, such as the
// compatibility fix database Windows installed meanwhile), then in that order.
std::vector<TraceLink> find_links(const LinkCandidates& candidates, const inventory::Inventory& inventory);

// The installed program a link leads to now, or null:
//   1. the entry with the same id;
//   2. the entry at the same Uninstall key;
//   3. the entry of the same Windows Installer product;
//   4. carefully, after an upgrade moved it: the one entry (or the one Settings lists) with the
//      same name once versions are stripped, of the same user, and with the same publisher or
//      install folder.
const inventory::Program* resolve_link(const TraceLink& link, const inventory::Inventory& inventory);

struct ResolvedProgram {
    std::size_t link = 0;  // Index into TraceInfo::links
    const inventory::Program* program = nullptr;
};

// The trace's program now: its primary link's, or failing that the first other link that leads to
// an installed program.
std::optional<ResolvedProgram> resolve_trace_program(const TraceInfo& info,
                                                     const inventory::Inventory& inventory);

// Brings every link that leads to an installed program up to date with it (its id, version, key,
// product code and folder after an upgrade). True if any does: this is retention's "installed"
// (TraceStore::purge).
bool refresh_links(TraceInfo& info, const inventory::Inventory& inventory);

// The finished traces whose links lead to `program` now, newest first (`traces` as
// TraceStore::list gives them). Package C uses the first as "the trace of program X".
std::vector<std::wstring> traces_of(const inventory::Program& program, std::span<const TraceInfo> traces,
                                    const inventory::Inventory& inventory);

// Sets the trace's automatic links: those found for `result`, after the links set by hand, which
// stay (and stay primary). Clears `unresolved_since` when a link leads to an installed program.
void set_links(TraceInfo& info, const TraceResult& result, const inventory::Inventory& inventory);

// Links the trace to `program` by hand: the link becomes primary, ahead of the others; an existing
// link to the same program is replaced.
void add_manual_link(TraceInfo& info, const inventory::Program& program);

// Resolves the links of a trace recorded elsewhere again (`trace import`): links that lead to an
// installed program here are brought up to date with it (refresh_links), the others are kept as
// they are, and entries `result` names that no link has yet are added after them. Clears
// `unresolved_since`.
void relink(TraceInfo& info, const TraceResult& result, const inventory::Inventory& inventory);

// Links a finished trace in the store automatically (set_links) and writes trace.json: what
// `trace stop`, `trace run` and the GUI do once a trace is stopped. Returns the trace as written,
// or nullopt with the reason (see TraceStore::update).
std::optional<TraceInfo> link_trace(TraceStore& store, const std::wstring& id, const TraceResult& result,
                                    const inventory::Inventory& inventory, TraceError& error);

}  // namespace avel::tracker
