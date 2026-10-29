// Linking traces to programs, on made-up traces and inventories: the candidates a trace offers,
// the order entries are matched in and which link is primary, finding the program again after an
// upgrade, links set by hand, the traces of a program, and linking a trace in a store.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/inventory/msi_codes.h"
#include "core/tracker/linking.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using inventory::Program;
using inventory::RegistryView;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOtherSid[] = L"S-1-5-21-1-2-3-1002";
constexpr wchar_t kUninstall[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr wchar_t kUninstall32[] = LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr wchar_t kUserUninstall[] = LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr wchar_t kProduct[] = L"{23170F69-40C1-2702-2600-000001000000}";
constexpr wchar_t kOtherProduct[] = L"{11111111-2222-3333-4444-555555555555}";

RegistryAddress machine(std::wstring path) { return {Hive::Machine, L"", std::move(path)}; }

RegistryAddress user(const wchar_t* sid, std::wstring path) { return {Hive::User, sid, std::move(path)}; }

TraceChange created_key(RegistryAddress key, NoiseReason noise = NoiseReason::None) {
    TraceChange change;
    change.location = leftovers::RegistryKeyLocation{std::move(key)};
    change.noise = noise;
    return change;
}

TraceChange value_change(ChangeKind kind, RegistryAddress key, std::wstring name) {
    TraceChange change;
    change.change = kind;
    change.location = leftovers::RegistryValueLocation{std::move(key), std::move(name)};
    return change;
}

Program entry(std::wstring name, RegistryView view, std::wstring key_name, std::wstring sid = {}) {
    Program program;
    program.display_name = std::move(name);
    program.publisher = L"Contoso";
    program.display_version = L"1.0";
    program.registry = inventory::RegistryLocation{view, key_name, {}};
    if (view == RegistryView::User || view == RegistryView::User32) {
        program.scope = inventory::Scope::User;
        program.user_sid = sid;
        program.id = L"reg:HKU\\" + sid + L":" + key_name;
    } else {
        program.id =
            std::wstring(view == RegistryView::Machine64 ? L"reg:HKLM64:" : L"reg:HKLM32:") + key_name;
    }
    return program;
}

std::vector<std::wstring> ids_of(const std::vector<TraceLink>& links) {
    std::vector<std::wstring> ids;
    for (const auto& link : links) {
        ids.push_back(link.program_id);
    }
    return ids;
}

std::vector<std::wstring> paths_of(const std::vector<RegistryAddress>& keys) {
    std::vector<std::wstring> paths;
    for (const auto& key : keys) {
        paths.push_back(platform::display_path(key));
    }
    return paths;
}

TEST(LinkingTest, CandidatesAreCreatedAndChangedUninstallKeysAndProductCodes) {
    const std::wstring packed = *inventory::pack_guid(kProduct);
    TraceResult result;
    result.changes = {
        // Created entries, 64-bit, 32-bit and a user's; a user's whole Uninstall key.
        created_key(machine(std::wstring(kUninstall) + L"\\Contoso")),
        created_key(machine(std::wstring(kUninstall32) + L"\\" + kProduct)),
        created_key(user(kSid, std::wstring(kUserUninstall) + L"\\Tool")),
        created_key(user(kOtherSid, LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)")),
        // Changed entries: a value, a key inside one.
        value_change(ChangeKind::Modified, machine(std::wstring(kUninstall) + L"\\Fabrikam"),
                     L"DisplayVersion"),
        created_key(machine(std::wstring(kUninstall) + LR"(\Northwind\Details)")),
        // Windows Installer's own registration.
        created_key(machine(LR"(SOFTWARE\Classes\Installer\Products\)" + packed)),
        value_change(
            ChangeKind::Created,
            machine(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Products\)" +
                    *inventory::pack_guid(kOtherProduct) + LR"(\InstallProperties)"),
            L"DisplayName"),
        // Not candidates: noise, a deleted entry, a key elsewhere, the Uninstall key's own value.
        created_key(machine(std::wstring(kUninstall) + L"\\Noise"), NoiseReason::SystemActivity),
        value_change(ChangeKind::Modified, machine(kUninstall), L"Stray"),
        created_key(machine(LR"(SOFTWARE\Contoso)")),
    };
    TraceChange deleted = created_key(machine(std::wstring(kUninstall) + L"\\Old"));
    deleted.change = ChangeKind::Deleted;
    result.changes.push_back(deleted);

    const auto candidates = link_candidates(result);
    EXPECT_EQ(paths_of(candidates.created_keys),
              (std::vector<std::wstring>{std::wstring(L"HKLM\\") + kUninstall + L"\\Contoso",
                                         std::wstring(L"HKLM\\") + kUninstall32 + L"\\" + kProduct,
                                         std::wstring(L"HKU\\") + kSid + L"\\" + kUserUninstall + L"\\Tool",
                                         std::wstring(L"HKU\\") + kOtherSid + L"\\" + kUserUninstall}));
    EXPECT_EQ(paths_of(candidates.changed_keys),
              (std::vector<std::wstring>{std::wstring(L"HKLM\\") + kUninstall + L"\\Fabrikam",
                                         std::wstring(L"HKLM\\") + kUninstall + L"\\Northwind"}));
    EXPECT_EQ(candidates.product_codes, (std::vector<std::wstring>{kProduct, kOtherProduct}));
}

TEST(LinkingTest, UninstallKeysAreSpelledAsTheTraceReportsThem) {
    EXPECT_EQ(platform::display_path(*uninstall_key_of(entry(L"A", RegistryView::Machine64, L"A"))),
              std::wstring(L"HKLM\\") + kUninstall + L"\\A");
    EXPECT_EQ(platform::display_path(*uninstall_key_of(entry(L"B", RegistryView::Machine32, L"B"))),
              std::wstring(L"HKLM\\") + kUninstall32 + L"\\B");
    EXPECT_EQ(platform::display_path(*uninstall_key_of(entry(L"C", RegistryView::User, L"C", kSid))),
              std::wstring(L"HKU\\") + kSid + L"\\" + kUserUninstall + L"\\C");
    Program packaged;
    packaged.source = inventory::Source::Appx;
    EXPECT_FALSE(uninstall_key_of(packaged));
}

// Entries are matched by a created key (or one inside a created key), then a changed key, then a
// product code; the primary link is an entry Settings lists.
TEST(LinkingTest, MatchesInOrderAndPrefersTheEntrySettingsLists) {
    inventory::Inventory inventory;
    Program hidden = entry(L"Contoso Core", RegistryView::Machine64, L"ContosoCore");
    hidden.system_component = true;  // Not in Settings
    Program bundle = entry(L"Contoso Suite", RegistryView::Machine32, kProduct);
    bundle.windows_installer = true;  // Its product code comes from the key's name
    Program upgraded = entry(L"Fabrikam Tool", RegistryView::Machine64, L"Fabrikam");
    Program per_user = entry(L"Contoso Helper", RegistryView::User, L"Helper", kOtherSid);
    Program msi_only;
    msi_only.id = std::wstring(L"msi:") + kOtherProduct;
    msi_only.source = inventory::Source::Msi;
    msi_only.display_name = L"Contoso Runtime";
    msi_only.msi_product_code = kOtherProduct;
    Program unrelated = entry(L"Other", RegistryView::Machine64, L"Other");
    inventory.programs = {hidden, msi_only, upgraded, unrelated, per_user, bundle};

    TraceResult result;
    result.changes = {
        created_key(machine(std::wstring(kUninstall) + L"\\ContosoCore")),
        created_key(machine(std::wstring(kUninstall32) + L"\\" + kProduct)),
        created_key(user(kOtherSid, kUserUninstall)),
        value_change(ChangeKind::Modified, machine(std::wstring(kUninstall) + L"\\Fabrikam"),
                     L"DisplayVersion"),
        created_key(
            machine(LR"(SOFTWARE\Classes\Installer\Products\)" + *inventory::pack_guid(kOtherProduct))),
    };
    const auto links = find_links(link_candidates(result), inventory);
    // Listed in Settings first (by how they were found, then the inventory's order), then the rest.
    EXPECT_EQ(ids_of(links),
              (std::vector<std::wstring>{per_user.id, bundle.id, upgraded.id, hidden.id, msi_only.id}));
    ASSERT_EQ(links.size(), 5u);
    EXPECT_TRUE(links[0].primary);
    EXPECT_FALSE(links[1].primary);
    EXPECT_EQ(links[0].matched_by, LinkMatch::CreatedKey);
    EXPECT_EQ(links[1].matched_by, LinkMatch::CreatedKey);
    EXPECT_EQ(links[1].product_code, kProduct);
    EXPECT_EQ(links[2].matched_by, LinkMatch::ChangedKey);
    EXPECT_EQ(links[3].matched_by, LinkMatch::CreatedKey);
    EXPECT_EQ(links[4].matched_by, LinkMatch::ProductCode);
    EXPECT_FALSE(links[4].uninstall_key);
    EXPECT_EQ(links[0].uninstall_key->sid, kOtherSid);

    // A created key matches before the same entry's product code; nothing at all, no links.
    EXPECT_TRUE(find_links(link_candidates({}), inventory).empty());
}

// Sandbox run 1: Windows installed its compatibility fix database (an Uninstall entry Settings
// lists, whose uninstaller is sdbinst) while Notepad++ installed, and the fix database was primary.
// The entry whose folder, uninstaller or icon is in a folder the installer created is primary.
TEST(LinkingTest, PrefersTheEntryWhoseFolderTheInstallerCreated) {
    Program fix_database = entry(L"Microsoft Windows Application Compatibility Fix Database",
                                 RegistryView::Machine64, L"{22221111-1111-1111-1111-111111111111}.sdb");
    fix_database.uninstall_string =
        LR"(C:\Windows\system32\sdbinst.exe -u "C:\Windows\AppPatch\CustomSDB\{22221111-1111-1111-1111-111111111111}.sdb")";
    // Notepad++'s entry records no folder: its uninstaller and its icon are in it.
    Program notepad = entry(L"Notepad++ (64-bit x64)", RegistryView::Machine64, L"Notepad++");
    Program by_folder = notepad;
    by_folder.install_location = LR"(C:\Program Files\Notepad++\)";
    Program by_uninstaller = notepad;
    by_uninstaller.uninstall_string = LR"("C:\Program Files\Notepad++\uninstall.exe")";
    Program by_icon = notepad;
    by_icon.display_icon = LR"(C:\PROGRAM FILES\Notepad++\notepad++.exe,0)";

    TraceResult result;
    result.changes = {
        created_key(machine(std::wstring(kUninstall) + L"\\{22221111-1111-1111-1111-111111111111}.sdb")),
        created_key(machine(std::wstring(kUninstall) + L"\\Notepad++")),
    };
    TraceChange folder;
    folder.location = leftovers::FileLocation{LR"(C:\Program Files\Notepad++)", true};
    result.changes.push_back(folder);
    const auto candidates = link_candidates(result);
    EXPECT_EQ(candidates.created_folders, std::vector<std::wstring>{LR"(C:\Program Files\Notepad++)"});
    inventory::Inventory inventory;
    for (const auto& program : {by_folder, by_uninstaller, by_icon}) {
        inventory.programs = {fix_database, program};
        EXPECT_EQ(ids_of(find_links(candidates, inventory)),
                  (std::vector<std::wstring>{notepad.id, fix_database.id}));
    }
    inventory.programs = {fix_database, notepad};  // Nothing tells where it is: the inventory's order
    EXPECT_EQ(ids_of(find_links(candidates, inventory)),
              (std::vector<std::wstring>{fix_database.id, notepad.id}));

    // A folder that is noise, or only modified, lays nothing down.
    inventory.programs = {fix_database, by_folder};
    result.changes.back().noise = NoiseReason::SystemActivity;
    EXPECT_EQ(ids_of(find_links(link_candidates(result), inventory)),
              (std::vector<std::wstring>{fix_database.id, notepad.id}));
    result.changes.back().noise = NoiseReason::None;
    result.changes.back().change = ChangeKind::Modified;
    EXPECT_TRUE(link_candidates(result).created_folders.empty());
}

TEST(LinkingTest, ALinkKeepsWhatFindsTheProgramAgain) {
    Program program = entry(L"Contoso App 1.0", RegistryView::Machine64, kProduct);
    program.publisher = L"Contoso Ltd";
    program.windows_installer = true;
    program.install_location = LR"(C:\Program Files\Contoso\)";
    const TraceLink link = make_link(program, LinkMatch::CreatedKey);
    EXPECT_EQ(link.program_id, program.id);
    EXPECT_EQ(link.name, L"Contoso App 1.0");
    EXPECT_EQ(link.publisher, L"Contoso Ltd");
    EXPECT_EQ(link.version, L"1.0");
    EXPECT_EQ(link.product_code, kProduct);
    EXPECT_EQ(link.install_location, LR"(C:\Program Files\Contoso)");
    ASSERT_TRUE(link.uninstall_key);
    EXPECT_EQ(link.uninstall_key->path, std::wstring(kUninstall) + L"\\" + kProduct);
}

// After an upgrade the program is found by its key (same key, new version), its product code (a
// new key), or carefully by its name, user and publisher or folder (a new key and product code).
TEST(LinkingTest, ResolvesTheProgramAgainAfterAnUpgrade) {
    Program old_version = entry(L"Contoso App 1.0", RegistryView::Machine64, kProduct);
    old_version.msi_product_code = kProduct;
    old_version.install_location = LR"(C:\Program Files\Contoso)";
    TraceInfo info;
    info.links = {make_link(old_version, LinkMatch::CreatedKey)};
    info.links[0].primary = true;

    // The same key, another version and id.
    Program same_key = old_version;
    same_key.id = L"reg:HKLM64:renamed-id";
    same_key.display_name = L"Contoso App 1.1";
    same_key.display_version = L"1.1";
    inventory::Inventory inventory;
    inventory.programs = {entry(L"Other", RegistryView::Machine64, L"Other"), same_key};
    EXPECT_EQ(resolve_link(info.links[0], inventory), &inventory.programs[1]);

    // Another key, the same product code.
    Program same_code = entry(L"Contoso App 1.2", RegistryView::Machine32, L"Contoso");
    same_code.msi_product_code = kProduct;
    inventory.programs = {same_code};
    EXPECT_EQ(resolve_link(info.links[0], inventory), &inventory.programs[0]);

    // A major upgrade: a new key and product code; the name without its version, the publisher.
    Program upgraded = entry(L"Contoso App 2.0", RegistryView::Machine64, kOtherProduct);
    upgraded.msi_product_code = kOtherProduct;
    upgraded.display_version = L"2.0";
    upgraded.install_location = LR"(C:\Program Files\Contoso)";
    Program other_user = entry(L"Contoso App 2.0", RegistryView::User, L"Contoso", kSid);
    Program other_publisher = entry(L"Contoso App 2.0", RegistryView::Machine64, L"Clone");
    other_publisher.publisher = L"Someone Else";
    inventory.programs = {other_user, other_publisher, upgraded};
    EXPECT_EQ(resolve_link(info.links[0], inventory), &inventory.programs[2]);
    ASSERT_TRUE(refresh_links(info, inventory));
    EXPECT_EQ(info.links[0].program_id, upgraded.id);
    EXPECT_EQ(info.links[0].version, L"2.0");
    EXPECT_EQ(info.links[0].product_code, kOtherProduct);
    EXPECT_EQ(info.links[0].matched_by, LinkMatch::CreatedKey);  // How it was found stays
    EXPECT_TRUE(info.links[0].primary);

    // Careful: two candidates Settings both lists are no answer; a hidden one doesn't count.
    TraceLink fresh = make_link(old_version, LinkMatch::CreatedKey);
    Program twin = upgraded;
    twin.id = L"reg:HKLM32:Twin";
    twin.registry->view = RegistryView::Machine32;
    twin.registry->key_name = L"Twin";
    twin.msi_product_code.clear();
    inventory.programs = {upgraded, twin};
    EXPECT_EQ(resolve_link(fresh, inventory), nullptr);
    inventory.programs[1].system_component = true;
    EXPECT_EQ(resolve_link(fresh, inventory), &inventory.programs[0]);

    // Uninstalled: nothing leads anywhere.
    inventory.programs = {entry(L"Other", RegistryView::Machine64, L"Other")};
    EXPECT_FALSE(refresh_links(info, inventory));
    EXPECT_FALSE(resolve_trace_program(info, inventory));
}

TEST(LinkingTest, ResolvesThePrimaryLinkFirst) {
    Program first = entry(L"Contoso Runtime", RegistryView::Machine64, L"Runtime");
    Program main = entry(L"Contoso App", RegistryView::Machine64, L"App");
    TraceInfo info;
    info.links = {make_link(first, LinkMatch::CreatedKey), make_link(main, LinkMatch::CreatedKey)};
    info.links[1].primary = true;
    inventory::Inventory inventory;
    inventory.programs = {first, main};
    auto resolved = resolve_trace_program(info, inventory);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->link, 1u);
    EXPECT_EQ(resolved->program, &inventory.programs[1]);
    inventory.programs = {first};  // The primary is gone: another link still leads somewhere
    resolved = resolve_trace_program(info, inventory);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->link, 0u);
}

TEST(LinkingTest, ALinkSetByHandIsPrimaryAndStays) {
    Program found = entry(L"Contoso App", RegistryView::Machine64, L"Contoso");
    Program chosen = entry(L"Fabrikam Tool", RegistryView::Machine64, L"Fabrikam");
    inventory::Inventory inventory;
    inventory.programs = {found, chosen};
    TraceResult result;
    result.changes = {created_key(machine(std::wstring(kUninstall) + L"\\Contoso"))};

    TraceInfo info;
    info.unresolved_since = std::chrono::system_clock::now();
    set_links(info, result, inventory);
    EXPECT_EQ(ids_of(info.links), (std::vector<std::wstring>{found.id}));
    EXPECT_FALSE(info.unresolved_since);

    add_manual_link(info, chosen);
    EXPECT_EQ(ids_of(info.links), (std::vector<std::wstring>{chosen.id, found.id}));
    EXPECT_EQ(info.links[0].matched_by, LinkMatch::Manual);
    EXPECT_TRUE(info.links[0].primary);
    EXPECT_FALSE(info.links[1].primary);

    // Linking automatically again keeps it first.
    set_links(info, result, inventory);
    EXPECT_EQ(ids_of(info.links), (std::vector<std::wstring>{chosen.id, found.id}));
    EXPECT_TRUE(info.links[0].primary);

    // Linking by hand to a program already linked replaces that link.
    add_manual_link(info, found);
    EXPECT_EQ(ids_of(info.links), (std::vector<std::wstring>{found.id, chosen.id}));
    EXPECT_EQ(info.links[0].matched_by, LinkMatch::Manual);
}

TEST(LinkingTest, RelinkKeepsLinksAndAddsWhatTheChangesName) {
    Program here = entry(L"Contoso App", RegistryView::Machine64, L"Contoso");
    Program also = entry(L"Contoso Helper", RegistryView::Machine64, L"Helper");
    TraceInfo info;
    TraceLink elsewhere;
    elsewhere.program_id = L"reg:HKLM64:Gone";
    elsewhere.name = L"Gone";
    elsewhere.primary = true;
    info.links = {elsewhere, make_link(here, LinkMatch::CreatedKey)};
    info.links[1].version = L"0.9";
    info.unresolved_since = std::chrono::system_clock::now();
    TraceResult result;
    result.changes = {created_key(machine(std::wstring(kUninstall) + L"\\Contoso")),
                      created_key(machine(std::wstring(kUninstall) + L"\\Helper"))};
    inventory::Inventory inventory;
    inventory.programs = {here, also};
    relink(info, result, inventory);
    EXPECT_EQ(ids_of(info.links), (std::vector<std::wstring>{elsewhere.program_id, here.id, also.id}));
    EXPECT_TRUE(info.links[0].primary);
    EXPECT_EQ(info.links[1].version, L"1.0");
    EXPECT_FALSE(info.links[2].primary);
    EXPECT_FALSE(info.unresolved_since);
}

TEST(LinkingTest, FindsTheTracesOfAProgramNewestFirst) {
    Program program = entry(L"Contoso App", RegistryView::Machine64, L"Contoso");
    inventory::Inventory inventory;
    inventory.programs = {program, entry(L"Other", RegistryView::Machine64, L"Other")};
    const auto trace = [&](std::wstring id, TraceState state, std::chrono::hours age, const Program& linked) {
        TraceInfo info;
        info.id = std::move(id);
        info.state = state;
        info.started = std::chrono::sys_days{std::chrono::year{2026} / 10 / 24} - age;
        info.links = {make_link(linked, LinkMatch::CreatedKey)};
        return info;
    };
    const std::vector<TraceInfo> traces{
        trace(L"20261023-000000-00000001", TraceState::Complete, 24h, program),
        trace(L"20261023-000000-00000002", TraceState::Open, 1h, program),  // Not finished
        trace(L"20261023-000000-00000003", TraceState::Incomplete, 2h, program),
        trace(L"20261023-000000-00000004", TraceState::Complete, 3h, inventory.programs[1]),
    };
    EXPECT_EQ(traces_of(program, traces, inventory),
              (std::vector<std::wstring>{L"20261023-000000-00000003", L"20261023-000000-00000001"}));
}

// In a store: link_trace writes the links to trace.json; an open trace can't be linked.
TEST(LinkingTest, LinksATraceInTheStore) {
    fakes::TempFolder temp;
    platform::FolderStatus status{};
    auto store = TraceStore::open(fakes::test_store_options(temp / L"Traces"), status);
    ASSERT_TRUE(store);
    TraceInfo info;
    TraceError error{};
    auto folder = store->create(info, error);
    ASSERT_TRUE(folder);
    info.state = TraceState::Open;
    ASSERT_TRUE(folder->write_info(info));
    folder->release();

    Program program = entry(L"Contoso App", RegistryView::Machine64, L"Contoso");
    inventory::Inventory inventory;
    inventory.programs = {program};
    TraceResult result;
    result.changes = {created_key(machine(std::wstring(kUninstall) + L"\\Contoso"))};
    EXPECT_FALSE(link_trace(*store, info.id, result, inventory, error));
    EXPECT_EQ(error, TraceError::NotFinished);

    info.state = TraceState::Complete;
    ASSERT_TRUE(folder->write_info(info));
    const auto linked = link_trace(*store, info.id, result, inventory, error);
    ASSERT_TRUE(linked);
    EXPECT_EQ(error, TraceError::None);
    const auto read = store->read(info.id);
    ASSERT_TRUE(read);
    ASSERT_EQ(read->links.size(), 1u);
    EXPECT_EQ(read->links[0].program_id, program.id);
    EXPECT_TRUE(read->links[0].primary);
    EXPECT_EQ(traces_of(program, store->list(), inventory), (std::vector<std::wstring>{info.id}));
}

}  // namespace
}  // namespace avel::tracker
