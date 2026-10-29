// The leftover scan with install traces (Scanner::Trace): a fake TraceSource's targets against a
// temporary folder tree and scratch registry keys standing in for the machine and a user. Checks
// the evidence of tied and untied items and the name match, the SharedDLLs penalty, items gone
// since, changes to what existed before never offered, value edits planned once per value (Restore
// at Questionable), protection, and merging with what the other scanners found.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/leftovers/post_uninstall.h"
#include "core/leftovers/scan.h"
#include "core/platform/text.h"
#include "scratch_registry.h"

namespace avel::leftovers {
namespace {

namespace fs = std::filesystem;
using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kProgramId[] = L"reg:HKLM64:ContosoFixture";
constexpr wchar_t kTraceId[] = L"20261025-100000-0000c0de";
constexpr wchar_t kMachineEnvironmentPath[] =
    LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)";
constexpr wchar_t kSharedDllsPath[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)";
constexpr wchar_t kServices[] = LR"(SYSTEM\CurrentControlSet\Services)";
constexpr wchar_t kTaskTree[] = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree)";
constexpr wchar_t kFirewallRules[] =
    LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)";

const RegistryAddress kMachineEnvironment{Hive::Machine, L"", kMachineEnvironmentPath};
const RegistryAddress kUserEnvironment{Hive::User, kSid, L"Environment"};
const RegistryAddress kSharedDlls{Hive::Machine, L"", kSharedDllsPath};

RawValue text(std::wstring name, std::wstring_view value, DWORD type = REG_SZ) {
    RawValue raw{std::move(name), type, {}};
    raw.data.resize((value.size() + 1) * sizeof(wchar_t));
    std::memcpy(raw.data.data(), value.data(), value.size() * sizeof(wchar_t));
    return raw;
}

RawValue count(std::wstring name, std::uint32_t number) {
    RawValue raw{std::move(name), REG_DWORD, std::vector<std::byte>(sizeof(number))};
    std::memcpy(raw.data.data(), &number, sizeof(number));
    return raw;
}

// A value as text, for comparing: "2:C:\Windows" (its type, then its string or count), "absent".
std::wstring shown(const std::optional<RawValue>& value) {
    if (!value) {
        return L"absent";
    }
    std::wstring text = std::to_wstring(value->type) + L":";
    if (const auto string = string_value_text(*value)) {
        return text + *string;
    }
    const auto number = shared_dll_count(*value);
    return text + (number ? std::to_wstring(*number) : L"?");
}

TracedItem traced(Location location, bool tied, TracedChange change = TracedChange::Created) {
    TracedItem item;
    item.category = category_of(location);
    item.location = std::move(location);
    item.change = change;
    item.tied = tied;
    if (tied) {
        item.tie = L"install-folder";
    }
    return item;
}

// Hands out the traces the test set up, by program id, as the tracker's StoreTraceSource would.
class FakeTraceSource : public TraceSource {
public:
    void set(std::wstring program_id, ScanTarget trace) {
        traces_.emplace_back(std::move(program_id), std::move(trace));
    }

    bool add_trace(const inventory::Program& program, ScanTarget& target) override {
        for (const auto& [id, trace] : traces_) {
            if (id == program.id) {
                target.trace_id = trace.trace_id;
                target.traced = trace.traced;
                target.traced_values = trace.traced_values;
                target.captured_values = trace.captured_values;
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::pair<std::wstring, ScanTarget>> traces_;
};

class TraceScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        com_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        root_ = fs::temp_directory_path() /
                (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-trace");
        fs::remove_all(root_);
        install_ = path(LR"(PF\Contoso Fixture)");
        file(LR"(PF\Contoso Fixture\app.exe)");
        folder(LR"(PF86)");
        folder(LR"(PD)");
        folder(LR"(Users\fixture\AppData\Roaming)");
        folder(LR"(Elsewhere\Tool Data)");
        folder(LR"(Elsewhere\Random Data)");
        file(LR"(Elsewhere\Contoso Fixture.dat)");
        file(LR"(Elsewhere\existing.ini)");
        file(LR"(Elsewhere\own.dll)");
        file(LR"(Shared Lib\shared.dll)");
        machine_.create(LR"(SOFTWARE\Zeta Widgets)");
        machine_.set_string(std::wstring(kServices) + LR"(\ZetaService)", L"DisplayName", L"Zeta");
        machine_.create(std::wstring(kTaskTree) + LR"(\Zeta\Updater)");
        machine_.set_string(kFirewallRules, L"{ZETA-RULE}", LR"(v2.31|Action=Allow|App=C:\Zeta\zeta.exe|)");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }

    std::wstring path(std::wstring_view relative) const { return (root_ / relative).wstring(); }
    void folder(std::wstring_view relative) const { fs::create_directories(root_ / relative); }
    void file(std::wstring_view relative) const {
        fs::create_directories((root_ / relative).parent_path());
        std::ofstream(root_ / relative) << "fixture";
    }

    Location dir(std::wstring_view relative) const { return FileLocation{path(relative), true}; }
    Location plain_file(std::wstring_view relative) const { return FileLocation{path(relative), false}; }
    static Location machine_key(std::wstring path) {
        return RegistryKeyLocation{{Hive::Machine, L"", std::move(path)}};
    }

    ScanEnvironment environment() {
        ScanEnvironment environment;
        environment.machine = machine_.get();
        auto& machine = environment.machine_folders;
        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        machine.windows = windows;
        machine.program_files = path(L"PF");
        machine.program_files_x86 = path(L"PF86");
        machine.program_data = path(L"PD");
        machine.users = path(L"Users");
        machine.all = {machine.program_files, machine.program_files_x86, machine.program_data, machine.users};
        UserScope user;
        user.profile = {kSid, L"PC\\fixture", path(LR"(Users\fixture)"), false};
        user.hive = user_.get();
        user.folders = platform::user_folders(user.hive, user.profile);
        environment.users.push_back(std::move(user));
        environment.current_user_sid = kSid;
        environment.tasks = std::vector<ScheduledTask>{};
        return environment;
    }

    // Contoso Fixture's target as make_scan_target gives it, without a trace.
    ScanTarget fixture() const {
        ScanTarget target;
        target.program_id = kProgramId;
        target.display_name = L"Contoso Fixture 1.0";
        target.names = make_product_names(target.display_name, L"Contoso Ltd");
        target.install_location = install_;
        return target;
    }

    ScanTarget traced_fixture(std::vector<TracedItem> items, std::vector<TracedValue> values = {}) const {
        ScanTarget target = fixture();
        target.trace_id = kTraceId;
        target.traced = std::move(items);
        target.traced_values = std::move(values);
        return target;
    }

    ScanResult scan(const std::vector<ScanTarget>& targets) {
        const auto env = environment();
        return scan_leftovers(env, targets, std::vector<inventory::Program>{});
    }

    static const Leftover* find(const ScanResult& result, const Location& location) {
        const std::wstring key = identity(location);
        for (const auto& item : result.leftovers) {
            if (identity(item.location) == key) {
                return &item;
            }
        }
        return nullptr;
    }

    static bool has(const Leftover& item, Evidence evidence) {
        return std::any_of(item.records.begin(), item.records.end(),
                           [&](const Record& record) { return record.evidence == evidence; });
    }

    static const Leftover* find_edit(const ScanResult& result, const RegistryAddress& key,
                                     std::wstring_view name) {
        for (const auto& item : result.leftovers) {
            const auto* edit = std::get_if<ValueEditLocation>(&item.location);
            if (edit && platform::same_address(edit->key, key) && platform::iequals(edit->value_name, name)) {
                return &item;
            }
        }
        return nullptr;
    }

    void expect_level(const ScanResult& result, const Location& location, Confidence expected) const {
        const Leftover* item = find(result, location);
        ASSERT_NE(item, nullptr) << platform::to_utf8(identity(location));
        EXPECT_EQ(level(item->records), expected)
            << platform::to_utf8(identity(location)) << " scored " << score(item->records);
    }

    HRESULT com_ = E_FAIL;
    fs::path root_;
    std::wstring install_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
};

TEST_F(TraceScannerTest, TiedItemsAreVeryLikelyAndUntiedOnesUncertainUnlessNamedLikeTheProgram) {
    const auto result = scan({traced_fixture({
        traced(dir(LR"(Elsewhere\Tool Data)"), true),
        traced(dir(LR"(Elsewhere\Random Data)"), false),
        traced(plain_file(LR"(Elsewhere\Contoso Fixture.dat)"), false),
        traced(machine_key(LR"(SOFTWARE\Zeta Widgets)"), true),
        traced(ServiceLocation{L"ZetaService", false}, true),
        traced(TaskLocation{LR"(\Zeta\Updater)", false}, true),
        traced(TaskLocation{LR"(\Zeta)", true}, true),
        traced(FirewallRuleLocation{L"{ZETA-RULE}"}, true),
    })});

    // Only the trace found these: TracedInstallRoot (+5) alone, or TracedCreated (+1).
    expect_level(result, dir(LR"(Elsewhere\Tool Data)"), Confidence::VeryGood);
    expect_level(result, dir(LR"(Elsewhere\Random Data)"), Confidence::Questionable);
    // An untied item named like the program: +1 and +2 for the name.
    expect_level(result, plain_file(LR"(Elsewhere\Contoso Fixture.dat)"), Confidence::Good);
    expect_level(result, machine_key(LR"(SOFTWARE\Zeta Widgets)"), Confidence::VeryGood);
    expect_level(result, ServiceLocation{L"ZetaService", false}, Confidence::VeryGood);
    expect_level(result, TaskLocation{LR"(\Zeta\Updater)", false}, Confidence::VeryGood);
    expect_level(result, TaskLocation{LR"(\Zeta)", true}, Confidence::VeryGood);
    // A firewall rule is reported like any traced item; removal leaves it (changes::is_removable).
    expect_level(result, FirewallRuleLocation{L"{ZETA-RULE}"}, Confidence::VeryGood);

    const Leftover* tied = find(result, dir(LR"(Elsewhere\Tool Data)"));
    ASSERT_NE(tied, nullptr);
    EXPECT_EQ(tied->found_by, Scanner::Trace);
    EXPECT_EQ(tied->category, Category::Files);
    EXPECT_EQ(tied->owner_ids, std::vector<std::wstring>{kProgramId});
    ASSERT_EQ(tied->records.size(), 1u);
    EXPECT_EQ(tied->records[0].evidence, Evidence::TracedInstallRoot);
    EXPECT_EQ(tied->records[0].detail, L"install-folder");
    EXPECT_EQ(find(result, ServiceLocation{L"ZetaService", false})->category, Category::Services);
    EXPECT_EQ(find(result, FirewallRuleLocation{L"{ZETA-RULE}"})->category, Category::Firewall);

    ASSERT_EQ(result.traces.size(), 1u);
    const TraceUse& use = result.traces[0];
    EXPECT_EQ(use.program_id, kProgramId);
    EXPECT_EQ(use.trace_id, kTraceId);
    EXPECT_EQ(use.items, 8u);
    EXPECT_EQ(use.tied, 6u);
    EXPECT_EQ(use.gone, 0u);
}

TEST_F(TraceScannerTest, ItemsGoneSinceAreDroppedAndOnlyCreatedOnesOffered) {
    const auto result = scan({traced_fixture({
        traced(dir(LR"(Elsewhere\Uninstalled)"), true),        // Gone: the uninstaller took it
        traced(machine_key(LR"(SOFTWARE\Gone Key)"), true),    // Gone
        traced(ServiceLocation{L"GoneService", false}, true),  // Gone
        traced(TaskLocation{LR"(\Gone\Task)", false}, true),   // Gone
        traced(RegistryValueLocation{{Hive::Machine, L"", LR"(SOFTWARE\Zeta Widgets)"}, L"Gone"}, true),
        traced(dir(LR"(Elsewhere\Tool Data)"), true, TracedChange::Modified),
        traced(plain_file(LR"(Elsewhere\existing.ini)"), true, TracedChange::Modified),
        traced(plain_file(LR"(Elsewhere\own.dll)"), false, TracedChange::Renamed),
        traced(plain_file(LR"(Elsewhere\deleted.txt)"), false, TracedChange::Deleted),
        traced(FileLocation{path(LR"(Elsewhere\Random Data)"), false}, true),  // A folder now, not a file
    })});

    EXPECT_FALSE(std::any_of(result.leftovers.begin(), result.leftovers.end(),
                             [](const Leftover& item) { return item.found_by == Scanner::Trace; }));
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].gone, 6u);
    EXPECT_EQ(result.traces[0].shown_only, 4u);
    EXPECT_EQ(result.traces[0].items, 0u);
}

TEST_F(TraceScannerTest, ProtectionStillRefusesWindowsAndKnownFolders) {
    wchar_t windows[MAX_PATH] = {};
    ::GetWindowsDirectoryW(windows, MAX_PATH);
    const Location system32 = FileLocation{std::wstring(windows) + LR"(\System32)", true};
    const auto result = scan({traced_fixture({
        traced(system32, true),
        traced(dir(L"PD"), true),  // A known folder of the machine
        traced(dir(LR"(Elsewhere\Tool Data)"), true),
    })});
    EXPECT_EQ(find(result, system32), nullptr);
    EXPECT_EQ(find(result, dir(L"PD")), nullptr);
    EXPECT_NE(find(result, dir(LR"(Elsewhere\Tool Data)")), nullptr);
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].protected_count, 2u);
    EXPECT_EQ(result.traces[0].items, 1u);
}

TEST_F(TraceScannerTest, MergesWithWhatTheOtherScannersFound) {
    // The install folder: the install-location scanner's explicit connection and the drive scan's
    // name match, plus the trace's tie, on one item that keeps the scanner that found it first.
    const auto result = scan({traced_fixture({traced(dir(LR"(PF\Contoso Fixture)"), true)})});
    const Leftover* item = find(result, dir(LR"(PF\Contoso Fixture)"));
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->found_by, Scanner::InstallLocation);
    EXPECT_TRUE(has(*item, Evidence::ExplicitConnection));
    EXPECT_TRUE(has(*item, Evidence::TracedInstallRoot));
    EXPECT_EQ(level(item->records), Confidence::VeryGood);
    EXPECT_EQ(std::count_if(result.leftovers.begin(), result.leftovers.end(),
                            [&](const Leftover& other) {
                                return identity(other.location) == identity(item->location);
                            }),
              1);
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].items, 1u);
    EXPECT_EQ(result.traces[0].tied, 1u);

    // Without a trace the same scan has no trace evidence at all.
    const auto plain = scan({fixture()});
    EXPECT_TRUE(plain.traces.empty());
    for (const auto& leftover : plain.leftovers) {
        EXPECT_FALSE(std::any_of(leftover.records.begin(), leftover.records.end(),
                                 [](const Record& record) { return is_trace_evidence(record.evidence); }));
    }
}

// The installer created DragDropHandlers for its own handler; another program added its handler
// there since. Only what the trace saw in the key goes.
TEST_F(TraceScannerTest, AKeyHoldingMoreThanTheTraceSawGoesAsTheItemsItSaw) {
    const std::wstring handlers = LR"(SOFTWARE\Classes\Directory\shellex\DragDropHandlers)";
    const std::wstring own = handlers + LR"(\SevenHandler)";
    const std::wstring others = handlers + LR"(\RarHandler)";
    machine_.set_string(own, L"", L"{C0C0A000-0000-0000-0000-000000000001}");
    machine_.set_string(others, L"", L"{2E7A0000-0000-0000-0000-000000000002}");
    TracedItem container = traced(machine_key(handlers), true);
    container.tie = L"references-tied";
    container.contents = 2;
    container.inside = std::vector<ContainedItem>{{L"SevenHandler", std::nullopt, true}, {L"SevenHandler", L"", false}};
    const auto result = scan({traced_fixture({container})});

    EXPECT_EQ(find(result, machine_key(handlers)), nullptr);
    EXPECT_EQ(find(result, machine_key(others)), nullptr);
    const Leftover* item = find(result, machine_key(own));
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->found_by, Scanner::Trace);
    EXPECT_EQ(item->category, Category::Registry);
    ASSERT_EQ(item->records.size(), 1u);
    EXPECT_EQ(item->records[0].evidence, Evidence::TracedInstallRoot);
    EXPECT_EQ(item->records[0].detail, L"references-tied");
    EXPECT_EQ(level(item->records), Confidence::VeryGood);
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].split, 1u);
    EXPECT_EQ(result.traces[0].split_into, 1u);
    EXPECT_EQ(result.traces[0].items, 1u);

    // A value someone added to the installer's own subkey keeps that subkey too: only the value
    // the trace saw goes.
    machine_.set_string(own, L"Added", L"by someone else");
    const auto deeper = scan({traced_fixture({container})});
    EXPECT_EQ(find(deeper, machine_key(own)), nullptr);
    EXPECT_NE(find(deeper, RegistryValueLocation{{Hive::Machine, L"", own}, L""}), nullptr);
    EXPECT_EQ(find(deeper, RegistryValueLocation{{Hive::Machine, L"", own}, L"Added"}), nullptr);
    EXPECT_EQ(deeper.traces[0].split_into, 1u);

    // Holding no more than the trace saw (less is fine), the key goes whole.
    EXPECT_EQ(::RegDeleteTreeW(machine_.get(), others.c_str()), ERROR_SUCCESS);
    EXPECT_EQ(::RegDeleteKeyValueW(machine_.get(), own.c_str(), L"Added"), ERROR_SUCCESS);
    const auto alone = scan({traced_fixture({container})});
    expect_level(alone, machine_key(handlers), Confidence::VeryGood);
    EXPECT_EQ(find(alone, machine_key(own)), nullptr);
    EXPECT_EQ(alone.traces[0].split, 0u);
    EXPECT_EQ(alone.traces[0].items, 1u);
}

// A trace that kept only how many items the key held: it can't tell which are the installer's.
TEST_F(TraceScannerTest, AContainerHoldingMoreThanItsCountIsOfferedWholeBelowGood) {
    const std::wstring handlers = LR"(SOFTWARE\Classes\Directory\shellex\DragDropHandlers)";
    machine_.set_string(handlers + LR"(\SevenHandler)", L"", L"{C0C0A000-0000-0000-0000-000000000001}");
    machine_.set_string(handlers + LR"(\RarHandler)", L"", L"{2E7A0000-0000-0000-0000-000000000002}");
    TracedItem tied = traced(machine_key(handlers), true);
    tied.contents = 2;  // SevenHandler and its value
    folder(LR"(Elsewhere\Random Data\Later)");
    TracedItem untied = traced(dir(LR"(Elsewhere\Random Data)"), false);
    const auto result = scan({traced_fixture({tied, untied})});

    const Leftover* key = find(result, machine_key(handlers));
    ASSERT_NE(key, nullptr);
    EXPECT_TRUE(has(*key, Evidence::ContainerHoldsMore));
    EXPECT_EQ(level(key->records), Confidence::Questionable);  // +5 - 4
    const Leftover* data = find(result, dir(LR"(Elsewhere\Random Data)"));
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(has(*data, Evidence::ContainerHoldsMore));
    EXPECT_EQ(level(data->records), Confidence::Bad);  // +1 - 4
    EXPECT_EQ(result.traces[0].split, 0u);
    EXPECT_EQ(result.traces[0].items, 2u);

    // Holding what the count says, it goes whole as before.
    tied.contents = 4;
    untied.contents = 1;
    const auto counted = scan({traced_fixture({tied, untied})});
    expect_level(counted, machine_key(handlers), Confidence::VeryGood);
    EXPECT_FALSE(has(*find(counted, machine_key(handlers)), Evidence::ContainerHoldsMore));
    EXPECT_FALSE(has(*find(counted, dir(LR"(Elsewhere\Random Data)")), Evidence::ContainerHoldsMore));
}

// A folder the installer created that other programs or the user put files in since.
TEST_F(TraceScannerTest, AFolderHoldingMoreThanTheTraceSawGoesAsTheItemsItSaw) {
    file(LR"(Elsewhere\Shared Tools\contoso.dll)");
    file(LR"(Elsewhere\Shared Tools\other.dll)");  // Someone else's
    file(LR"(Elsewhere\Shared Tools\Sub\a.txt)");
    file(LR"(Elsewhere\Shared Tools\Sub\b.txt)");  // Someone else's
    file(LR"(Elsewhere\Shared Tools\Clean\c.txt)");
    TracedItem tools = traced(dir(LR"(Elsewhere\Shared Tools)"), true);
    tools.contents = 6;
    tools.inside = std::vector<ContainedItem>{
        {L"Clean", std::nullopt, true},   {LR"(Clean\c.txt)", std::nullopt, false},
        {L"Sub", std::nullopt, true},     {LR"(Sub\a.txt)", std::nullopt, false},
        {L"contoso.dll", std::nullopt, false}, {L"gone.txt", std::nullopt, false},  // Removed since
    };
    const auto result = scan({traced_fixture({tools})});

    for (const auto* kept : {LR"(Elsewhere\Shared Tools)", LR"(Elsewhere\Shared Tools\Sub)"}) {
        EXPECT_EQ(find(result, dir(kept)), nullptr) << platform::to_utf8(kept);
    }
    for (const auto* kept : {LR"(Elsewhere\Shared Tools\other.dll)", LR"(Elsewhere\Shared Tools\Sub\b.txt)",
                             LR"(Elsewhere\Shared Tools\gone.txt)"}) {
        EXPECT_EQ(find(result, plain_file(kept)), nullptr) << platform::to_utf8(kept);
    }
    expect_level(result, plain_file(LR"(Elsewhere\Shared Tools\contoso.dll)"), Confidence::VeryGood);
    expect_level(result, plain_file(LR"(Elsewhere\Shared Tools\Sub\a.txt)"), Confidence::VeryGood);
    expect_level(result, dir(LR"(Elsewhere\Shared Tools\Clean)"), Confidence::VeryGood);  // Whole: nothing added
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].split, 1u);
    EXPECT_EQ(result.traces[0].split_into, 3u);
    EXPECT_EQ(result.traces[0].items, 3u);
    EXPECT_EQ(result.traces[0].tied, 3u);
}

// Notepad++ registers a sparse package for its context menu while it installs: Windows writes the
// package's keys, which go with the package, never as a trace's raw keys and values.
TEST_F(TraceScannerTest, APackagesOwnRegistrationIsNeverOffered) {
    const std::wstring package =
        LR"(Software\Classes\PackagedCom\Package\NotepadPlusPlus_1.0.0.0_neutral__7njy0v32s6xk6)";
    const std::wstring prog_id = L"AppX4hxtad77fbk3jkkeerkrm0ze94wjf3s9";
    user_.set_string(package + LR"(\Class\{B298D29A-A6ED-11DE-BA8C-A68E55D89593})", L"DllPath", L"NppShell.dll");
    user_.set_string(LR"(Software\Classes\)" + prog_id, L"", L"Text document");
    user_.set_string(LR"(Software\Classes\.txt\OpenWithProgids)", prog_id, L"");
    const auto user_key = [](std::wstring path) -> Location {
        return RegistryKeyLocation{{Hive::User, kSid, std::move(path)}};
    };
    const Location association =
        RegistryValueLocation{{Hive::User, kSid, LR"(Software\Classes\.txt\OpenWithProgids)"}, prog_id};
    const auto result = scan({traced_fixture({
        traced(user_key(package), true),
        traced(user_key(LR"(Software\Classes\)" + prog_id), true),
        traced(association, true),
        traced(dir(LR"(Elsewhere\Tool Data)"), true),
    })});
    EXPECT_EQ(find(result, user_key(package)), nullptr);
    EXPECT_EQ(find(result, user_key(LR"(Software\Classes\)" + prog_id)), nullptr);
    EXPECT_EQ(find(result, association), nullptr);
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].protected_count, 3u);
    EXPECT_EQ(result.traces[0].items, 1u);
}

TEST_F(TraceScannerTest, ASharedDllAnotherProgramStillCountsIsUnlikely) {
    const std::wstring shared = path(LR"(Shared Lib\shared.dll)");
    const std::wstring own = path(LR"(Elsewhere\own.dll)");
    machine_.set_dword(kSharedDllsPath, shared, 2);  // Another program installed after this one
    machine_.set_dword(kSharedDllsPath, own, 1);
    const auto result = scan(
        {traced_fixture({traced(plain_file(LR"(Shared Lib\shared.dll)"), true),
                         traced(plain_file(LR"(Elsewhere\own.dll)"), true), traced(dir(L"Shared Lib"), true)},
                        {{ValueSpot::SharedDlls, kSharedDlls, shared, count(shared, 1), count(shared, 2)},
                         {ValueSpot::SharedDlls, kSharedDlls, own, std::nullopt, count(own, 1)}})});

    // 2 - 1 = 1 left: tied (+5) and still used (-7).
    const Leftover* item = find(result, plain_file(LR"(Shared Lib\shared.dll)"));
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(has(*item, Evidence::SharedDllStillUsed));
    EXPECT_EQ(level(item->records), Confidence::Bad);
    // So is the folder that holds it.
    const Leftover* holder = find(result, dir(L"Shared Lib"));
    ASSERT_NE(holder, nullptr);
    EXPECT_TRUE(has(*holder, Evidence::SharedDllStillUsed));
    // Its own DLL's count goes to 0 with its edit: no penalty.
    expect_level(result, plain_file(LR"(Elsewhere\own.dll)"), Confidence::VeryGood);
    const Leftover* own_dll = find(result, plain_file(LR"(Elsewhere\own.dll)"));
    ASSERT_NE(own_dll, nullptr);
    EXPECT_FALSE(has(*own_dll, Evidence::SharedDllStillUsed));

    const Leftover* lowered = find_edit(result, kSharedDlls, shared);
    ASSERT_NE(lowered, nullptr);
    const auto& edit = std::get<ValueEditLocation>(lowered->location);
    EXPECT_EQ(edit.kind, ValueEditKind::DecrementSharedDll);
    EXPECT_EQ(edit.delta, 1);
    // Its path is a tied file: the edit points into what the program installed.
    EXPECT_EQ(level(lowered->records), Confidence::VeryGood);
    const Leftover* deleted = find_edit(result, kSharedDlls, own);
    ASSERT_NE(deleted, nullptr);
    EXPECT_EQ(std::get<ValueEditLocation>(deleted->location).kind, ValueEditKind::DeleteValue);
}

TEST_F(TraceScannerTest, ValueEditsUndoAtGoodOrBetterAndRestoreAtQuestionable) {
    const std::wstring entry = install_ + L"\\bin";
    machine_.set_string(kMachineEnvironmentPath, L"Path", L"C:\\Windows;" + entry, REG_EXPAND_SZ);
    machine_.set_string(kMachineEnvironmentPath, L"CONTOSO_HOME", install_);
    machine_.set_string(kMachineEnvironmentPath, L"CONTOSO_FLAG", L"1");
    machine_.set_string(kMachineEnvironmentPath, L"CONTOSO_THEME", L"dark");
    user_.set_string(L"Environment", L"Path", L"C:\\Tools;D:\\Mine", REG_EXPAND_SZ);
    const auto result =
        scan({traced_fixture({traced(dir(LR"(PF\Contoso Fixture)"), true)},
                             {
                                 {ValueSpot::MachineEnvironment, kMachineEnvironment, L"Path",
                                  text(L"Path", L"C:\\Windows", REG_EXPAND_SZ),
                                  text(L"Path", L"C:\\Windows;" + entry, REG_EXPAND_SZ)},
                                 {ValueSpot::MachineEnvironment, kMachineEnvironment, L"CONTOSO_HOME",
                                  std::nullopt, text(L"CONTOSO_HOME", install_)},
                                 {ValueSpot::MachineEnvironment, kMachineEnvironment, L"CONTOSO_FLAG",
                                  std::nullopt, text(L"CONTOSO_FLAG", L"1")},
                                 {ValueSpot::MachineEnvironment, kMachineEnvironment, L"CONTOSO_THEME",
                                  text(L"CONTOSO_THEME", L"light"), text(L"CONTOSO_THEME", L"dark")},
                                 // The user took the installer's entry out already.
                                 {ValueSpot::UserEnvironment, kUserEnvironment, L"Path",
                                  text(L"Path", L"C:\\Tools", REG_EXPAND_SZ),
                                  text(L"Path", L"C:\\Tools;C:\\Elsewhere\\bin", REG_EXPAND_SZ)},
                             })});

    // Entries into the install folder: undo (+3) and a path reference (+4).
    const Leftover* path = find_edit(result, kMachineEnvironment, L"Path");
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->found_by, Scanner::Trace);
    EXPECT_EQ(path->category, Category::Registry);
    EXPECT_TRUE(has(*path, Evidence::TracedValueUndo));
    EXPECT_TRUE(has(*path, Evidence::PathReference));
    EXPECT_EQ(level(path->records), Confidence::VeryGood);
    const auto& removal = std::get<ValueEditLocation>(path->location);
    EXPECT_EQ(removal.kind, ValueEditKind::RemovePathEntries);
    EXPECT_EQ(removal.entries, std::vector<std::wstring>{entry});
    EXPECT_EQ(shown(removal.desired), shown(text(L"Path", L"C:\\Windows", REG_EXPAND_SZ)));

    const Leftover* home = find_edit(result, kMachineEnvironment, L"CONTOSO_HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(std::get<ValueEditLocation>(home->location).kind, ValueEditKind::DeleteValue);
    EXPECT_EQ(level(home->records), Confidence::VeryGood);
    // A created variable that names nothing of the program's: undo alone.
    const Leftover* flag = find_edit(result, kMachineEnvironment, L"CONTOSO_FLAG");
    ASSERT_NE(flag, nullptr);
    EXPECT_EQ(level(flag->records), Confidence::Good);
    // The old value back: the new one may be what the user wants.
    const Leftover* theme = find_edit(result, kMachineEnvironment, L"CONTOSO_THEME");
    ASSERT_NE(theme, nullptr);
    EXPECT_EQ(std::get<ValueEditLocation>(theme->location).kind, ValueEditKind::RestoreValue);
    EXPECT_TRUE(has(*theme, Evidence::TracedValueRestore));
    EXPECT_EQ(level(theme->records), Confidence::Questionable);
    EXPECT_EQ(find_edit(result, kUserEnvironment, L"Path"), nullptr);

    ASSERT_EQ(result.traces.size(), 1u);
    const TraceUse& use = result.traces[0];
    EXPECT_EQ(use.value_edits, 4u);
    ASSERT_EQ(use.values.size(), 5u);
    EXPECT_EQ(use.values[0].basis, ValueEditBasis::Undo);
    EXPECT_EQ(use.values[3].basis, ValueEditBasis::Restore);
    EXPECT_EQ(use.values[4].basis, ValueEditBasis::AlreadyUndone);
}

TEST_F(TraceScannerTest, TwoTracesEditingOneValueMakeOneEdit) {
    const std::wstring first = install_ + L"\\bin";
    const std::wstring second = path(LR"(Elsewhere\Zeta\bin)");
    machine_.set_string(kMachineEnvironmentPath, L"Path", L"C:\\Windows;" + first + L";" + second,
                        REG_EXPAND_SZ);
    ScanTarget contoso = traced_fixture({}, {{ValueSpot::MachineEnvironment, kMachineEnvironment, L"Path",
                                              text(L"Path", L"C:\\Windows", REG_EXPAND_SZ),
                                              text(L"Path", L"C:\\Windows;" + first, REG_EXPAND_SZ)}});
    ScanTarget zeta;
    zeta.program_id = L"reg:HKLM64:Zeta";
    zeta.display_name = L"Zeta Tools";
    zeta.names = make_product_names(zeta.display_name, L"Zeta");
    zeta.trace_id = L"20261025-110000-0000beef";
    zeta.traced_values = {{ValueSpot::MachineEnvironment, kMachineEnvironment, L"Path",
                           text(L"Path", L"C:\\Windows;" + first, REG_EXPAND_SZ),
                           text(L"Path", L"C:\\Windows;" + first + L";" + second, REG_EXPAND_SZ)}};
    const auto result = scan({contoso, zeta});

    std::vector<const Leftover*> edits;
    for (const auto& item : result.leftovers) {
        if (std::holds_alternative<ValueEditLocation>(item.location)) {
            edits.push_back(&item);
        }
    }
    ASSERT_EQ(edits.size(), 1u);
    const auto& edit = std::get<ValueEditLocation>(edits[0]->location);
    EXPECT_EQ(edit.kind, ValueEditKind::RemovePathEntries);
    EXPECT_EQ(shown(edit.expected),
              shown(text(L"Path", L"C:\\Windows;" + first + L";" + second, REG_EXPAND_SZ)));
    EXPECT_EQ(shown(edit.desired), shown(text(L"Path", L"C:\\Windows", REG_EXPAND_SZ)));
    EXPECT_EQ(edit.entries, (std::vector<std::wstring>{first, second}));
    EXPECT_EQ(edits[0]->owner_ids, (std::vector<std::wstring>{kProgramId, L"reg:HKLM64:Zeta"}));
    ASSERT_EQ(result.traces.size(), 2u);
    EXPECT_EQ(result.traces[0].values[0].basis, ValueEditBasis::Undo);
    EXPECT_EQ(result.traces[1].values[0].basis, ValueEditBasis::Undo);
}

TEST_F(TraceScannerTest, OneTraceSharedByTwoTargetsLowersACountOnce) {
    const std::wstring dll = path(LR"(Shared Lib\shared.dll)");
    machine_.set_dword(kSharedDllsPath, dll, 2);
    const TracedValue raised{ValueSpot::SharedDlls, kSharedDlls, dll, count(dll, 1), count(dll, 2)};
    ScanTarget bundle = traced_fixture({}, {raised});
    ScanTarget package = traced_fixture({}, {raised});
    package.program_id = L"msi:{11111111-2222-3333-4444-555555555555}";
    const auto result = scan({bundle, package});

    const Leftover* item = find_edit(result, kSharedDlls, dll);
    ASSERT_NE(item, nullptr);
    const auto& edit = std::get<ValueEditLocation>(item->location);
    EXPECT_EQ(shown(edit.desired), shown(count(dll, 1)));
    EXPECT_EQ(edit.delta, 1);
    EXPECT_EQ(item->owner_ids.size(), 2u);
    ASSERT_EQ(result.traces.size(), 2u);
    EXPECT_EQ(result.traces[1].values[0].basis, ValueEditBasis::Undo);
}

TEST_F(TraceScannerTest, ACountTheUninstallerLoweredIsNotLoweredAgain) {
    const std::wstring dll = path(LR"(Shared Lib\shared.dll)");
    machine_.set_dword(kSharedDllsPath, dll, 1);  // The uninstaller took its reference back: 2 -> 1
    ScanTarget target =
        traced_fixture({traced(plain_file(LR"(Shared Lib\shared.dll)"), true)},
                       {{ValueSpot::SharedDlls, kSharedDlls, dll, count(dll, 1), count(dll, 2)}});
    target.captured_values = {{true, count(dll, 2)}};
    const auto result = scan({target});
    EXPECT_EQ(find_edit(result, kSharedDlls, dll), nullptr);
    ASSERT_EQ(result.traces.size(), 1u);
    EXPECT_EQ(result.traces[0].values[0].basis, ValueEditBasis::AlreadyUndone);
    // The count stays 1: the DLL is another program's too.
    const Leftover* file = find(result, plain_file(LR"(Shared Lib\shared.dll)"));
    ASSERT_NE(file, nullptr);
    EXPECT_TRUE(has(*file, Evidence::SharedDllStillUsed));
}

TEST_F(TraceScannerTest, AnEditReplacesTheRawValueTheReferencesFound) {
    // A SharedDLLs value naming a file in the install folder: the references scanner offers to
    // delete it; the trace says the installer created it, so its edit stands for it.
    const std::wstring dll = install_ + L"\\bundled.dll";
    const std::wstring other = install_ + L"\\other.dll";
    machine_.set_dword(kSharedDllsPath, dll, 1);
    machine_.set_dword(kSharedDllsPath, other, 2);
    ScanTarget target =
        traced_fixture({}, {{ValueSpot::SharedDlls, kSharedDlls, dll, std::nullopt, count(dll, 1)},
                            // Raised by the installer from 2 to 3, and lowered by its uninstaller since.
                            {ValueSpot::SharedDlls, kSharedDlls, other, count(other, 2), count(other, 3)}});
    target.captured_values = {{}, {true, count(other, 3)}};
    const auto result = scan({target});
    EXPECT_EQ(find(result, RegistryValueLocation{kSharedDlls, dll}), nullptr);
    const Leftover* edit = find_edit(result, kSharedDlls, dll);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(std::get<ValueEditLocation>(edit->location).kind, ValueEditKind::DeleteValue);
    EXPECT_TRUE(has(*edit, Evidence::TracedValueUndo));
    const auto reference = std::find_if(edit->records.begin(), edit->records.end(), [](const Record& record) {
        return record.evidence == Evidence::PathReference;
    });
    ASSERT_NE(reference, edit->records.end());
    EXPECT_EQ(reference->reference, ReferenceKind::SharedDll);
    EXPECT_EQ(level(edit->records), Confidence::VeryGood);
    // Nothing of the installer's is left in the other count: neither an edit nor the raw value.
    EXPECT_EQ(find(result, RegistryValueLocation{kSharedDlls, other}), nullptr);
    EXPECT_EQ(find_edit(result, kSharedDlls, other), nullptr);

    // Without the trace, the references offer both raw values, as they always did.
    const auto plain = scan({fixture()});
    EXPECT_NE(find(plain, RegistryValueLocation{kSharedDlls, dll}), nullptr);
    EXPECT_NE(find(plain, RegistryValueLocation{kSharedDlls, other}), nullptr);
}

TEST_F(TraceScannerTest, CaptureTargetsAddsTheTraceFromTheSource) {
    inventory::Program traced_program;
    traced_program.id = kProgramId;
    traced_program.display_name = L"Contoso Fixture 1.0";
    inventory::Program other;
    other.id = L"reg:HKLM64:Other";
    other.display_name = L"Other";
    FakeTraceSource source;
    source.set(kProgramId, traced_fixture({traced(dir(LR"(Elsewhere\Tool Data)"), true)}));
    const std::vector<const inventory::Program*> programs{&traced_program, &other};

    const auto targets = capture_targets(programs, &source);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].trace_id, kTraceId);
    EXPECT_EQ(targets[0].traced.size(), 1u);
    EXPECT_TRUE(targets[1].trace_id.empty());
    EXPECT_TRUE(targets[1].traced.empty());
    // Without a source, nothing is traced.
    EXPECT_TRUE(capture_targets(programs).front().trace_id.empty());
}

}  // namespace
}  // namespace avel::leftovers
