// A trace's review (review.json): excluding and taking back items by their identity, the exact
// JSON round trip, the store's reading and writing of it, the trace source leaving excluded items
// out and passing them on to the scan (and not using a trace whose review is damaged), and export
// and import carrying it.
#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "core/tracker/known_values.h"
#include "core/tracker/linking.h"
#include "core/tracker/trace_export.h"
#include "core/tracker/trace_review.h"
#include "core/tracker/trace_source.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using changes::Json;
using leftovers::FileLocation;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kInstall[] = LR"(C:\Avel.Tests\Contoso Fixture)";
constexpr wchar_t kAlice[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kAliceProfile[] = LR"(C:\Users\Alice)";
constexpr wchar_t kBob[] = L"S-1-5-21-9-8-7-1002";
constexpr wchar_t kBobProfile[] = LR"(D:\Profiles\Bob)";

RegistryAddress machine(std::wstring path) { return {Hive::Machine, L"", std::move(path)}; }

TraceChange change(leftovers::Location location, ChangeKind kind = ChangeKind::Created) {
    TraceChange result;
    result.change = kind;
    result.location = std::move(location);
    return result;
}

platform::RawValue string_value(std::wstring name, std::wstring_view text) {
    platform::RawValue value{std::move(name), REG_EXPAND_SZ, {}};
    value.data.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

KnownValueChange path_change(const RegistryAddress& key = fakes::kMachineEnvironment,
                             KnownSpot spot = KnownSpot::MachineEnvironment) {
    return {spot, key, L"Path", string_value(L"Path", L"C:\\Windows"),
            string_value(L"Path", L"C:\\Windows;C:\\Avel.Tests\\Contoso Fixture\\bin")};
}

inventory::Program contoso() {
    inventory::Program program;
    program.id = L"reg:HKLM64:ContosoFixture";
    program.display_name = L"Contoso Fixture 1.0";
    program.publisher = L"Contoso Ltd";
    program.install_location = kInstall;
    program.registry = inventory::RegistryLocation{inventory::RegistryView::Machine64, L"ContosoFixture", {}};
    return program;
}

StoreTraceSourceOptions fake_options() {
    TieSources tie;
    tie.strings = [](const leftovers::Location&) { return std::vector<std::wstring>(); };
    tie.expand = [](std::wstring_view text) { return std::wstring(text); };
    tie.file_exists = [](const std::wstring&) { return false; };
    StoreTraceSourceOptions options;
    options.tie = std::move(tie);
    options.read_value = [](const RegistryAddress&, const std::wstring&) {
        return leftovers::CapturedValue{true, std::nullopt};
    };
    return options;
}

TEST(TraceReviewTest, ExcludesAndTakesBackItemsByTheirIdentity) {
    const TraceChange folder = change(FileLocation{kInstall, true});
    const TraceChange key = change(RegistryKeyLocation{machine(LR"(SOFTWARE\Contoso)")});
    const KnownValueChange path = path_change();
    TraceReview review;
    EXPECT_TRUE(review.empty());
    EXPECT_FALSE(is_excluded(review, folder));

    EXPECT_TRUE(set_excluded(review, folder, true));
    EXPECT_FALSE(set_excluded(review, folder, true));  // Already excluded
    EXPECT_TRUE(set_excluded(review, path, true));
    EXPECT_TRUE(is_excluded(review, folder));
    EXPECT_FALSE(is_excluded(review, key));
    EXPECT_TRUE(is_excluded(review, path));
    EXPECT_FALSE(review.empty());
    // Names compare as Windows compares them.
    EXPECT_TRUE(is_excluded(review, change(FileLocation{LR"(c:\avel.tests\CONTOSO FIXTURE)", true})));
    KnownValueChange other_case = path;
    other_case.name = L"PATH";
    EXPECT_TRUE(is_excluded(review, other_case));
    KnownValueChange user_path =
        path_change({Hive::User, kAlice, L"Environment"}, KnownSpot::UserEnvironment);
    EXPECT_FALSE(is_excluded(review, user_path));

    EXPECT_TRUE(set_excluded(review, other_case, false));
    EXPECT_FALSE(is_excluded(review, path));
    EXPECT_FALSE(set_excluded(review, path, false));
    EXPECT_TRUE(set_excluded(review, folder, false));
    EXPECT_TRUE(review.empty());
}

TEST(TraceReviewTest, ApplyingItLeavesTheExcludedItemsOutInOrder) {
    TraceResult result;
    result.changes = {change(FileLocation{kInstall, true}),
                      change(FileLocation{LR"(C:\Avel.Tests\Elsewhere\x.bin)", false}),
                      change(RegistryKeyLocation{machine(LR"(SOFTWARE\Contoso)")})};
    result.known_values = {path_change(),
                           path_change({Hive::User, kAlice, L"Environment"}, KnownSpot::UserEnvironment)};
    TraceReview review;
    set_excluded(review, result.changes[1], true);
    set_excluded(review, result.known_values[0], true);

    const TraceResult kept = apply_review(result, review);
    ASSERT_EQ(kept.changes.size(), 2u);
    EXPECT_EQ(std::get<FileLocation>(kept.changes[0].location).path, kInstall);
    EXPECT_TRUE(std::holds_alternative<RegistryKeyLocation>(kept.changes[1].location));
    ASSERT_EQ(kept.known_values.size(), 1u);
    EXPECT_EQ(kept.known_values[0].spot, KnownSpot::UserEnvironment);
    EXPECT_EQ(to_json(apply_review(result, {})), to_json(result));
}

TEST(TraceReviewTest, RoundTripsExactly) {
    TraceReview review;
    review.excluded = {FileLocation{kInstall, true},
                       RegistryValueLocation{{Hive::User, kAlice, LR"(Software\Contoso)"}, L"Name\x1F"},
                       leftovers::ServiceLocation{L"ContosoSvc", false},
                       leftovers::TaskLocation{LR"(\Contoso\Updater)", false},
                       // A name UTF-8 can't carry: a lone surrogate.
                       RegistryKeyLocation{machine(std::wstring(LR"(SOFTWARE\Odd)") + L"\xD800")}};
    review.excluded_values = {{KnownSpot::SharedDlls,
                               machine(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"),
                               LR"(C:\Program Files\Common Files\Contoso\shared.dll)"}};
    const Json json = to_json(review);
    EXPECT_EQ(json["schema_version"], kReviewSchemaVersion);
    EXPECT_EQ(json["excluded"].size(), 5u);
    EXPECT_EQ(json["excluded_values"][0]["spot"], "shared-dlls");
    EXPECT_FALSE(json["excluded_values"][0].contains("before"));

    TraceReview parsed;
    ASSERT_TRUE(from_json(Json::parse(json.dump()), parsed));
    EXPECT_EQ(to_json(parsed), json);
    EXPECT_EQ(std::get<RegistryKeyLocation>(parsed.excluded[4]).key.path,
              std::wstring(LR"(SOFTWARE\Odd)") + L"\xD800");

    // Another schema version, or a malformed file, isn't read.
    Json other = json;
    other["schema_version"] = kReviewSchemaVersion + 1;
    EXPECT_FALSE(from_json(other, parsed));
    Json broken = json;
    broken["excluded"][0] = 7;
    EXPECT_FALSE(from_json(broken, parsed));
    EXPECT_FALSE(from_json(Json::object(), parsed));
}

class TraceReviewStoreTest : public ::testing::Test {
protected:
    std::unique_ptr<TraceStore> open_store(const std::wstring& name) {
        platform::FolderStatus status{};
        auto store = TraceStore::open(fakes::test_store_options(temp_ / name), status);
        EXPECT_EQ(status, platform::FolderStatus::Ok);
        return store;
    }

    // A finished trace of the fixture's install, linked to it: its folder, an unrelated file, its
    // Uninstall key, and an entry added to the machine's Path.
    std::wstring add_trace(TraceStore& store, const TraceReview& review = {}) {
        const inventory::Program program = contoso();
        TraceInfo info;
        info.state = TraceState::Complete;
        info.description = L"Contoso Fixture";
        info.started = std::chrono::system_clock::now() - 1h;
        info.stopped = info.started + 1min;
        info.links = {make_link(program, LinkMatch::CreatedKey)};
        info.links[0].primary = true;
        TraceError error = TraceError::None;
        const auto folder = store.add_finished(info, result(), KnownValues{}, error, review);
        EXPECT_TRUE(folder) << to_string(error);
        return info.id;
    }

    static TraceResult result() {
        TraceResult result;
        result.changes = {
            change(FileLocation{kInstall, true}),
            change(FileLocation{LR"(C:\Avel.Tests\Elsewhere\x.bin)", false}),
            change(RegistryKeyLocation{
                machine(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ContosoFixture)")}),
        };
        result.known_values = {path_change()};
        return result;
    }

    fakes::TempFolder temp_;
};

TEST_F(TraceReviewStoreTest, TheStoreReadsAndWritesAFinishedTracesReview) {
    auto store = open_store(L"Traces");
    const std::wstring id = add_trace(*store);
    // None yet: an empty review, and no file.
    const auto none = store->read_review(id);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->empty());
    const std::wstring file = store->folder_of(id) + L"\\" + std::wstring(kReviewFile);
    EXPECT_EQ(::GetFileAttributesW(file.c_str()), INVALID_FILE_ATTRIBUTES);

    TraceReview review;
    set_excluded(review, result().changes[1], true);
    TraceError error = TraceError::None;
    ASSERT_TRUE(store->write_review(id, review, error)) << to_string(error);
    const auto read = store->read_review(id);
    ASSERT_TRUE(read);
    EXPECT_EQ(to_json(*read), to_json(review));
    // Replaced, not added to.
    ASSERT_TRUE(store->write_review(id, {}, error));
    EXPECT_TRUE(store->read_review(id)->empty());

    // A damaged review reads as nothing at all.
    ASSERT_TRUE(changes::write_file_durably(file, "{ not json", true));
    EXPECT_FALSE(store->read_review(id));
    EXPECT_FALSE(store->read_review(L"..\\elsewhere"));

    // Only a finished trace's, and not while another process holds it.
    TraceInfo open;
    auto folder = store->create(open, error);
    ASSERT_TRUE(folder);
    EXPECT_FALSE(store->write_review(open.id, review, error));
    EXPECT_EQ(error, TraceError::InUse);
    folder->release();
    EXPECT_FALSE(store->write_review(open.id, review, error));
    EXPECT_EQ(error, TraceError::NotFinished);
    EXPECT_FALSE(store->write_review(L"20261030-000000-0000dead", review, error));
    EXPECT_EQ(error, TraceError::NotFound);
}

TEST_F(TraceReviewStoreTest, UsingTheTraceLeavesOutWhatItsReviewExcludes) {
    auto store = open_store(L"Traces");
    TraceReview review;
    set_excluded(review, result().changes[1], true);
    set_excluded(review, result().known_values[0], true);
    const std::wstring id = add_trace(*store, review);
    const inventory::Program program = contoso();
    inventory::Inventory inventory;
    inventory.programs = {program};

    StoreTraceSource source(*store, inventory, fake_options());
    auto target = leftovers::make_scan_target(program);
    ASSERT_TRUE(source.add_trace(program, target));
    ASSERT_EQ(target.traced.size(), 2u);
    EXPECT_EQ(std::get<FileLocation>(target.traced[0].location).path, kInstall);
    EXPECT_TRUE(std::holds_alternative<RegistryKeyLocation>(target.traced[1].location));
    EXPECT_TRUE(target.traced_values.empty());
    EXPECT_TRUE(target.captured_values.empty());
    // What it excludes goes along, a known value as the registry value, so the scan can tell the
    // other scanners' matches of it.
    const auto excluded_identities = [](const leftovers::ScanTarget& scanned) {
        std::vector<std::wstring> keys;
        for (const auto& location : scanned.trace_excluded) {
            keys.push_back(leftovers::identity(location));
        }
        return keys;
    };
    const std::vector<std::wstring> expected = {
        leftovers::identity(result().changes[1].location),
        leftovers::identity(RegistryValueLocation{fakes::kMachineEnvironment, L"Path"})};
    EXPECT_EQ(excluded_identities(target), expected);

    TraceError error = TraceError::None;
    const auto forced = source.trace_target(id, error);
    ASSERT_TRUE(forced);
    EXPECT_EQ(forced->traced.size(), 2u);
    EXPECT_TRUE(forced->traced_values.empty());
    EXPECT_EQ(excluded_identities(*forced), expected);

    // Taken back in, it's offered again.
    ASSERT_TRUE(store->write_review(id, {}, error));
    auto again = leftovers::make_scan_target(program);
    ASSERT_TRUE(source.add_trace(program, again));
    EXPECT_EQ(again.traced.size(), 3u);
    EXPECT_EQ(again.traced_values.size(), 1u);
    EXPECT_TRUE(again.trace_excluded.empty());

    // A damaged review could hide what was excluded: the trace isn't used, nor what it excluded.
    ASSERT_TRUE(
        changes::write_file_durably(store->folder_of(id) + L"\\" + std::wstring(kReviewFile), "[]", true));
    auto damaged = leftovers::make_scan_target(program);
    EXPECT_FALSE(source.add_trace(program, damaged));
    EXPECT_TRUE(damaged.trace_id.empty());
    EXPECT_TRUE(damaged.trace_excluded.empty());
    EXPECT_FALSE(source.trace_target(id, error));
    EXPECT_EQ(error, TraceError::Damaged);
}

TEST_F(TraceReviewStoreTest, ExportAndImportCarryTheReviewToTheImportingUser) {
    auto there = open_store(L"There");
    // A per-user install of Alice's: a folder in her profile and a key in her hive, both excluded.
    TraceInfo info;
    info.state = TraceState::Complete;
    info.description = L"Widget";
    info.started = std::chrono::system_clock::now() - 1h;
    info.stopped = info.started + 1min;
    TraceResult result;
    result.changes = {change(FileLocation{std::wstring(kAliceProfile) + LR"(\AppData\Local\Widget)", true}),
                      change(RegistryKeyLocation{{Hive::User, kAlice, LR"(Software\Widget)"}}),
                      change(RegistryKeyLocation{machine(LR"(SOFTWARE\Widget)")})};
    result.known_values = {path_change({Hive::User, kAlice, L"Environment"}, KnownSpot::UserEnvironment)};
    TraceReview review;
    set_excluded(review, result.changes[0], true);
    set_excluded(review, result.changes[1], true);
    set_excluded(review, result.known_values[0], true);
    TraceError error = TraceError::None;
    ASSERT_TRUE(there->add_finished(info, result, KnownValues{}, error, review));

    const std::vector<platform::UserProfile> profiles = {{kAlice, L"PC\\Alice", kAliceProfile, true}};
    const auto exported = read_export(*there, info.id, profiles, error);
    ASSERT_TRUE(exported) << to_string(error);
    EXPECT_EQ(to_json(exported->review), to_json(review));
    const Json json = to_json(*exported, std::chrono::system_clock::now());
    EXPECT_EQ(json["review"]["excluded"].size(), 2u);

    TraceExport parsed;
    int version = 0;
    ASSERT_EQ(parse_export(json.dump(), parsed, version), ImportError::None);
    auto here = open_store(L"Here");
    ImportOptions options;
    options.user = {kBob, kBobProfile};
    const auto imported = import_trace(*here, std::move(parsed), inventory::Inventory{}, options, error);
    ASSERT_TRUE(imported) << to_string(error);

    // The exclusions moved to Bob along with the items.
    const auto moved = here->read_review(imported->id);
    ASSERT_TRUE(moved);
    ASSERT_EQ(moved->excluded.size(), 2u);
    EXPECT_EQ(std::get<FileLocation>(moved->excluded[0]).path,
              std::wstring(kBobProfile) + LR"(\AppData\Local\Widget)");
    EXPECT_EQ(std::get<RegistryKeyLocation>(moved->excluded[1]).key.sid, kBob);
    ASSERT_EQ(moved->excluded_values.size(), 1u);
    EXPECT_EQ(moved->excluded_values[0].key.sid, kBob);
    const auto stored = here->read_result(imported->id);
    ASSERT_TRUE(stored);
    EXPECT_TRUE(is_excluded(*moved, stored->changes[0]));
    EXPECT_TRUE(is_excluded(*moved, stored->changes[1]));
    EXPECT_FALSE(is_excluded(*moved, stored->changes[2]));
    EXPECT_TRUE(is_excluded(*moved, stored->known_values[0]));

    // An export from before the review existed imports with none; a damaged review is refused.
    Json old = json;
    old.erase("review");
    ASSERT_EQ(parse_export(old.dump(), parsed, version), ImportError::None);
    EXPECT_TRUE(parsed.review.empty());
    Json damaged = json;
    damaged["review"] = "excluded";
    EXPECT_EQ(parse_export(damaged.dump(), parsed, version), ImportError::Damaged);
}

}  // namespace
}  // namespace avel::tracker
