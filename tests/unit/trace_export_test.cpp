// Exporting a trace and importing it into another store: the file round trip, a single user's
// items moved to the importing user, several users' left as they are, links resolved again, and
// files that are damaged, of another format or of another version refused.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/platform/text.h"
#include "core/tracker/linking.h"
#include "core/tracker/trace_export.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using changes::Json;
using leftovers::FileLocation;
using leftovers::RegistryKeyLocation;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kAlice[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kAliceProfile[] = LR"(C:\Users\Alice)";
constexpr wchar_t kBob[] = L"S-1-5-21-9-8-7-1002";
constexpr wchar_t kBobProfile[] = LR"(D:\Profiles\Bob)";
constexpr wchar_t kUninstall[] = LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)";

std::vector<std::byte> bytes_of(std::wstring_view text) {
    std::vector<std::byte> data((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(data.data(), text.data(), text.size() * sizeof(wchar_t));
    return data;
}

std::wstring text_of(const platform::RawValue& value) {
    std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    return text;
}

class TraceExportTest : public ::testing::Test {
protected:
    std::unique_ptr<TraceStore> open_store(const std::wstring& name) {
        platform::FolderStatus status{};
        auto store =
            TraceStore::open(fakes::test_store_options(temp_ / name, [this] { return now_; }), status);
        EXPECT_EQ(status, platform::FolderStatus::Ok);
        return store;
    }

    // A finished per-user install of Widget for Alice: a folder in her profile, keys in her hive,
    // an entry added to her Path.
    std::wstring add_widget_trace(TraceStore& store, bool second_user = false) {
        TraceInfo info;
        info.state = TraceState::Complete;
        info.description = L"Widget";
        info.started = now_ - 1h;
        info.stopped = now_ - 59min;
        TraceLink link;
        link.program_id = std::wstring(L"reg:HKU\\") + kAlice + L":Widget";
        link.name = L"Widget 1.0";
        link.publisher = L"Contoso";
        link.uninstall_key = RegistryAddress{Hive::User, kAlice, std::wstring(kUninstall) + L"\\Widget"};
        link.install_location = std::wstring(kAliceProfile) + LR"(\AppData\Local\Programs\Widget)";
        link.primary = true;
        info.links = {link};

        TraceResult result;
        TraceChange folder;
        folder.location =
            FileLocation{std::wstring(kAliceProfile) + LR"(\AppData\Local\Programs\Widget)", true};
        folder.contents = 3;
        TraceChange entry;
        entry.location = RegistryKeyLocation{{Hive::User, kAlice, std::wstring(kUninstall) + L"\\Widget"}};
        TraceChange machine_key;
        machine_key.location = RegistryKeyLocation{{Hive::Machine, L"", LR"(SOFTWARE\Contoso)"}};
        TraceChange noise;
        noise.location = FileLocation{LR"(C:\Users\Carol\AppData\Local\Temp\x.tmp)", false};
        noise.noise = NoiseReason::Temporary;
        result.changes = {folder, entry, machine_key, noise};
        if (second_user) {
            TraceChange other;
            other.location = RegistryKeyLocation{{Hive::User, kBob, LR"(Software\Widget)"}};
            result.changes.push_back(other);
        }
        KnownValueChange path;
        path.spot = KnownSpot::UserEnvironment;
        path.key = {Hive::User, kAlice, L"Environment"};
        path.name = L"Path";
        path.before = platform::RawValue{L"Path", REG_EXPAND_SZ, bytes_of(L"%USERPROFILE%\\bin")};
        path.after = platform::RawValue{L"Path", REG_EXPAND_SZ,
                                        bytes_of(std::wstring(L"%USERPROFILE%\\bin;") + kAliceProfile +
                                                 LR"(\AppData\Local\Programs\Widget)")};
        result.known_values = {path};

        KnownValues saved;
        saved.keys.push_back({KnownSpot::UserEnvironment, path.key, {*path.before}, true});
        TraceError error{};
        auto added = store.add_finished(info, result, saved, error);
        EXPECT_TRUE(added) << to_string(error);
        return info.id;
    }

    std::vector<platform::UserProfile> profiles() const {
        return {{kAlice, L"PC\\Alice", kAliceProfile, true},
                {L"S-1-5-21-5-5-5-1005", L"PC\\Carol", LR"(C:\Users\Carol)", false}};
    }

    // Export from `from`, then parse the file again as an import would.
    TraceExport exported(TraceStore& from, const std::wstring& id) {
        TraceError error{};
        const auto trace = read_export(from, id, profiles(), error);
        EXPECT_TRUE(trace) << to_string(error);
        TraceExport parsed;
        int version = 0;
        EXPECT_EQ(parse_export(to_json(*trace, now_).dump(1, '\t'), parsed, version), ImportError::None);
        EXPECT_EQ(version, kExportSchemaVersion);
        return parsed;
    }

    fakes::TempFolder temp_;
    std::chrono::system_clock::time_point now_ =
        std::chrono::sys_days{std::chrono::year{2026} / 10 / 25} + 10h;
};

TEST_F(TraceExportTest, TheFileHoldsTheTraceItsLinksChangesAndSavedValues) {
    auto store = open_store(L"Here");
    const std::wstring id = add_widget_trace(*store);
    TraceError error{};
    const auto trace = read_export(*store, id, profiles(), error);
    ASSERT_TRUE(trace);
    // Alice's items, not Carol's noise.
    ASSERT_EQ(trace->users.size(), 1u);
    EXPECT_EQ(trace->users[0].sid, kAlice);
    EXPECT_EQ(trace->users[0].profile, kAliceProfile);
    EXPECT_EQ(environment_values(*trace), 2u);  // The saved Path and its change

    const Json json = Json::parse(to_json(*trace, now_).dump());
    EXPECT_EQ(json["format"], "avel-trace");
    EXPECT_EQ(json["schema_version"], 1);
    EXPECT_EQ(json["exported"], "2026-10-25T10:00:00Z");
    EXPECT_EQ(json["users"][0]["profile"], "C:\\Users\\Alice");
    EXPECT_EQ(json["trace"]["links"][0]["name"], "Widget 1.0");
    EXPECT_EQ(json["changes"]["changes"].size(), 4u);
    EXPECT_EQ(json["saved_values"]["keys"].size(), 1u);

    TraceExport parsed;
    int version = 0;
    ASSERT_EQ(parse_export(json.dump(), parsed, version), ImportError::None);
    EXPECT_EQ(to_json(parsed.info), to_json(trace->info));
    EXPECT_EQ(to_json(parsed.result), to_json(trace->result));
    EXPECT_EQ(to_json(parsed.saved_values), to_json(trace->saved_values));

    // Only finished traces.
    TraceInfo open;
    auto folder = store->create(open, error);
    ASSERT_TRUE(folder);
    EXPECT_FALSE(read_export(*store, open.id, profiles(), error));
    EXPECT_EQ(error, TraceError::NotFinished);
    EXPECT_FALSE(read_export(*store, L"20261025-000000-0000dead", profiles(), error));
    EXPECT_EQ(error, TraceError::NotFound);
}

// One user: their SID and profile folder become the importing user's everywhere, and the links are
// resolved against this machine's programs.
TEST_F(TraceExportTest, ASingleUsersItemsMoveToTheImportingUser) {
    auto there = open_store(L"There");
    const std::wstring origin = add_widget_trace(*there);
    TraceExport trace = exported(*there, origin);

    auto here = open_store(L"Here");
    inventory::Program widget;
    widget.id = std::wstring(L"reg:HKU\\") + kBob + L":Widget";
    widget.display_name = L"Widget 1.0";
    widget.publisher = L"Contoso";
    widget.scope = inventory::Scope::User;
    widget.user_sid = kBob;
    widget.registry = inventory::RegistryLocation{inventory::RegistryView::User, L"Widget", {}};
    inventory::Inventory inventory;
    inventory.programs = {widget};
    ImportOptions options;
    options.user = {kBob, kBobProfile};
    now_ += 24h;
    TraceError error{};
    const auto info = import_trace(*here, std::move(trace), inventory, options, error);
    ASSERT_TRUE(info) << to_string(error);

    EXPECT_NE(info->id, origin);
    EXPECT_EQ(info->state, TraceState::Complete);
    ASSERT_TRUE(info->imported);
    EXPECT_EQ(info->imported->time, now_);
    EXPECT_EQ(info->imported->origin_id, origin);
    EXPECT_EQ(info->imported->from_sid, kAlice);
    EXPECT_EQ(info->imported->to_profile, kBobProfile);
    ASSERT_EQ(info->links.size(), 1u);
    EXPECT_EQ(info->links[0].program_id, widget.id);
    EXPECT_EQ(info->links[0].uninstall_key->sid, kBob);
    ASSERT_TRUE(resolve_trace_program(*info, inventory));

    const auto stored = here->read(info->id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(to_json(*stored), to_json(*info));
    const auto result = here->read_result(info->id);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->changes.size(), 4u);
    EXPECT_EQ(std::get<FileLocation>(result->changes[0].location).path,
              std::wstring(kBobProfile) + LR"(\AppData\Local\Programs\Widget)");
    EXPECT_EQ(std::get<RegistryKeyLocation>(result->changes[1].location).key.sid, kBob);
    EXPECT_EQ(std::get<RegistryKeyLocation>(result->changes[2].location).key.hive, Hive::Machine);
    EXPECT_EQ(std::get<FileLocation>(result->changes[3].location).path,
              LR"(C:\Users\Carol\AppData\Local\Temp\x.tmp)");  // Not Alice's
    ASSERT_EQ(result->known_values.size(), 1u);
    EXPECT_EQ(result->known_values[0].key.sid, kBob);
    EXPECT_EQ(text_of(*result->known_values[0].before), L"%USERPROFILE%\\bin");
    EXPECT_EQ(text_of(*result->known_values[0].after),
              std::wstring(L"%USERPROFILE%\\bin;") + kBobProfile + LR"(\AppData\Local\Programs\Widget)");

    // The saved values moved along.
    const auto saved_text =
        changes::read_file(here->folder_of(info->id) + L"\\" + std::wstring(kSavedValuesFile));
    ASSERT_TRUE(saved_text);
    KnownValues saved;
    ASSERT_TRUE(from_json(Json::parse(*saved_text), saved));
    ASSERT_EQ(saved.keys.size(), 1u);
    EXPECT_EQ(saved.keys[0].key.sid, kBob);
}

// Several users: nothing is moved, and each user gets a diagnostic. The links that lead nowhere
// here stay, for retention to count from the import.
TEST_F(TraceExportTest, SeveralUsersItemsStayAsTheyAre) {
    auto there = open_store(L"There");
    const std::wstring origin = add_widget_trace(*there, true);
    TraceExport trace = exported(*there, origin);
    ASSERT_EQ(trace.users.size(), 2u);
    EXPECT_TRUE(trace.users[1].profile.empty());  // Bob has no profile there

    auto here = open_store(L"Here");
    ImportOptions options;
    options.user = {L"S-1-5-21-4-4-4-1004", LR"(C:\Users\Dave)"};
    TraceError error{};
    const auto info = import_trace(*here, std::move(trace), {}, options, error);
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->imported->from_sid.empty());
    std::vector<std::wstring> not_remapped;
    for (const auto& diagnostic : info->diagnostics) {
        if (diagnostic.code == TraceDiagnosticCode::UserNotRemapped) {
            not_remapped.push_back(diagnostic.subject);
        }
    }
    EXPECT_EQ(not_remapped, (std::vector<std::wstring>{kAlice, kBob}));
    EXPECT_EQ(info->state, TraceState::Complete);  // Still complete
    ASSERT_EQ(info->links.size(), 1u);             // Kept, though nothing here matches it
    EXPECT_FALSE(info->unresolved_since);
    const auto result = here->read_result(info->id);
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<RegistryKeyLocation>(result->changes[1].location).key.sid, kAlice);
    EXPECT_EQ(std::get<FileLocation>(result->changes[0].location).path,
              std::wstring(kAliceProfile) + LR"(\AppData\Local\Programs\Widget)");

    // Imported again elsewhere, the earlier import's diagnostics go.
    TraceError again{};
    TraceExport second = exported(*here, info->id);
    const auto reimported = import_trace(*here, std::move(second), {}, options, again);
    ASSERT_TRUE(reimported);
    EXPECT_EQ(std::count_if(reimported->diagnostics.begin(), reimported->diagnostics.end(),
                            [](const TraceDiagnostic& diagnostic) {
                                return diagnostic.code == TraceDiagnosticCode::UserNotRemapped;
                            }),
              2);
    EXPECT_EQ(reimported->imported->origin_id, info->id);
}

// A path that only begins like the profile folder isn't in it.
TEST_F(TraceExportTest, RemappingKeepsToWholeNames) {
    TraceExport trace;
    TraceChange inside;
    inside.location = FileLocation{LR"(C:\Users\Al\file.txt)", false};
    TraceChange longer;
    longer.location = FileLocation{LR"(C:\Users\Alice\file.txt)", false};
    trace.result.changes = {inside, longer};
    KnownValueChange path;
    path.spot = KnownSpot::UserEnvironment;
    path.key = {Hive::User, L"S-1-5-21-1", L"Environment"};
    path.after =
        platform::RawValue{L"Path", REG_SZ, bytes_of(LR"(C:\Users\Al;C:\Users\Alice\bin;"C:\Users\Al\x")")};
    trace.result.known_values = {path};
    TraceLink link;
    link.program_id = L"reg:HKU\\S-1-5-21-1:Tool";
    TraceLink other;
    other.program_id = L"reg:HKU\\S-1-5-21-10:Tool";
    trace.info.links = {link, other};
    remap_user(trace, {L"S-1-5-21-1", LR"(C:\Users\Al)"}, {L"S-1-5-21-2", LR"(C:\Users\Bea)"});
    EXPECT_EQ(std::get<FileLocation>(trace.result.changes[0].location).path, LR"(C:\Users\Bea\file.txt)");
    EXPECT_EQ(std::get<FileLocation>(trace.result.changes[1].location).path, LR"(C:\Users\Alice\file.txt)");
    EXPECT_EQ(text_of(*trace.result.known_values[0].after),
              LR"(C:\Users\Bea;C:\Users\Alice\bin;"C:\Users\Bea\x")");
    EXPECT_EQ(trace.result.known_values[0].key.sid, L"S-1-5-21-2");
    EXPECT_EQ(trace.info.links[0].program_id, L"reg:HKU\\S-1-5-21-2:Tool");
    EXPECT_EQ(trace.info.links[1].program_id, L"reg:HKU\\S-1-5-21-10:Tool");
}

TEST_F(TraceExportTest, RefusesDamagedFilesAndOtherFormatsOrVersions) {
    auto store = open_store(L"Here");
    const std::wstring id = add_widget_trace(*store);
    TraceError error{};
    const auto trace = read_export(*store, id, profiles(), error);
    ASSERT_TRUE(trace);
    const Json good = Json::parse(to_json(*trace, now_).dump());
    TraceExport parsed;
    int version = 0;

    const auto parse = [&](const std::string& text) { return parse_export(text, parsed, version); };
    EXPECT_EQ(parse(good.dump()), ImportError::None);
    EXPECT_EQ(parse(""), ImportError::Damaged);
    EXPECT_EQ(parse(good.dump().substr(0, good.dump().size() / 2)), ImportError::Damaged);  // Cut short
    EXPECT_EQ(parse("[1, 2, 3]"), ImportError::OtherFormat);
    EXPECT_EQ(parse(R"({"format": "something-else", "schema_version": 1})"), ImportError::OtherFormat);
    Json bad = good;
    bad.erase("format");
    EXPECT_EQ(parse(bad.dump()), ImportError::OtherFormat);

    bad = good;
    bad["schema_version"] = 2;
    EXPECT_EQ(parse(bad.dump()), ImportError::OtherVersion);
    EXPECT_EQ(version, 2);
    bad["schema_version"] = "1";
    EXPECT_EQ(parse(bad.dump()), ImportError::Damaged);

    for (const char* part : {"trace", "changes", "saved_values"}) {
        bad = good;
        bad.erase(part);
        EXPECT_EQ(parse(bad.dump()), ImportError::Damaged) << part;
    }
    bad = good;
    bad["trace"]["state"] = "open";
    EXPECT_EQ(parse(bad.dump()), ImportError::NotFinished);
    bad = good;
    bad["changes"]["changes"][0]["change"] = "made";
    EXPECT_EQ(parse(bad.dump()), ImportError::Damaged);
    bad = good;
    bad["users"] = Json::array({Json::object()});
    EXPECT_EQ(parse(bad.dump()), ImportError::Damaged);

    // Without a list of users, they are known by their SIDs alone.
    bad = good;
    bad.erase("users");
    ASSERT_EQ(parse(bad.dump()), ImportError::None);
    ASSERT_EQ(parsed.users.size(), 1u);
    EXPECT_EQ(parsed.users[0].sid, kAlice);
    EXPECT_TRUE(parsed.users[0].profile.empty());

    EXPECT_EQ(to_string(ImportError::OtherVersion), "other-version");
}

}  // namespace
}  // namespace avel::tracker
