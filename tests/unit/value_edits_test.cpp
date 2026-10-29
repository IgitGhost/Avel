// Planning edits to shared values from what a trace saved: the installer's PATH entries taken out
// of the live value (which otherwise stays byte for byte), created variables deleted, changed ones
// restored, SharedDLLs counts lowered. Pure: no registry involved.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/leftovers/value_edits.h"
#include "core/platform/text.h"

namespace avel::leftovers {
namespace {

using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

const RegistryAddress kMachineEnvironment{Hive::Machine, L"",
                                          LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"};
const RegistryAddress kUserEnvironment{Hive::User, L"S-1-5-21-1-2-3-1001", L"Environment"};
const RegistryAddress kSharedDlls{Hive::Machine, L"",
                                  LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"};
const RegistryAddress kSharedDlls32{Hive::Machine, L"",
                                    LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)"};

constexpr wchar_t kSystemPath[] = LR"(%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem)";

// A string value with its terminator, or without one (as some programs store them).
RawValue text(std::wstring_view value, DWORD type = REG_EXPAND_SZ, bool terminated = true) {
    RawValue raw{L"Path", type, {}};
    raw.data.resize((value.size() + (terminated ? 1 : 0)) * sizeof(wchar_t));
    if (!value.empty()) {
        std::memcpy(raw.data.data(), value.data(), value.size() * sizeof(wchar_t));
    }
    return raw;
}

RawValue count(std::uint32_t number) {
    RawValue raw{L"C:\\Shared\\contoso.dll", REG_DWORD, std::vector<std::byte>(4)};
    std::memcpy(raw.data.data(), &number, 4);
    return raw;
}

TracedValue path_change(std::optional<RawValue> before, std::optional<RawValue> after,
                        const RegistryAddress& key = kMachineEnvironment,
                        ValueSpot spot = ValueSpot::MachineEnvironment) {
    return {spot, key, L"Path", std::move(before), std::move(after)};
}

TracedValue variable(std::wstring name, std::optional<RawValue> before, std::optional<RawValue> after) {
    for (auto* value : {&before, &after}) {
        if (*value) {
            (*value)->name = name;
        }
    }
    return {ValueSpot::MachineEnvironment, kMachineEnvironment, std::move(name), std::move(before),
            std::move(after)};
}

TracedValue shared_dll(std::optional<RawValue> before, std::optional<RawValue> after,
                       const RegistryAddress& key = kSharedDlls) {
    return {ValueSpot::SharedDlls, key, L"C:\\Shared\\contoso.dll", std::move(before), std::move(after)};
}

// A string value's text, with "<no terminator>" when the data stops without one.
std::wstring text_of(const RawValue& value) {
    std::wstring all(value.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(all.data(), value.data.data(), all.size() * sizeof(wchar_t));
    if (!all.empty() && all.back() == L'\0') {
        all.pop_back();
        return all;
    }
    return all + L"<no terminator>";
}

// The edit's result as text.
std::wstring written_text(const ValueEditPlan& plan) {
    return plan.edit && plan.edit->desired ? text_of(*plan.edit->desired) : L"<none>";
}

void expect_removes(const ValueEditPlan& plan, const RawValue& live, const std::wstring& result,
                    const std::vector<std::wstring>& entries) {
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.basis, ValueEditBasis::Undo);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::RemovePathEntries);
    ASSERT_TRUE(plan.edit->expected);
    EXPECT_EQ(plan.edit->expected->data, live.data);
    EXPECT_EQ(plan.edit->expected->type, live.type);
    ASSERT_TRUE(plan.edit->desired);
    EXPECT_EQ(plan.edit->desired->type, live.type);
    EXPECT_EQ(written_text(plan), result);
    EXPECT_EQ(plan.edit->entries, entries);
}

TEST(ValueEditsTest, AnAppendedEntryIsTakenOutAndTheRestStaysByteForByte) {
    const RawValue before = text(kSystemPath);
    const RawValue after = text(kSystemPath + std::wstring(LR"(;C:\Contoso\bin)"));
    const auto plan = plan_value_edit(path_change(before, after), after);
    expect_removes(plan, after, kSystemPath, {LR"(C:\Contoso\bin)"});
    EXPECT_EQ(plan.edit->desired->data, before.data);
    EXPECT_EQ(plan.edit->desired->type, static_cast<DWORD>(REG_EXPAND_SZ));
    EXPECT_EQ(plan.edit->value_name, L"Path");
    EXPECT_EQ(plan.edit->desired->name, L"Path");
}

TEST(ValueEditsTest, APrependedEntryIsTakenOut) {
    const RawValue before = text(kSystemPath);
    const RawValue after = text(LR"(C:\Contoso\bin;)" + std::wstring(kSystemPath));
    expect_removes(plan_value_edit(path_change(before, after), after), after, kSystemPath,
                   {LR"(C:\Contoso\bin)"});
}

TEST(ValueEditsTest, TheUsersLaterEditsStay) {
    const RawValue before = text(LR"(C:\a;C:\b)");
    const RawValue after = text(LR"(C:\a;C:\b;C:\Contoso\bin)");
    // Since the install the user added an entry and moved another.
    const RawValue live = text(LR"(C:\b;C:\Contoso\bin;C:\a;D:\Tools)");
    expect_removes(plan_value_edit(path_change(before, after), live), live, LR"(C:\b;C:\a;D:\Tools)",
                   {LR"(C:\Contoso\bin)"});
}

TEST(ValueEditsTest, AnEntryThatWasThereBeforeStays) {
    // The installer appended an entry the value already had: only its copy goes.
    const RawValue before = text(LR"(C:\a;C:\Contoso\bin;C:\b)");
    const RawValue after = text(LR"(C:\a;C:\Contoso\bin;C:\b;C:\Contoso\bin)");
    expect_removes(plan_value_edit(path_change(before, after), after), after, LR"(C:\a;C:\Contoso\bin;C:\b)",
                   {LR"(C:\Contoso\bin)"});
    // The same, written differently before: the copy written as the installer wrote it goes.
    const RawValue spelled = text(LR"(c:\contoso\BIN\;C:\a)");
    const RawValue appended = text(LR"(c:\contoso\BIN\;C:\a;C:\Contoso\bin)");
    expect_removes(plan_value_edit(path_change(spelled, appended), appended), appended,
                   LR"(c:\contoso\BIN\;C:\a)", {LR"(C:\Contoso\bin)"});
}

TEST(ValueEditsTest, NothingIsPlannedWhenTheUserAlreadyRemovedTheEntry) {
    const RawValue before = text(LR"(C:\a;C:\b)");
    const RawValue after = text(LR"(C:\a;C:\b;C:\Contoso\bin)");
    const auto plan = plan_value_edit(path_change(before, after), text(LR"(C:\a;C:\b;D:\Tools)"));
    EXPECT_FALSE(plan.edit);
    EXPECT_EQ(plan.basis, ValueEditBasis::AlreadyUndone);
    EXPECT_EQ(plan_value_edit(path_change(before, after), std::nullopt).basis, ValueEditBasis::AlreadyUndone);
}

TEST(ValueEditsTest, OnlyTheEntriesStillThereAreTakenOut) {
    const RawValue before = text(LR"(C:\a)");
    const RawValue after = text(LR"(C:\a;C:\Contoso\bin;C:\Contoso\tools)");
    const RawValue live = text(LR"(C:\a;C:\Contoso\tools)");
    expect_removes(plan_value_edit(path_change(before, after), live), live, LR"(C:\a)",
                   {LR"(C:\Contoso\tools)"});
}

TEST(ValueEditsTest, TrailingSemicolonsAndEmptyEntriesAreKept) {
    // The value ended with ';' before, and the installer inserted its entry before it.
    const RawValue before = text(LR"(C:\a;)");
    const RawValue after = text(LR"(C:\a;C:\Contoso\bin;)");
    expect_removes(plan_value_edit(path_change(before, after), after), after, LR"(C:\a;)",
                   {LR"(C:\Contoso\bin)"});
    // A naive append after the ';' makes an empty entry, which was there already.
    const RawValue naive = text(LR"(C:\a;;C:\Contoso\bin)");
    expect_removes(plan_value_edit(path_change(before, naive), naive), naive, LR"(C:\a;)",
                   {LR"(C:\Contoso\bin)"});
    // The installer's own trailing ';' goes with its entry.
    const RawValue bare = text(LR"(C:\a)");
    const RawValue with_semicolon = text(LR"(C:\a;C:\Contoso\bin;)");
    expect_removes(plan_value_edit(path_change(bare, with_semicolon), with_semicolon), with_semicolon,
                   LR"(C:\a)", {LR"(C:\Contoso\bin)", L""});
    // An empty entry of the user's elsewhere stays.
    const RawValue user = text(LR"(C:\a;;D:\x;C:\Contoso\bin;)");
    expect_removes(plan_value_edit(path_change(bare, with_semicolon), user), user, LR"(C:\a;;D:\x)",
                   {LR"(C:\Contoso\bin)", L""});
}

TEST(ValueEditsTest, EntriesMatchIgnoringCaseQuotesAndATrailingBackslash) {
    const RawValue before = text(LR"(C:\a)");
    const RawValue after = text(LR"(C:\a;C:\Program Files\Contoso\bin\)");
    // A tool rewrote the entry since.
    for (const auto* written : {LR"(c:\program files\contoso\BIN)", LR"("C:\Program Files\Contoso\bin")",
                                LR"(C:\Program Files\Contoso\bin\\)"}) {
        const RawValue live = text(LR"(C:\a;)" + std::wstring(written));
        expect_removes(plan_value_edit(path_change(before, after), live), live, LR"(C:\a)", {written});
    }
    // A root keeps its backslash: "C:" is another entry than "C:\".
    const RawValue root_after = text(LR"(C:\a;D:\)");
    EXPECT_EQ(plan_value_edit(path_change(before, root_after), text(LR"(C:\a;D:)")).basis,
              ValueEditBasis::AlreadyUndone);
}

TEST(ValueEditsTest, TheLiveTypeAndTerminatorAreKept) {
    // setx rewrote the user's Path as REG_SZ with the machine's entries expanded, and added one.
    const RawValue before = text(LR"(%USERPROFILE%\bin)", REG_EXPAND_SZ);
    const RawValue after = text(LR"(%USERPROFILE%\bin;C:\Contoso\bin)", REG_EXPAND_SZ);
    const RawValue rewritten =
        text(LR"(C:\Windows\system32;C:\Windows;C:\Users\me\bin;C:\Contoso\bin;D:\Tools)", REG_SZ);
    const auto plan =
        plan_value_edit(path_change(before, after, kUserEnvironment, ValueSpot::UserEnvironment), rewritten);
    expect_removes(plan, rewritten, LR"(C:\Windows\system32;C:\Windows;C:\Users\me\bin;D:\Tools)",
                   {LR"(C:\Contoso\bin)"});
    EXPECT_EQ(plan.edit->desired->type, static_cast<DWORD>(REG_SZ));
    // A value stored without a terminator stays without one.
    const RawValue unterminated = text(LR"(C:\a;C:\Contoso\bin)", REG_SZ, false);
    expect_removes(
        plan_value_edit(path_change(text(LR"(C:\a)"), text(LR"(C:\a;C:\Contoso\bin)")), unterminated),
        unterminated, LR"(C:\a<no terminator>)", {LR"(C:\Contoso\bin)"});
    // An odd trailing byte stays too.
    RawValue odd = text(LR"(C:\a;C:\Contoso\bin)");
    odd.data.push_back(std::byte{0x7f});
    const auto odd_plan =
        plan_value_edit(path_change(text(LR"(C:\a)"), text(LR"(C:\a;C:\Contoso\bin)")), odd);
    ASSERT_TRUE(odd_plan.edit);
    EXPECT_EQ(odd_plan.edit->desired->data.back(), std::byte{0x7f});
    EXPECT_EQ(odd_plan.edit->desired->data.size(), (4 + 1) * sizeof(wchar_t) + 1);
}

TEST(ValueEditsTest, AnyVariableTheInstallerOnlyAddedEntriesToIsAList) {
    const auto change =
        variable(L"CONTOSO_PLUGINS", text(LR"(C:\a)", REG_SZ), text(LR"(C:\a;C:\Contoso\plugins)", REG_SZ));
    const RawValue live = *change.after;
    const auto plan = plan_value_edit(change, live);
    expect_removes(plan, live, LR"(C:\a)", {LR"(C:\Contoso\plugins)"});
}

TEST(ValueEditsTest, ACreatedVariableIsDeletedOnlyIfUnchanged) {
    const auto change = variable(L"CONTOSO_HOME", std::nullopt, text(LR"(C:\Contoso)", REG_SZ));
    const auto plan = plan_value_edit(change, change.after);
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.basis, ValueEditBasis::Undo);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::DeleteValue);
    EXPECT_EQ(plan.edit->value_name, L"CONTOSO_HOME");
    ASSERT_TRUE(plan.edit->expected);
    EXPECT_EQ(plan.edit->expected->data, change.after->data);
    EXPECT_FALSE(plan.edit->desired);

    EXPECT_EQ(plan_value_edit(change, text(LR"(D:\Elsewhere)", REG_SZ)).basis, ValueEditBasis::ChangedSince);
    EXPECT_EQ(plan_value_edit(change, std::nullopt).basis, ValueEditBasis::AlreadyUndone);
}

TEST(ValueEditsTest, ACreatedListKeepsTheEntriesAddedSince) {
    // The user had no Path of their own; the installer created one.
    const auto change =
        path_change(std::nullopt, text(LR"(C:\Contoso\bin)"), kUserEnvironment, ValueSpot::UserEnvironment);
    const auto unchanged = plan_value_edit(change, change.after);
    ASSERT_TRUE(unchanged.edit);
    EXPECT_EQ(unchanged.edit->kind, ValueEditKind::DeleteValue);
    const RawValue live = text(LR"(C:\Contoso\bin;D:\Tools)");
    expect_removes(plan_value_edit(change, live), live, LR"(D:\Tools)", {LR"(C:\Contoso\bin)"});
    // Only empty entries left: the value goes.
    const auto emptied = plan_value_edit(change, text(LR"(C:\Contoso\bin;)"));
    ASSERT_TRUE(emptied.edit);
    EXPECT_EQ(emptied.edit->kind, ValueEditKind::DeleteValue);
    // The machine's Path is never deleted, whoever created it.
    const auto machine =
        plan_value_edit(path_change(std::nullopt, text(LR"(C:\Contoso\bin)")), text(LR"(C:\Contoso\bin)"));
    EXPECT_FALSE(machine.edit);
    EXPECT_EQ(machine.basis, ValueEditBasis::ProtectedVariable);
}

TEST(ValueEditsTest, AnotherChangedVariableIsRestoredAsQuestionable) {
    const auto change = variable(L"CONTOSO_THEME", text(L"dark", REG_SZ), text(L"light", REG_SZ));
    const auto plan = plan_value_edit(change, change.after);
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.basis, ValueEditBasis::Restore);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::RestoreValue);
    EXPECT_EQ(plan.edit->expected->data, change.after->data);
    EXPECT_EQ(plan.edit->desired->data, change.before->data);
    EXPECT_EQ(plan_value_edit(change, text(L"blue", REG_SZ)).basis, ValueEditBasis::ChangedSince);
    EXPECT_EQ(plan_value_edit(change, change.before).basis, ValueEditBasis::AlreadyUndone);

    // A variable the installer deleted comes back.
    const auto deleted = variable(L"CONTOSO_OLD", text(L"1", REG_SZ), std::nullopt);
    const auto recreated = plan_value_edit(deleted, std::nullopt);
    ASSERT_TRUE(recreated.edit);
    EXPECT_EQ(recreated.basis, ValueEditBasis::Restore);
    EXPECT_FALSE(recreated.edit->expected);
    ASSERT_TRUE(recreated.edit->desired);
    EXPECT_EQ(recreated.edit->desired->data, deleted.before->data);
    EXPECT_EQ(recreated.edit->desired->name, L"CONTOSO_OLD");
}

TEST(ValueEditsTest, VariablesWindowsReliesOnAreLeftAlone) {
    for (const auto* name :
         {L"ComSpec", L"windir", L"TEMP", L"tmp", L"PROCESSOR_ARCHITECTURE", L"ProgramFiles"}) {
        const auto change = variable(name, text(L"C:\\old", REG_SZ), text(L"C:\\new", REG_SZ));
        const auto plan = plan_value_edit(change, change.after);
        EXPECT_FALSE(plan.edit) << platform::to_utf8(name);
        EXPECT_EQ(plan.basis, ValueEditBasis::ProtectedVariable) << platform::to_utf8(name);
    }
    // Windows' lists only lose entries; what an installer took out of them stays out.
    const auto pathext =
        variable(L"PATHEXT", text(L".COM;.EXE;.BAT", REG_SZ), text(L".COM;.EXE;.BAT;.PY", REG_SZ));
    expect_removes(plan_value_edit(pathext, pathext.after), *pathext.after, L".COM;.EXE;.BAT", {L".PY"});
    const auto removed = variable(L"PATHEXT", text(L".COM;.EXE;.BAT", REG_SZ), text(L".COM;.EXE", REG_SZ));
    EXPECT_EQ(plan_value_edit(removed, removed.after).basis, ValueEditBasis::Unsupported);
    // An upgrade replaced its old entry: the new one goes, the old one stays out.
    const auto upgrade = path_change(text(LR"(C:\a;C:\Contoso\v1\bin)"), text(LR"(C:\a;C:\Contoso\v2\bin)"));
    expect_removes(plan_value_edit(upgrade, upgrade.after), *upgrade.after, LR"(C:\a)",
                   {LR"(C:\Contoso\v2\bin)"});
    // A list replaced whole is never left empty.
    const auto rewritten = variable(L"PSModulePath", text(L"C:\\a", REG_SZ), text(L"C:\\b", REG_SZ));
    EXPECT_EQ(plan_value_edit(rewritten, rewritten.after).basis, ValueEditBasis::Unsupported);
}

TEST(ValueEditsTest, SharedDllCountsGoDownByTheInstallersIncrement) {
    // Raised from 1 to 2: back to 1, also after another program raised it since.
    auto plan = plan_value_edit(shared_dll(count(1), count(2)), count(2));
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.basis, ValueEditBasis::Undo);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::DecrementSharedDll);
    EXPECT_EQ(shared_dll_count(*plan.edit->expected), 2u);
    EXPECT_EQ(shared_dll_count(*plan.edit->desired), 1u);
    EXPECT_EQ(plan.edit->delta, 1);
    EXPECT_EQ(plan.edit->desired->name, L"C:\\Shared\\contoso.dll");
    plan = plan_value_edit(shared_dll(count(1), count(3), kSharedDlls32), count(5));
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(shared_dll_count(*plan.edit->desired), 3u);
    EXPECT_EQ(plan.edit->delta, 2);

    // A count the installer created goes at 0.
    plan = plan_value_edit(shared_dll(std::nullopt, count(1)), count(1));
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::DeleteValue);
    EXPECT_FALSE(plan.edit->desired);
    EXPECT_EQ(plan.edit->delta, 1);
    plan = plan_value_edit(shared_dll(std::nullopt, count(1)), count(2));
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::DecrementSharedDll);
    EXPECT_EQ(shared_dll_count(*plan.edit->desired), 1u);

    // One that existed before stays, at 0 at the lowest.
    plan = plan_value_edit(shared_dll(count(1), count(2)), count(1));
    ASSERT_TRUE(plan.edit);
    EXPECT_EQ(plan.edit->kind, ValueEditKind::DecrementSharedDll);
    EXPECT_EQ(shared_dll_count(*plan.edit->desired), 0u);
    EXPECT_EQ(plan.edit->delta, 1);
    EXPECT_EQ(plan_value_edit(shared_dll(count(1), count(2)), count(0)).basis, ValueEditBasis::AlreadyUndone);

    // Nothing to take back.
    EXPECT_EQ(plan_value_edit(shared_dll(count(2), count(1)), count(1)).basis, ValueEditBasis::Unsupported);
    EXPECT_EQ(plan_value_edit(shared_dll(count(1), std::nullopt), std::nullopt).basis,
              ValueEditBasis::Unsupported);
    EXPECT_EQ(plan_value_edit(shared_dll(count(1), text(L"2", REG_SZ)), count(2)).basis,
              ValueEditBasis::Unsupported);
    EXPECT_EQ(plan_value_edit(shared_dll(count(1), count(2)), std::nullopt).basis,
              ValueEditBasis::AlreadyUndone);
    EXPECT_EQ(plan_value_edit(shared_dll(count(1), count(2)), text(L"2", REG_SZ)).basis,
              ValueEditBasis::ChangedSince);
}

TEST(ValueEditsTest, OnlyTheEnvironmentAndSharedDllsAreEditableSpots) {
    EXPECT_EQ(value_spot(kMachineEnvironment), ValueSpot::MachineEnvironment);
    EXPECT_EQ(
        value_spot({Hive::Machine, L"", LR"(system\currentcontrolset\control\session manager\environment)"}),
        ValueSpot::MachineEnvironment);
    EXPECT_EQ(value_spot(kUserEnvironment), ValueSpot::UserEnvironment);
    EXPECT_EQ(value_spot(kSharedDlls), ValueSpot::SharedDlls);
    EXPECT_EQ(value_spot(kSharedDlls32), ValueSpot::SharedDlls);
    EXPECT_FALSE(
        value_spot({Hive::Machine, L"", LR"(SYSTEM\ControlSet001\Control\Session Manager\Environment)"}));
    EXPECT_FALSE(value_spot({Hive::User, L"S-1-5-21-1-2-3-1001_Classes", L"Environment"}));
    EXPECT_FALSE(value_spot({Hive::User, L"S-1-5-21-1-2-3-1001", L"Volatile Environment"}));
    EXPECT_FALSE(value_spot({Hive::Machine, L"", L"Environment"}));
    EXPECT_FALSE(value_spot({Hive::Machine, L"", LR"(SOFTWARE\Contoso)"}));
}

TEST(ValueEditsTest, UndoPutsEntriesBackWhereTheyWere) {
    const RawValue original = text(LR"(C:\a;C:\Contoso\bin;C:\Contoso\tools;C:\b)");
    const RawValue edited = text(LR"(C:\a;C:\b)");
    const auto back = [&](const RawValue& live,
                          const RawValue& from = text(LR"(C:\a;C:\Contoso\bin;C:\Contoso\tools;C:\b)"),
                          const RawValue& to = text(LR"(C:\a;C:\b)")) {
        const auto value = reinsert_path_entries(live, from, to);
        EXPECT_TRUE(value);
        return value ? text_of(*value) : std::wstring();
    };
    // After the entry before them, in their order, wherever the user moved things.
    EXPECT_EQ(back(text(LR"(D:\x;C:\a;C:\b;D:\y)")),
              LR"(D:\x;C:\a;C:\Contoso\bin;C:\Contoso\tools;C:\b;D:\y)");
    // Their neighbour is gone: last, before a trailing ';'.
    EXPECT_EQ(back(text(LR"(D:\x;)")), LR"(D:\x;C:\Contoso\bin;C:\Contoso\tools;)");
    // First when nothing preceded them.
    EXPECT_EQ(back(text(LR"(C:\a;D:\y)"), text(LR"(C:\Contoso\bin;C:\a)"), text(LR"(C:\a)")),
              LR"(C:\Contoso\bin;C:\a;D:\y)");
    // Empty entries aren't put back.
    EXPECT_EQ(back(text(LR"(C:\a;D:\y)"), text(LR"(C:\a;C:\Contoso\bin;)"), text(LR"(C:\a)")),
              LR"(C:\a;C:\Contoso\bin;D:\y)");
    // An entry that is back already isn't added twice; the live type stays.
    const RawValue live = text(LR"(C:\a;c:\contoso\bin\;C:\b)", REG_SZ);
    const auto partly = reinsert_path_entries(live, original, edited);
    ASSERT_TRUE(partly);
    EXPECT_EQ(partly->type, static_cast<DWORD>(REG_SZ));
    EXPECT_EQ(text_of(*partly), LR"(C:\a;c:\contoso\bin\;C:\Contoso\tools;C:\b)");
    const RawValue complete = text(LR"(C:\a;C:\Contoso\bin;C:\Contoso\tools;C:\b)");
    EXPECT_EQ(reinsert_path_entries(complete, original, edited)->data, complete.data);
    // Not a string, or not an edit of the original.
    EXPECT_FALSE(reinsert_path_entries(count(1), original, edited));
    EXPECT_FALSE(reinsert_path_entries(live, original, text(LR"(C:\z)")));
}

}  // namespace
}  // namespace avel::leftovers
