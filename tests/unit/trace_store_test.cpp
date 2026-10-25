// The trace store in a temporary folder: creating, listing and reading traces, one open trace at
// a time, lock files that keep a trace from being removed, purging what died while starting, and
// trace.json and changes.json read back exactly.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/changes/serialization.h"
#include "core/platform/record_folders.h"
#include "core/tracker/known_values.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_store.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using changes::Json;
using fakes::exists;
using fakes::write_text;

std::vector<std::byte> bytes_of(std::wstring_view text) {
    std::vector<std::byte> data(text.size() * sizeof(wchar_t));
    std::memcpy(data.data(), text.data(), data.size());
    return data;
}

// JSON written, then parsed again as it would be from the file.
Json reparsed(const Json& json) { return Json::parse(json.dump(1, '\t')); }

class TraceStoreTest : public ::testing::Test {
protected:
    std::unique_ptr<TraceStore> open_store() {
        platform::FolderStatus status{};
        auto store = TraceStore::open(fakes::test_store_options(root(), [this] { return now_; }), status);
        EXPECT_EQ(status, platform::FolderStatus::Ok);
        return store;
    }

    std::wstring root() const { return temp_ / L"Traces"; }

    std::unique_ptr<TraceFolder> create(TraceStore& store, const std::wstring& description) {
        TraceInfo info;
        info.description = description;
        TraceError error{};
        auto folder = store.create(info, error);
        EXPECT_EQ(error, TraceError::None);
        return folder;
    }

    static void set_state(const TraceFolder& folder, TraceState state) {
        auto info = folder.read_info();
        ASSERT_TRUE(info);
        info->state = state;
        ASSERT_TRUE(folder.write_info(*info));
    }

    fakes::TempFolder temp_;
    std::chrono::system_clock::time_point now_ =
        std::chrono::sys_days{std::chrono::year{2026} / 10 / 23} + 12h;
};

TEST_F(TraceStoreTest, CreatesListsAndReadsTraces) {
    auto store = open_store();
    ASSERT_TRUE(store);
    auto first = create(*store, L"7-Zip");
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->id().starts_with(L"20261023-120000-"));
    EXPECT_TRUE(exists(first->file(kTraceInfoFile)));
    EXPECT_TRUE(exists(first->file(L"lock")));
    const auto read = store->read(first->id());
    ASSERT_TRUE(read);
    EXPECT_EQ(read->state, TraceState::Starting);
    EXPECT_EQ(read->description, L"7-Zip");
    EXPECT_EQ(read->started, now_);
    set_state(*first, TraceState::Complete);

    now_ += 1h;
    auto second = create(*store, L"Notepad++");
    ASSERT_TRUE(second);
    set_state(*second, TraceState::Open);

    const auto traces = store->list();
    ASSERT_EQ(traces.size(), 2u);
    EXPECT_EQ(traces[0].id, second->id());
    EXPECT_EQ(traces[1].id, first->id());
    EXPECT_EQ(traces[1].state, TraceState::Complete);
    const auto open = store->open_trace();
    ASSERT_TRUE(open);
    EXPECT_EQ(open->id, second->id());
    EXPECT_FALSE(store->read_result(first->id()));  // No changes.json yet
    EXPECT_FALSE(store->read(L"..\\elsewhere"));
}

TEST_F(TraceStoreTest, OnlyOneTraceIsOpenAtATime) {
    auto store = open_store();
    auto first = create(*store, L"first");
    ASSERT_TRUE(first);
    TraceInfo info;
    TraceError error{};
    for (const auto state : {TraceState::Starting, TraceState::Open, TraceState::Stopping}) {
        set_state(*first, state);
        EXPECT_FALSE(store->create(info, error)) << to_string(state);
        EXPECT_EQ(error, TraceError::AlreadyOpen) << to_string(state);
    }
    // An open trace nobody drives still counts.
    set_state(*first, TraceState::Open);
    first->release();
    EXPECT_FALSE(store->create(info, error));
    EXPECT_EQ(error, TraceError::AlreadyOpen);

    set_state(*first, TraceState::Incomplete);
    auto second = store->create(info, error);
    ASSERT_TRUE(second);
    EXPECT_EQ(error, TraceError::None);
    EXPECT_EQ(info.id, second->id());
    EXPECT_EQ(info.state, TraceState::Starting);
}

TEST_F(TraceStoreTest, ATraceThatDiedWhileStartingIsPurged) {
    auto store = open_store();
    std::wstring id;
    {
        auto folder = create(*store, L"crashed");
        ASSERT_TRUE(folder);
        id = folder->id();
        // The process holds it while starting: neither purged nor replaced.
        EXPECT_EQ(store->purge(), 0u);
        TraceInfo info;
        TraceError error{};
        EXPECT_FALSE(store->create(info, error));
        EXPECT_EQ(error, TraceError::AlreadyOpen);
    }  // The process dies
    ASSERT_TRUE(store->open_trace());
    EXPECT_EQ(store->open_trace()->state, TraceState::Starting);

    // A trace folder whose trace.json is unreadable is damaged too; other folders stay.
    const std::wstring garbage = store->folder_of(L"20261023-110000-0badf00d");
    const std::wstring newer = store->folder_of(L"20261023-110000-00c0ffee");
    const std::wstring other = store->folder_of(L"not-a-trace");
    for (const auto& folder : {garbage, newer, other}) {
        ASSERT_TRUE(::CreateDirectoryW(folder.c_str(), nullptr));
    }
    write_text(garbage + L"\\trace.json", "{\"schema_version\": 1, \"id\": ");
    write_text(newer + L"\\trace.json", R"({"schema_version": 99, "id": "20261023-110000-00c0ffee"})");
    write_text(other + L"\\trace.json", "garbage");

    EXPECT_EQ(store->purge(), 2u);
    EXPECT_FALSE(exists(store->folder_of(id)));
    EXPECT_FALSE(exists(garbage));
    EXPECT_TRUE(exists(newer));  // Another version's
    EXPECT_TRUE(exists(other));
    EXPECT_FALSE(store->open_trace());
}

TEST_F(TraceStoreTest, CreatingReplacesATraceThatDiedWhileStarting) {
    auto store = open_store();
    std::wstring id;
    {
        auto folder = create(*store, L"crashed");
        ASSERT_TRUE(folder);
        id = folder->id();
    }
    now_ += 1min;
    auto next = create(*store, L"next");
    ASSERT_TRUE(next);
    EXPECT_FALSE(exists(store->folder_of(id)));
}

TEST_F(TraceStoreTest, AHeldTraceIsNeitherOpenedNorRemoved) {
    auto store = open_store();
    auto folder = create(*store, L"held");
    ASSERT_TRUE(folder);
    const std::wstring id = folder->id();
    set_state(*folder, TraceState::Complete);
    TraceError error{};
    EXPECT_TRUE(store->held(id));
    EXPECT_FALSE(store->open_folder(id, error));
    EXPECT_EQ(error, TraceError::InUse);
    EXPECT_FALSE(store->remove(id, error));
    EXPECT_EQ(error, TraceError::InUse);

    // Another process holds its lock file instead.
    folder->release();
    {
        const auto other = platform::take_folder_lock(store->folder_of(id));
        ASSERT_TRUE(other);
        EXPECT_TRUE(store->held(id));
        EXPECT_FALSE(store->remove(id, error));
        EXPECT_EQ(error, TraceError::InUse);
        EXPECT_EQ(store->purge([](const TraceInfo&) { return false; }), 0u);
    }
    EXPECT_FALSE(store->held(id));
    EXPECT_TRUE(store->remove(id, error));
    EXPECT_EQ(error, TraceError::None);
    EXPECT_FALSE(exists(store->folder_of(id)));
    EXPECT_FALSE(store->remove(id, error));
    EXPECT_EQ(error, TraceError::NotFound);
}

TEST_F(TraceStoreTest, PurgeKeepsFinishedTracesUnlessToldOtherwise) {
    auto store = open_store();
    std::wstring finished;
    std::wstring open;
    {
        auto folder = create(*store, L"finished");
        finished = folder->id();
        set_state(*folder, TraceState::Complete);
    }
    now_ += 1h;
    {
        auto folder = create(*store, L"open");
        open = folder->id();
        set_state(*folder, TraceState::Open);
    }
    EXPECT_EQ(store->purge(), 0u);  // No retention without links (5b)
    EXPECT_EQ(store->list().size(), 2u);
    // Even a `keep` that wants everything gone leaves the open trace alone.
    EXPECT_EQ(store->purge([](const TraceInfo&) { return false; }), 1u);
    const auto traces = store->list();
    ASSERT_EQ(traces.size(), 1u);
    EXPECT_EQ(traces[0].id, open);
}

TEST_F(TraceStoreTest, PurgeTidiesAFinishedTraceItsProcessLeft) {
    auto store = open_store();
    auto folder = create(*store, L"died after finishing");
    ASSERT_TRUE(folder);
    const platform::RawValue path_before{L"Path", REG_EXPAND_SZ, bytes_of(L"C:\\A")};
    const platform::RawValue other{L"Other", REG_SZ, bytes_of(L"same")};
    KnownValues saved;
    saved.keys.push_back(
        {KnownSpot::MachineEnvironment, fakes::kMachineEnvironment, {other, path_before}, true});
    write_text(folder->file(kSavedValuesFile), to_json(saved).dump());
    TraceResult result;
    KnownValueChange change;
    change.spot = KnownSpot::MachineEnvironment;
    change.key = fakes::kMachineEnvironment;
    change.name = L"PATH";  // Names compare ignoring case
    change.before = path_before;
    change.after = platform::RawValue{L"PATH", REG_EXPAND_SZ, bytes_of(L"C:\\A;C:\\B")};
    result.known_values.push_back(change);
    write_text(folder->file(kChangesFile), to_json(result).dump());
    write_text(folder->file(kSnapshotFile), "snapshot");
    write_text(folder->file(checkpoint_file(0)), "checkpoint");
    write_text(folder->file(checkpoint_file(1)), "checkpoint");
    set_state(*folder, TraceState::Complete);
    folder->release();

    EXPECT_EQ(store->purge(), 0u);
    EXPECT_FALSE(exists(folder->file(kSnapshotFile)));
    EXPECT_FALSE(exists(folder->file(checkpoint_file(0))));
    EXPECT_FALSE(exists(folder->file(checkpoint_file(1))));
    const auto text = changes::read_file(folder->file(kSavedValuesFile));
    ASSERT_TRUE(text);
    KnownValues kept;
    ASSERT_TRUE(from_json(Json::parse(*text), kept));
    ASSERT_EQ(kept.keys.size(), 1u);
    ASSERT_EQ(kept.keys[0].values.size(), 1u);
    EXPECT_EQ(kept.keys[0].values[0].name, L"Path");
    EXPECT_EQ(kept.keys[0].values[0].data, path_before.data);
    ASSERT_TRUE(store->read_result(folder->id()));
}

TEST(TraceRecordTest, TraceInfoRoundTripsExactly) {
    TraceInfo info;
    info.id = L"20261023-210501-1a2b3c4d";
    info.state = TraceState::Incomplete;
    info.mode = TraceMode::Launch;
    info.description = std::wstring(L"Odd \xD800 name");  // An unpaired surrogate
    info.command = LR"(C:\Installers\7z.msi /qn)";
    info.started = std::chrono::sys_days{std::chrono::year{2026} / 10 / 23} + 21h + 5min + 1s;
    info.stopped = info.started + 39s;
    info.trace_start = 134058123010000000;
    info.stop_reason = StopReason::InstallerDone;
    VolumeTrace c;
    c.root = L"C:\\";
    c.guid_path = LR"(\\?\Volume{11111111-2222-3333-4444-555555555555}\)";
    c.serial = 0xDEADBEEF;
    c.root_frn = 0x0005000000000005;
    c.journal_id = 0x01DB0123456789AB;
    c.start_usn = 1000;
    c.next_usn = 5000;
    VolumeTrace d = c;
    d.root = L"D:\\";
    d.status = JournalStatus::Replaced;
    d.error = ERROR_JOURNAL_NOT_ACTIVE;
    d.created_journal = true;
    info.volumes = {c, d};
    info.registry = {312004, 801233, 1840, 410, {}};
    info.registry.diagnostics.push_back(
        {SnapshotDiagnosticCode::KeyUnreadable,
         {platform::Hive::User, L"S-1-5-21-1", std::wstring(L"Software\\\xDC00")},
         5});
    WatchResult watch;
    watch.end = WatchEnd::InstallerDone;
    watch.exit_code = 0;
    watch.left_running = {{4242, LR"(C:\Program Files\X\x.exe)", false}};
    info.watch = watch;
    info.counts = {12, 85};
    info.diagnostics.push_back({TraceDiagnosticCode::CheckpointDamaged, L"E:\\"});
    info.diagnostics.push_back({TraceDiagnosticCode::SnapshotDamaged, L""});

    const Json json = reparsed(to_json(info));
    // The names docs/trace.md promises.
    EXPECT_EQ(json["state"], "incomplete");
    EXPECT_EQ(json["complete"], false);
    EXPECT_EQ(json["mode"], "launch");
    EXPECT_EQ(json["command"], R"(C:\Installers\7z.msi /qn)");
    EXPECT_EQ(json["started"], "2026-10-23T21:05:01Z");
    EXPECT_EQ(json["stopped"], "2026-10-23T21:05:40Z");
    EXPECT_EQ(json["stop_reason"], "installer-done");
    EXPECT_EQ(json["volumes"][0]["root"], "C:\\");
    EXPECT_EQ(json["volumes"][0]["status"], "ok");
    EXPECT_EQ(json["volumes"][1]["created_journal"], true);
    EXPECT_EQ(json["registry"]["keys"], 312004);
    EXPECT_EQ(json["registry"]["snapshot_ms"], 1840);
    EXPECT_EQ(json["registry"]["diff_ms"], 410);
    EXPECT_EQ(json["watch"]["end"], "installer-done");
    EXPECT_EQ(json["watch"]["left_running"][0]["pid"], 4242);
    EXPECT_EQ(json["watch"]["left_running"][0]["image"], R"(C:\Program Files\X\x.exe)");
    EXPECT_EQ(json["counts"]["changes"], 12);
    EXPECT_EQ(json["counts"]["noise"], 85);
    ASSERT_EQ(json["incomplete_reasons"].size(), 3u);
    EXPECT_EQ(json["incomplete_reasons"][0], Json::parse(R"({"volume": "D:\\", "status": "replaced"})"));
    EXPECT_EQ(json["incomplete_reasons"][1]["registry"], "key-unreadable");
    EXPECT_EQ(json["incomplete_reasons"][1]["error"], 5);
    EXPECT_EQ(json["incomplete_reasons"][2], Json::parse(R"({"registry": "snapshot-damaged"})"));
    EXPECT_TRUE(json.contains("description_hex"));

    TraceInfo read;
    ASSERT_TRUE(from_json(json, read));
    EXPECT_EQ(read.description, info.description);
    EXPECT_EQ(read.registry.diagnostics[0].key.path, info.registry.diagnostics[0].key.path);
    EXPECT_EQ(read.volumes[1].journal_id, c.journal_id);
    EXPECT_EQ(read.stopped, info.stopped);
    EXPECT_EQ(to_json(read), to_json(info));

    // Still open: no stop time or reason.
    TraceInfo open;
    open.id = L"20261023-210501-00000001";
    open.state = TraceState::Open;
    const Json open_json = reparsed(to_json(open));
    EXPECT_TRUE(open_json["stopped"].is_null());
    EXPECT_TRUE(open_json["stop_reason"].is_null());
    EXPECT_FALSE(open_json.contains("watch"));
    TraceInfo open_read;
    ASSERT_TRUE(from_json(open_json, open_read));
    EXPECT_FALSE(open_read.stopped);
    EXPECT_EQ(open_read.stop_reason, StopReason::None);
    EXPECT_FALSE(open_read.watch);

    Json other_version = json;
    other_version["schema_version"] = 2;
    EXPECT_FALSE(from_json(other_version, read));
}

TEST(TraceRecordTest, TraceResultRoundTripsExactly) {
    using namespace leftovers;
    TraceResult result;
    TraceChange folder;
    folder.location = FileLocation{LR"(C:\Program Files\Contoso)", true};
    folder.contents = 104;
    TraceChange odd;
    odd.location = FileLocation{std::wstring(LR"(C:\Data\odd )") + L'\xD800' + L".txt", false};
    odd.noise = NoiseReason::Temporary;
    TraceChange renamed;
    renamed.change = ChangeKind::Renamed;
    renamed.location = FileLocation{LR"(C:\x\new.txt)", false};
    renamed.old_path = LR"(C:\x\old.txt)";
    TraceChange replaced;
    replaced.change = ChangeKind::Modified;
    replaced.location = FileLocation{LR"(C:\x\app.dll)", false};
    replaced.replaced = true;
    TraceChange value;
    value.change = ChangeKind::Modified;
    value.location =
        RegistryValueLocation{{platform::Hive::Machine, L"", LR"(SOFTWARE\X)"}, std::wstring(L"Th\xDFFFme")};
    TraceChange empty_key;
    empty_key.location = RegistryKeyLocation{{platform::Hive::User, L"S-1-5-21-1", LR"(Software\Contoso)"}};
    TraceChange service;
    service.location = ServiceLocation{L"ContosoSvc", true};
    TraceChange unresolved;
    unresolved.change = ChangeKind::Deleted;
    unresolved.location = FileLocation{LR"(C:\?\gone.txt)", false};
    unresolved.noise = NoiseReason::Unresolved;
    result.changes = {folder, odd, renamed, replaced, value, empty_key, service, unresolved};

    KnownValueChange added;
    added.spot = KnownSpot::UserEnvironment;
    added.key = {platform::Hive::User, L"S-1-5-21-1", L"Environment"};
    added.name = L"CONTOSO_HOME";
    added.after = platform::RawValue{L"CONTOSO_HOME", REG_SZ, bytes_of(std::wstring(L"C:\\Contoso\0", 11))};
    KnownValueChange raw;
    raw.spot = KnownSpot::SharedDlls;
    raw.key = {platform::Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"};
    raw.name = LR"(C:\Common\shared.dll)";
    raw.before =
        platform::RawValue{raw.name, REG_DWORD, {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}};
    raw.after = platform::RawValue{raw.name, REG_BINARY, {std::byte{0xFF}, std::byte{0x00}, std::byte{0x80}}};
    result.known_values = {added, raw};

    const Json json = reparsed(to_json(result));
    EXPECT_EQ(json["changes"][0]["change"], "created");
    EXPECT_EQ(json["changes"][0]["location"]["kind"], "folder");
    EXPECT_EQ(json["changes"][0]["contents"], 104);
    EXPECT_EQ(json["changes"][0]["noise"], "none");
    EXPECT_EQ(json["changes"][2]["old_path"], R"(C:\x\old.txt)");
    EXPECT_EQ(json["changes"][3]["replaced"], true);
    EXPECT_EQ(json["changes"][5]["contents"], 0);  // A created key, even an empty one
    EXPECT_FALSE(json["changes"][1].contains("contents"));
    EXPECT_EQ(json["known_values"][1]["after"]["data"], "ff0080");

    TraceResult read;
    ASSERT_TRUE(from_json(json, read));
    ASSERT_EQ(read.changes.size(), result.changes.size());
    for (std::size_t i = 0; i < result.changes.size(); ++i) {
        EXPECT_EQ(leftovers::identity(read.changes[i].location),
                  leftovers::identity(result.changes[i].location))
            << i;
        EXPECT_EQ(read.changes[i].change, result.changes[i].change) << i;
        EXPECT_EQ(read.changes[i].old_path, result.changes[i].old_path) << i;
        EXPECT_EQ(read.changes[i].contents, result.changes[i].contents) << i;
        EXPECT_EQ(read.changes[i].replaced, result.changes[i].replaced) << i;
        EXPECT_EQ(read.changes[i].noise, result.changes[i].noise) << i;
    }
    EXPECT_EQ(std::get<FileLocation>(read.changes[1].location).path,
              std::get<FileLocation>(odd.location).path);
    EXPECT_EQ(std::get<RegistryValueLocation>(read.changes[4].location).value_name, L"Th\xDFFFme");
    ASSERT_EQ(read.known_values.size(), 2u);
    EXPECT_FALSE(read.known_values[0].before);
    EXPECT_EQ(read.known_values[0].after->data, added.after->data);
    EXPECT_EQ(read.known_values[1].before->type, static_cast<DWORD>(REG_DWORD));
    EXPECT_EQ(read.known_values[1].after->data, raw.after->data);
    EXPECT_EQ(to_json(read), to_json(result));

    EXPECT_FALSE(from_json(Json::parse(R"({"changes": [{"change": "made"}], "known_values": []})"), read));
    EXPECT_FALSE(from_json(Json::parse(R"({"changes": []})"), read));
}

}  // namespace
}  // namespace avel::tracker
