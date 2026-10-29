// Tying a trace's items to its program, on a made-up trace: each rule (the entry's own folders,
// Uninstall key and Windows Installer keys, created roots named after the program or its
// publisher), items inside tied ones, references by path and by ProgID or CLSID name over three
// rounds, and what stays untied. The strings items hold come from a table, not the registry.
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "core/inventory/msi_codes.h"
#include "core/leftovers/product_name.h"
#include "core/platform/text.h"
#include "core/tracker/trace_items.h"

namespace avel::tracker {
namespace {

using leftovers::FileLocation;
using leftovers::Location;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kProduct[] = L"{23170F69-40C1-2702-2600-000001000000}";
constexpr wchar_t kInstall[] = LR"(C:\Program Files\Widget Studio)";
constexpr wchar_t kClsid[] = L"{C0FFEE00-1111-2222-3333-444455556666}";

RegistryAddress machine(std::wstring path) { return {Hive::Machine, L"", std::move(path)}; }

TraceChange created(Location location, ChangeKind kind = ChangeKind::Created) {
    TraceChange change;
    change.change = kind;
    change.location = std::move(location);
    return change;
}

TraceChange folder(std::wstring path) { return created(FileLocation{std::move(path), true}); }
TraceChange file(std::wstring path) { return created(FileLocation{std::move(path), false}); }
TraceChange key(std::wstring path) { return created(RegistryKeyLocation{machine(std::move(path))}); }
TraceChange value(std::wstring path, std::wstring name, ChangeKind kind = ChangeKind::Created) {
    return created(RegistryValueLocation{machine(std::move(path)), std::move(name)}, kind);
}

ProgramPlaces widget_studio() {
    ProgramPlaces places;
    auto& target = places.target;
    target.program_id = L"reg:HKLM64:" + std::wstring(kProduct);
    target.display_name = L"Widget Studio 2.1";
    target.names = leftovers::make_product_names(L"Widget Studio 2.1", L"Fabrikam");
    target.install_location = kInstall;
    target.uninstaller = LR"(C:\ProgramData\Package Cache\{ABC}\setup.exe)";
    target.uninstaller_folder = LR"(C:\ProgramData\Package Cache\{ABC})";
    target.uninstall_key =
        machine(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\)" + std::wstring(kProduct));
    target.msi_product_code = kProduct;
    places.icon_folder = LR"(C:\Widget Icons)";
    return places;
}

// The strings each item holds, by its identity.
class FakeStrings {
public:
    void set(const TraceChange& change, std::vector<std::wstring> strings) {
        strings_[leftovers::identity(change.location)] = std::move(strings);
    }

    TieSources sources() const {
        TieSources sources;
        sources.strings = [this](const Location& location) {
            const auto found = strings_.find(leftovers::identity(location));
            return found != strings_.end() ? found->second : std::vector<std::wstring>();
        };
        sources.expand = [](std::wstring_view text) { return std::wstring(text); };
        sources.file_exists = [](const std::wstring&) { return false; };
        sources.broad_folders = {LR"(C:\Program Files)", LR"(C:\ProgramData)", LR"(C:\Users\Test)"};
        return sources;
    }

private:
    std::map<std::wstring, std::vector<std::wstring>> strings_;
};

struct Expected {
    bool tied;
    TieReason reason;
    int round;
};

void expect_tie(const std::vector<TiedItem>& items, std::size_t change, Expected expected,
                std::optional<std::size_t> via = std::nullopt) {
    const auto found =
        std::find_if(items.begin(), items.end(), [&](const TiedItem& item) { return item.change == change; });
    ASSERT_NE(found, items.end()) << "change " << change;
    EXPECT_EQ(found->tied, expected.tied) << "change " << change;
    EXPECT_EQ(found->reason, expected.reason) << "change " << change << ": " << to_string(found->reason);
    EXPECT_EQ(found->round, expected.round) << "change " << change;
    if (expected.reason == TieReason::InsideTied || expected.reason == TieReason::ReferencesTied) {
        EXPECT_EQ(found->via, via) << "change " << change;
    }
}

TEST(TraceItemsTest, TheEntrysOwnLocationsAreTied) {
    const std::wstring packed = *inventory::pack_guid(kProduct);
    TraceResult result;
    result.changes = {
        folder(kInstall),                                                                          // 0
        folder(LR"(C:\ProgramData\Package Cache\{ABC})"),                                          // 1
        file(LR"(C:\Widget Icons\app.ico)"),                                                       // 2
        key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\)" + std::wstring(kProduct)),  // 3
        key(LR"(SOFTWARE\Classes\Installer\Products\)" + packed),                                  // 4
        key(LR"(SOFTWARE\Classes\Installer\Features\)" + packed),                                  // 5
        key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Products\)" +
            packed),                                                                                    // 6
        value(LR"(SOFTWARE\Classes\Installer\UpgradeCodes\0123456789ABCDEF0123456789ABCDEF)", packed),  // 7
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Components\ABCDEF)",
              packed),                                                                 // 8
        key(LR"(SOFTWARE\Classes\Installer\Dependencies\)" + std::wstring(kProduct)),  // 9
        // An upgrade: the install folder and Uninstall key existed; what changed in them is tied.
        file(std::wstring(kInstall) + LR"(\new.dll)"),  // 10
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\)" + std::wstring(kProduct),
              L"DisplayVersion",
              ChangeKind::Modified),  // 11
        // Another product's registration isn't.
        key(LR"(SOFTWARE\Classes\Installer\Products\)" + std::wstring(32, L'0')),  // 12
        value(LR"(SOFTWARE\Classes\Installer\UpgradeCodes\0123456789ABCDEF0123456789ABCDEF)",
              std::wstring(32, L'0')),  // 13
    };
    const auto items = tie_trace_items(result, widget_studio(), FakeStrings().sources());
    ASSERT_EQ(items.size(), result.changes.size());
    expect_tie(items, 0, {true, TieReason::InstallFolder, 0});
    expect_tie(items, 1, {true, TieReason::UninstallerFolder, 0});
    expect_tie(items, 2, {true, TieReason::IconFolder, 0});
    expect_tie(items, 3, {true, TieReason::UninstallKey, 0});
    for (const std::size_t msi : {4u, 5u, 6u, 7u, 8u, 9u}) {
        expect_tie(items, msi, {true, TieReason::MsiRegistration, 0});
    }
    expect_tie(items, 10, {true, TieReason::InstallFolder, 0});
    expect_tie(items, 11, {true, TieReason::UninstallKey, 0});
    expect_tie(items, 12, {false, TieReason::None, 0});
    expect_tie(items, 13, {false, TieReason::None, 0});
}

TEST(TraceItemsTest, CreatedRootsNamedAfterTheProgramOrPublisherAreTied) {
    TraceResult result;
    result.changes = {
        folder(LR"(C:\Users\Test\AppData\Roaming\Widget Studio)"),               // 0: the program
        file(LR"(C:\Users\Public\Desktop\Widget Studio.lnk)"),                   // 1: without its extension
        key(LR"(SOFTWARE\Fabrikam)"),                                            // 2: the publisher
        created(leftovers::ServiceLocation{L"Widget Studio Updater", false}),    // 3
        created(leftovers::TaskLocation{LR"(\Fabrikam\Widget Studio Update)"}),  // 4
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", L"Widget Studio"),  // 5
        // Not created, or not named after it: untied.
        value(LR"(SOFTWARE\Fabrikam Shared)", L"Theme", ChangeKind::Modified),         // 6
        folder(LR"(C:\Users\Test\AppData\Local\Unrelated)"),                           // 7
        created(FileLocation{LR"(C:\Old\Widget Studio)", true}, ChangeKind::Deleted),  // 8
    };
    TraceChange noise = folder(LR"(C:\Windows\Prefetch\WIDGET STUDIO.EXE-1.pf)");
    noise.noise = NoiseReason::SystemActivity;
    result.changes.push_back(noise);  // 9: noise isn't tied or untied

    const auto items = tie_trace_items(result, widget_studio(), FakeStrings().sources());
    ASSERT_EQ(items.size(), 9u);
    expect_tie(items, 0, {true, TieReason::NamedAfterProgram, 0});
    expect_tie(items, 1, {true, TieReason::NamedAfterProgram, 0});
    expect_tie(items, 2, {true, TieReason::NamedAfterPublisher, 0});
    expect_tie(items, 3, {true, TieReason::NamedAfterProgram, 0});
    expect_tie(items, 4, {true, TieReason::NamedAfterProgram, 0});
    expect_tie(items, 5, {true, TieReason::NamedAfterProgram, 0});
    expect_tie(items, 6, {false, TieReason::None, 0});
    expect_tie(items, 7, {false, TieReason::None, 0});
    expect_tie(items, 8, {false, TieReason::None, 0});
    EXPECT_TRUE(
        std::none_of(items.begin(), items.end(), [](const TiedItem& item) { return item.change == 9; }));
}

// Round 1 ties what references the entry's folders or round 0's items, or lies inside them;
// round 2 what references those, round 3 what references round 2's. A fourth step stays untied.
TEST(TraceItemsTest, ReferencesAndContainmentTieOverThreeRounds) {
    TraceResult result;
    result.changes = {
        folder(kInstall),                                                                          // 0
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", L"Tray"),                       // 1
        key(LR"(SOFTWARE\Classes\CLSID\)" + std::wstring(kClsid)),                                 // 2
        key(LR"(SOFTWARE\Classes\WS.Doc.1)"),                                                      // 3
        key(LR"(SOFTWARE\Classes\WS.Doc)"),                                                        // 4
        key(LR"(SOFTWARE\Classes\WS.Legacy)"),                                                     // 5
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved)", kClsid),  // 6
        created(leftovers::ServiceLocation{L"WsAgent", false}),                                    // 7
        created(leftovers::TaskLocation{LR"(\Maintenance\Nightly)"}),                              // 8
        key(LR"(SOFTWARE\Classes\.wsd)"),                                                          // 9
        folder(LR"(D:\Data\Widget Studio)"),                                                       // 10
        created(FileLocation{LR"(D:\Data\Widget Studio\notes.txt)", false}, ChangeKind::Renamed),  // 11
        key(LR"(SOFTWARE\Classes\*\shellex\ContextMenuHandlers\WS)"),                              // 12
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\ws.exe)", L"Path"),          // 13
        key(LR"(SOFTWARE\Unrelated)"),                                                             // 14
        value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", L"Elsewhere"),                  // 15
    };
    FakeStrings strings;
    strings.set(result.changes[1], {LR"("C:\Program Files\Widget Studio\tray.exe" /background)"});
    strings.set(result.changes[2],
                {L"Widget shell extension", LR"(C:\Program Files\Widget Studio\shell.dll)"});
    strings.set(result.changes[3], {L"Widget document", kClsid});       // Its CLSID subkey
    strings.set(result.changes[4], {L"Widget document", L"WS.Doc.1"});  // CurVer
    strings.set(result.changes[5], {L"WS.Doc"});                        // A fourth step
    strings.set(result.changes[7], {LR"("C:\Program Files\Widget Studio\agent.exe" --service)"});
    strings.set(result.changes[8], {LR"(C:\Widget Icons\refresh.exe)"});  // The icon's folder
    strings.set(result.changes[9], {L"WS.Doc"});                          // Round 3, through WS.Doc
    strings.set(result.changes[12], {kClsid});
    strings.set(result.changes[13],
                {LR"(C:\Program Files\Widget Studio\bin,0)"});  // A path with an icon index
    strings.set(result.changes[14], {LR"(C:\Program Files\Other\x.exe)", L"Widget document"});
    strings.set(result.changes[15], {LR"(C:\Program Files\Widget Studio Extras\x.exe)"});  // Not inside it

    const auto items = tie_trace_items(result, widget_studio(), strings.sources());
    expect_tie(items, 0, {true, TieReason::InstallFolder, 0});
    expect_tie(items, 1, {true, TieReason::ReferencesTied, 1}, 0);  // The traced folder, not the entry's
    expect_tie(items, 2, {true, TieReason::ReferencesTied, 1}, 0);
    expect_tie(items, 3, {true, TieReason::ReferencesTied, 2}, 2);  // By the CLSID
    expect_tie(items, 4, {true, TieReason::ReferencesTied, 3}, 3);  // By the ProgID
    expect_tie(items, 5, {false, TieReason::None, 0});
    expect_tie(items, 6, {true, TieReason::ReferencesTied, 2}, 2);  // Named by the CLSID
    expect_tie(items, 7, {true, TieReason::ReferencesTied, 1}, 0);
    expect_tie(items, 8, {true, TieReason::ReferencesTied, 1}, std::nullopt);  // The entry's icon folder
    expect_tie(items, 9, {false, TieReason::None, 0});  // WS.Doc is tied only in round 3
    expect_tie(items, 10, {true, TieReason::NamedAfterProgram, 0});
    expect_tie(items, 11, {true, TieReason::InsideTied, 1}, 10);
    expect_tie(items, 12, {true, TieReason::ReferencesTied, 2}, 2);
    expect_tie(items, 13, {true, TieReason::ReferencesTied, 1}, 0);
    expect_tie(items, 14, {false, TieReason::None, 0});
    expect_tie(items, 15, {false, TieReason::None, 0});
}

// An install folder as broad as Program Files ties nothing by itself; neither does a drive root.
TEST(TraceItemsTest, BroadFoldersAreNoProgramsOwn) {
    ProgramPlaces places = widget_studio();
    places.target.install_location = LR"(C:\Program Files)";
    places.target.uninstaller_folder = LR"(C:\)";
    places.icon_folder.clear();
    places.target.uninstall_key.reset();
    places.target.msi_product_code.clear();
    TraceResult result;
    result.changes = {folder(LR"(C:\Program Files\Something)"), file(LR"(C:\stray.txt)"),
                      value(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", L"Something")};
    FakeStrings strings;
    strings.set(result.changes[2], {LR"(C:\Program Files\Something\x.exe)"});
    const auto items = tie_trace_items(result, places, strings.sources());
    for (std::size_t i = 0; i < items.size(); ++i) {
        expect_tie(items, i, {false, TieReason::None, 0});
    }
}

TEST(TraceItemsTest, ReasonsHaveNames) {
    EXPECT_EQ(to_string(TieReason::None), "none");
    EXPECT_EQ(to_string(TieReason::InstallFolder), "install-folder");
    EXPECT_EQ(to_string(TieReason::MsiRegistration), "msi-registration");
    EXPECT_EQ(to_string(TieReason::NamedAfterPublisher), "named-after-publisher");
    EXPECT_EQ(to_string(TieReason::InsideTied), "inside-tied");
    EXPECT_EQ(to_string(TieReason::ReferencesTied), "references-tied");
}

}  // namespace
}  // namespace avel::tracker
