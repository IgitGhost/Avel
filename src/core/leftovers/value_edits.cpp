#include "core/leftovers/value_edits.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

#include "core/platform/text.h"

namespace avel::leftovers {

namespace {

using platform::RawValue;

constexpr std::wstring_view kMachineEnvironment =
    LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)";
constexpr std::wstring_view kSharedDlls = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)";
constexpr std::wstring_view kSharedDlls32 =
    LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)";

// Windows' own lists: an installer's entries may go, the value itself never (but a user's, which
// the installer created).
constexpr std::wstring_view kWindowsLists[] = {L"Path", L"PATHEXT", L"PSModulePath"};
// Other variables that hold lists of folders.
constexpr std::wstring_view kOtherLists[] = {L"CLASSPATH", L"INCLUDE",    L"LIB",
                                             L"LIBPATH",   L"PYTHONPATH", L"PERL5LIB"};

// Variables Windows sets up and programs rely on: restoring an old value or deleting one could
// break every program that starts afterwards.
constexpr std::wstring_view kProtectedVariables[] = {
    L"ComSpec", L"windir", L"SystemRoot", L"SystemDrive", L"TEMP", L"TMP", L"OS",
    L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL", L"PROCESSOR_REVISION",
    L"NUMBER_OF_PROCESSORS", L"DriverData", L"USERNAME", L"USERPROFILE", L"USERDOMAIN", L"APPDATA",
    L"LOCALAPPDATA", L"HOMEDRIVE", L"HOMEPATH", L"HOMESHARE", L"ProgramData", L"ProgramFiles",
    L"ProgramFiles(x86)", L"ProgramW6432", L"CommonProgramFiles", L"CommonProgramFiles(x86)",
    L"CommonProgramW6432", L"ALLUSERSPROFILE", L"PUBLIC", L"OneDrive", L"OneDriveConsumer",
    L"OneDriveCommercial", L"__PSLockdownPolicy"};

template <std::size_t N>
bool in_list(const std::wstring_view (&list)[N], std::wstring_view name) {
    return std::any_of(std::begin(list), std::end(list),
                       [&](auto item) { return platform::iequals(item, name); });
}

bool same(const std::optional<RawValue>& a, const std::optional<RawValue>& b) {
    return a.has_value() == b.has_value() && (!a || (a->type == b->type && a->data == b->data));
}

// A string value cut at its terminator: the text, and the terminator with whatever follows it (an
// unterminated value has none), which an edit keeps byte for byte.
struct StringData {
    std::wstring text;
    std::vector<std::byte> tail;
};

std::optional<StringData> split_string(const RawValue& value) {
    if (value.type != REG_SZ && value.type != REG_EXPAND_SZ) {
        return std::nullopt;
    }
    const std::size_t chars = value.data.size() / sizeof(wchar_t);
    std::wstring all(chars, L'\0');
    if (chars > 0) {
        std::memcpy(all.data(), value.data.data(), chars * sizeof(wchar_t));
    }
    const std::size_t end = std::min(all.find(L'\0'), chars);
    StringData data;
    data.text = all.substr(0, end);
    data.tail.assign(value.data.begin() + static_cast<std::ptrdiff_t>(end * sizeof(wchar_t)),
                     value.data.end());
    return data;
}

RawValue join_string(const RawValue& like, std::wstring_view text, const std::vector<std::byte>& tail) {
    RawValue value{like.name, like.type, {}};
    value.data.resize(text.size() * sizeof(wchar_t));
    if (!text.empty()) {
        std::memcpy(value.data.data(), text.data(), value.data.size());
    }
    value.data.insert(value.data.end(), tail.begin(), tail.end());
    return value;
}

// A list's entries, as written. An empty text has none; "a;;b;" has four, two of them empty.
std::vector<std::wstring> split_entries(std::wstring_view text) {
    std::vector<std::wstring> entries;
    if (text.empty()) {
        return entries;
    }
    for (std::size_t start = 0;;) {
        const std::size_t end = text.find(L';', start);
        if (end == std::wstring_view::npos) {
            entries.emplace_back(text.substr(start));
            return entries;
        }
        entries.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
}

std::wstring join_entries(const std::vector<std::wstring>& entries) {
    std::wstring text;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            text += L';';
        }
        text += entries[i];
    }
    return text;
}

// What entries are compared by: "C:\Tools\", "c:\tools" and "\"C:\Tools\"" are one entry. A root
// keeps its backslash ("C:\" isn't "C:").
std::wstring entry_key(std::wstring_view entry) {
    entry = platform::trim(entry);
    if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') {
        entry = platform::trim(entry.substr(1, entry.size() - 2));
    }
    while (entry.size() > 1 && (entry.back() == L'\\' || entry.back() == L'/') &&
           entry[entry.size() - 2] != L':') {
        entry.remove_suffix(1);
    }
    return platform::to_lower(entry);
}

struct Added {
    std::wstring text;  // As the installer wrote it
    bool appended;      // No entry that was there before follows it
};

// The entries `after` has beyond those of `before`: a multiset difference by entry_key, each entry
// of `before` accounting for one of `after`. `kept` tells whether all of `before`'s are still there.
std::vector<Added> added_entries(const std::vector<std::wstring>& before,
                                 const std::vector<std::wstring>& after, bool& kept) {
    std::vector<bool> matched(after.size(), false);
    // In order first, so an entry appended twice is told from the one already there.
    std::size_t next = 0;
    for (std::size_t i = 0; i < after.size() && next < before.size(); ++i) {
        if (entry_key(after[i]) == entry_key(before[next])) {
            matched[i] = true;
            ++next;
        }
    }
    kept = next == before.size();
    if (!kept) {
        // Reordered, or entries taken out: match each one wherever it is.
        std::fill(matched.begin(), matched.end(), false);
        kept = true;
        for (const auto& entry : before) {
            const std::wstring key = entry_key(entry);
            bool found = false;
            for (std::size_t i = 0; i < after.size() && !found; ++i) {
                if (!matched[i] && entry_key(after[i]) == key) {
                    matched[i] = found = true;
                }
            }
            kept = kept && found;
        }
    }
    std::size_t last = after.size();
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (matched[i]) {
            last = i;
        }
    }
    std::vector<Added> added;
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (!matched[i]) {
            added.push_back({after[i], last == after.size() || i > last});
        }
    }
    return added;
}

// Takes each of `added` out of `entries` once: preferably an entry written exactly as the
// installer wrote it, else one that only compares equal; of several, the last for an appended
// entry and the first otherwise. Returns the entries taken out, as written, in their order.
std::vector<std::wstring> remove_entries(std::vector<std::wstring>& entries,
                                         const std::vector<Added>& added) {
    std::vector<bool> removed(entries.size(), false);
    for (const Added& entry : added) {
        const std::wstring key = entry_key(entry.text);
        std::optional<std::size_t> exact;
        std::optional<std::size_t> similar;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (removed[i]) {
                continue;
            }
            if (entries[i] == entry.text) {
                if (!exact || entry.appended) {
                    exact = i;
                }
            } else if (entry_key(entries[i]) == key && (!similar || entry.appended)) {
                similar = i;
            }
        }
        if (const auto pick = exact ? exact : similar) {
            removed[*pick] = true;
        }
    }
    std::vector<std::wstring> taken;
    std::vector<std::wstring> kept;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        (removed[i] ? taken : kept).push_back(std::move(entries[i]));
    }
    entries = std::move(kept);
    return taken;
}

ValueEditLocation edit_of(const TracedValue& change, ValueEditKind kind,
                          const std::optional<RawValue>& expected, std::optional<RawValue> desired) {
    ValueEditLocation edit{change.key, change.name, kind, expected, std::move(desired), {}, 0};
    for (auto* value : {&edit.expected, &edit.desired}) {
        if (*value) {
            (*value)->name = change.name;
        }
    }
    return edit;
}

// Entries the installer added to a list; nullopt when the change isn't one.
std::optional<ValueEditPlan> plan_list(const TracedValue& change, const std::optional<RawValue>& live) {
    const bool list = is_list_variable(change.name);
    const auto after = change.after ? split_string(*change.after) : std::nullopt;
    const auto before = change.before ? split_string(*change.before) : std::nullopt;
    if (!after || (change.before && !before)) {
        return std::nullopt;  // Deleted, or not a string
    }
    std::vector<Added> added;
    bool kept = true;
    if (before) {
        added = added_entries(split_entries(before->text), split_entries(after->text), kept);
        if (!list && (!kept || added.empty())) {
            return std::nullopt;  // Not entries added: the whole value changed
        }
        if (added.empty()) {
            return ValueEditPlan{std::nullopt, ValueEditBasis::Unsupported};  // Only taken out or reordered
        }
    } else {
        if (!list || same(live, change.after)) {
            return std::nullopt;  // Created: deleted as a whole if unchanged
        }
        for (auto& entry : split_entries(after->text)) {
            added.push_back({std::move(entry), true});
        }
    }
    if (!live) {
        return ValueEditPlan{std::nullopt, ValueEditBasis::AlreadyUndone};
    }
    const auto current = split_string(*live);
    if (!current) {
        return ValueEditPlan{std::nullopt, ValueEditBasis::ChangedSince};
    }
    auto entries = split_entries(current->text);
    auto taken = remove_entries(entries, added);
    if (taken.empty()) {
        return ValueEditPlan{std::nullopt, ValueEditBasis::AlreadyUndone};
    }
    const bool nothing_left = std::all_of(entries.begin(), entries.end(),
                                          [](const auto& entry) { return platform::trim(entry).empty(); });
    if (!change.before && nothing_left) {
        return ValueEditPlan{edit_of(change, ValueEditKind::DeleteValue, live, std::nullopt),
                             ValueEditBasis::Undo};
    }
    if (!kept && nothing_left) {
        // The installer replaced the whole list: taking its entries out would leave none.
        return ValueEditPlan{std::nullopt, ValueEditBasis::Unsupported};
    }
    auto edit = edit_of(change, ValueEditKind::RemovePathEntries, live,
                        join_string(*live, join_entries(entries), current->tail));
    edit.entries = std::move(taken);
    return ValueEditPlan{std::move(edit), ValueEditBasis::Undo};
}

ValueEditPlan plan_variable(const TracedValue& change, const std::optional<RawValue>& live) {
    if (is_protected_variable(change.name)) {
        return {std::nullopt, ValueEditBasis::ProtectedVariable};
    }
    ValueEditPlan plan;
    if (auto list = plan_list(change, live)) {
        plan = std::move(*list);
    } else if (same(live, change.before)) {
        plan = {std::nullopt, ValueEditBasis::AlreadyUndone};
    } else if (!same(live, change.after)) {
        plan = {std::nullopt, ValueEditBasis::ChangedSince};
    } else if (!change.before) {
        plan = {edit_of(change, ValueEditKind::DeleteValue, live, std::nullopt), ValueEditBasis::Undo};
    } else {
        plan = {edit_of(change, ValueEditKind::RestoreValue, live, change.before), ValueEditBasis::Restore};
    }
    if (plan.edit && !may_edit_variable(change.spot, change.name, plan.edit->kind)) {
        return {std::nullopt, ValueEditBasis::ProtectedVariable};
    }
    return plan;
}

ValueEditPlan plan_shared_dll(const TracedValue& change, const std::optional<RawValue>& live) {
    const auto after = change.after ? shared_dll_count(*change.after) : std::nullopt;
    const auto before = change.before ? shared_dll_count(*change.before) : std::optional<std::uint32_t>(0);
    if (!after || !before || *after <= *before) {
        return {std::nullopt, ValueEditBasis::Unsupported};  // The installer added no reference
    }
    if (!live) {
        return {std::nullopt, ValueEditBasis::AlreadyUndone};
    }
    const auto count = shared_dll_count(*live);
    if (!count) {
        return {std::nullopt, ValueEditBasis::ChangedSince};
    }
    const std::int64_t increment = static_cast<std::int64_t>(*after) - *before;
    const std::int64_t lowered = std::max<std::int64_t>(0, static_cast<std::int64_t>(*count) - increment);
    if (lowered == 0 && !change.before) {
        auto edit = edit_of(change, ValueEditKind::DeleteValue, live, std::nullopt);
        edit.delta = *count;
        return {std::move(edit), ValueEditBasis::Undo};
    }
    if (lowered == *count) {
        return {std::nullopt, ValueEditBasis::AlreadyUndone};  // At 0 already
    }
    auto edit = edit_of(change, ValueEditKind::DecrementSharedDll, live,
                        with_shared_dll_count(*live, static_cast<std::uint32_t>(lowered)));
    edit.delta = *count - lowered;
    return {std::move(edit), ValueEditBasis::Undo};
}

}  // namespace

ValueEditPlan plan_value_edit(const TracedValue& change, const std::optional<RawValue>& live) {
    return change.spot == ValueSpot::SharedDlls ? plan_shared_dll(change, live) : plan_variable(change, live);
}

std::string_view to_string(ValueEditBasis basis) {
    switch (basis) {
        case ValueEditBasis::Undo:
            return "undo";
        case ValueEditBasis::Restore:
            return "restore";
        case ValueEditBasis::AlreadyUndone:
            return "already-undone";
        case ValueEditBasis::ChangedSince:
            return "changed-since";
        case ValueEditBasis::ProtectedVariable:
            return "protected-variable";
        case ValueEditBasis::Unsupported:
            return "unsupported";
    }
    return "unknown";
}

std::optional<ValueSpot> value_spot(const platform::RegistryAddress& key) {
    if (key.hive == platform::Hive::Machine) {
        if (platform::iequals(key.path, kMachineEnvironment)) {
            return ValueSpot::MachineEnvironment;
        }
        if (platform::iequals(key.path, kSharedDlls) || platform::iequals(key.path, kSharedDlls32)) {
            return ValueSpot::SharedDlls;
        }
        return std::nullopt;
    }
    if (!key.sid.empty() && !platform::iends_with(key.sid, L"_Classes") &&
        platform::iequals(key.path, L"Environment")) {
        return ValueSpot::UserEnvironment;
    }
    return std::nullopt;
}

bool is_protected_variable(std::wstring_view name) { return in_list(kProtectedVariables, name); }

bool is_list_variable(std::wstring_view name) {
    return in_list(kWindowsLists, name) || in_list(kOtherLists, name);
}

bool may_edit_variable(ValueSpot spot, std::wstring_view name, ValueEditKind kind) {
    if (spot == ValueSpot::SharedDlls || kind == ValueEditKind::DecrementSharedDll ||
        is_protected_variable(name)) {
        return false;
    }
    if (in_list(kWindowsLists, name)) {
        return kind == ValueEditKind::RemovePathEntries ||
               (kind == ValueEditKind::DeleteValue && spot == ValueSpot::UserEnvironment);
    }
    return kind != ValueEditKind::RestoreValue || !is_list_variable(name);
}

std::optional<std::wstring> string_value_text(const RawValue& value) {
    auto data = split_string(value);
    return data ? std::optional<std::wstring>(std::move(data->text)) : std::nullopt;
}

std::optional<std::uint32_t> shared_dll_count(const RawValue& value) {
    if (value.type != REG_DWORD || value.data.size() != sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, value.data.data(), sizeof(count));
    return count;
}

RawValue with_shared_dll_count(const RawValue& value, std::uint32_t count) {
    RawValue result{value.name, REG_DWORD, std::vector<std::byte>(sizeof(count))};
    std::memcpy(result.data.data(), &count, sizeof(count));
    return result;
}

std::optional<RawValue> reinsert_path_entries(const RawValue& live, const RawValue& original,
                                              const RawValue& edited) {
    const auto current = split_string(live);
    const auto old_text = split_string(original);
    const auto new_text = split_string(edited);
    if (!current || !old_text || !new_text) {
        return std::nullopt;
    }
    const auto old_entries = split_entries(old_text->text);
    const auto kept_entries = split_entries(new_text->text);
    // The edit took entries out and changed nothing else, so `edited` is `original` minus some.
    std::vector<bool> removed(old_entries.size(), true);
    std::size_t next = 0;
    for (std::size_t i = 0; i < old_entries.size() && next < kept_entries.size(); ++i) {
        if (old_entries[i] == kept_entries[next]) {
            removed[i] = false;
            ++next;
        }
    }
    if (next != kept_entries.size()) {
        return std::nullopt;
    }
    auto entries = split_entries(current->text);
    const auto find = [&](const std::wstring& entry) -> std::optional<std::size_t> {
        const auto exact = std::find(entries.begin(), entries.end(), entry);
        if (exact != entries.end()) {
            return static_cast<std::size_t>(exact - entries.begin());
        }
        const std::wstring key = entry_key(entry);
        const auto similar = std::find_if(entries.begin(), entries.end(), [&](const std::wstring& candidate) {
            return entry_key(candidate) == key;
        });
        return similar == entries.end()
                   ? std::nullopt
                   : std::optional<std::size_t>(static_cast<std::size_t>(similar - entries.begin()));
    };
    bool changed = false;
    for (std::size_t i = 0; i < old_entries.size(); ++i) {
        // Empty entries mean nothing; only the exact undo brings them back.
        if (!removed[i] || platform::trim(old_entries[i]).empty() || find(old_entries[i])) {
            continue;
        }
        // After the nearest entry before it that `live` has (one just put back included); first
        // if nothing preceded it; otherwise last, before the empty entries of a trailing ';'.
        std::size_t at = 0;
        if (i > 0) {
            at = entries.size();
            while (at > 0 && entries[at - 1].empty()) {
                --at;
            }
            for (std::size_t j = i; j-- > 0;) {
                if (const auto found = find(old_entries[j])) {
                    at = *found + 1;
                    break;
                }
            }
        }
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(at), old_entries[i]);
        changed = true;
    }
    return changed ? join_string(live, join_entries(entries), current->tail) : live;
}

}  // namespace avel::leftovers
