// The trace session against a made-up system (trace_fakes.h): registry roots in the volatile
// scratch key, a made-up journal for C:, and a store in a temporary folder. Start, an installer's
// changes, stop; resuming from a new session as another process would, after the journal wrapped;
// stopping again after a crash while stopping; damaged files; cancelling.
//
// TraceSessionLiveTest runs a real trace on this machine and needs administrator rights, so it
// skips without them; it runs in Windows Sandbox.
#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/changes/serialization.h"
#include "core/platform/elevation.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/known_values.h"
#include "core/tracker/noise_filter.h"
#include "core/tracker/trace_session.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using namespace fakes;
using changes::Json;
using platform::to_utf8;

// Items of the made-up installation.
constexpr std::uint64_t kContoso = 0x0001000000002000;
constexpr std::uint64_t kApp = 0x0001000000002001;
constexpr std::uint64_t kJunk = 0x0001000000002002;
constexpr std::uint64_t kNote = 0x0001000000002003;
constexpr std::uint64_t kLate = 0x0001000000002004;
constexpr std::uint64_t kUnpacked = 0x0001000000002005;
constexpr std::uint64_t kSetup = 0x0001000000002006;

struct SimulatedCrash {};

std::vector<std::string> sorted(std::vector<std::string> lines) {
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::wstring text_of(const platform::RawValue& value) {
    std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    return text;
}

class TraceSessionTest : public ::testing::Test {
protected:
    TraceSessionTest() : system_(scratch_, temp_.path()) {
        platform::FolderStatus status{};
        store_ = TraceStore::open(test_store_options(temp_ / L"Traces"), status);
        EXPECT_EQ(status, platform::FolderStatus::Ok);
    }

    // The system before the installer runs.
    void SetUp() override {
        ASSERT_TRUE(store_);
        scratch_.set_string(LR"(Machine\Existing)", L"Theme", L"Blue");
        scratch_.set_string(L"Environment", L"Path", L"C:\\A", REG_EXPAND_SZ);
        scratch_.set_string(L"Environment", L"Other", L"same");
    }

    std::unique_ptr<TraceSession> start(const std::wstring& description = L"Contoso") {
        StartOptions options;
        options.description = description;
        std::vector<SessionStep> steps;
        TraceError error{};
        auto session = TraceSession::start(
            *store_, options, error, [&](SessionStep step) { steps.push_back(step); }, system_.sources());
        EXPECT_EQ(error, TraceError::None) << to_string(error);
        if (session) {
            const std::vector expected{SessionStep::Volumes, SessionStep::KnownValues, SessionStep::Snapshot};
            EXPECT_EQ(steps, expected);
        }
        return session;
    }

    std::unique_ptr<TraceSession> resume(const std::wstring& id) {
        TraceError error{};
        auto session = TraceSession::resume(*store_, id, error, system_.sources());
        EXPECT_EQ(error, TraceError::None) << to_string(error);
        return session;
    }

    // What the made-up installer does: a key, a changed and an added value, the environment, a
    // folder with a file, and some noise in Temp and in Avel's own folder.
    void install() {
        next_clock_tick();
        scratch_.set_string(LR"(Machine\Contoso)", L"Version", L"1.0");
        scratch_.set_string(LR"(Machine\Existing)", L"Theme", L"Green");
        scratch_.set_string(LR"(Machine\Existing)", L"Added", L"yes");
        scratch_.set_string(L"Environment", L"Path", L"C:\\A;C:\\Contoso", REG_EXPAND_SZ);
        scratch_.set_string(L"Environment", L"CONTOSO_HOME", L"C:\\Contoso");
        system_.create(kContoso, kProgramFiles, L"Contoso", kFolder);
        system_.create(kApp, kContoso, L"app.exe");
        system_.create(kJunk, kTemp, L"junk.tmp");
        system_.create(kNote, kAvelData, L"note.txt");
    }

    std::vector<std::string> file_changes() const {
        return {
            R"(created folder C:\Program Files\Contoso [1])",
            R"(created file C:\Users\Test\AppData\Local\Temp\junk.tmp <temporary>)",
            "created file " + to_utf8(platform::normalize_path(temp_.path())) + R"(\note.txt <avel>)",
        };
    }

    std::vector<std::string> registry_changes() const {
        return {
            R"(created registry-key HKLM\SOFTWARE\Contoso [1])",
            R"(created registry-value HKLM\SOFTWARE\Existing : Added)",
            R"(modified registry-value HKLM\SOFTWARE\Existing : Theme)",
        };
    }

    std::vector<std::string> installed() const {
        auto lines = file_changes();
        const auto registry = registry_changes();
        lines.insert(lines.end(), registry.begin(), registry.end());
        return sorted(lines);
    }

    static void expect_installed_known_values(const std::vector<KnownValueChange>& values) {
        ASSERT_EQ(values.size(), 2u);
        const auto& home = values[0].name == L"CONTOSO_HOME" ? values[0] : values[1];
        const auto& path = values[0].name == L"Path" ? values[0] : values[1];
        EXPECT_EQ(home.name, L"CONTOSO_HOME");
        EXPECT_FALSE(home.before);
        ASSERT_TRUE(home.after);
        EXPECT_EQ(text_of(*home.after), L"C:\\Contoso");
        EXPECT_EQ(path.name, L"Path");
        ASSERT_TRUE(path.before && path.after);
        EXPECT_EQ(text_of(*path.before), L"C:\\A");
        EXPECT_EQ(text_of(*path.after), L"C:\\A;C:\\Contoso");
        EXPECT_EQ(path.after->type, static_cast<DWORD>(REG_EXPAND_SZ));
        EXPECT_EQ(path.spot, KnownSpot::MachineEnvironment);
    }

    TempFolder temp_;
    ScratchKey scratch_;
    FakeSystem system_;
    std::unique_ptr<TraceStore> store_;
};

TEST_F(TraceSessionTest, StartAndStopFindWhatTheInstallerChanged) {
    auto session = start();
    ASSERT_TRUE(session);
    const std::wstring id = session->info().id;
    const std::wstring folder = session->folder();
    EXPECT_EQ(session->info().state, TraceState::Open);
    EXPECT_EQ(session->info().mode, TraceMode::Manual);
    EXPECT_NE(session->info().trace_start, 0);
    for (const auto* file :
         {L"trace.json", L"registry.snapshot", L"saved_values.json", L"volume-0.checkpoint", L"lock"}) {
        EXPECT_TRUE(exists(folder + L"\\" + file)) << to_utf8(file);
    }
    const auto stored = store_->read(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->state, TraceState::Open);
    EXPECT_EQ(stored->description, L"Contoso");
    EXPECT_EQ(stored->registry.keys, 2u);  // Machine and Existing
    EXPECT_EQ(stored->registry.values, 1u);
    ASSERT_EQ(stored->volumes.size(), 1u);
    EXPECT_EQ(stored->volumes[0].root, L"C:\\");
    ASSERT_TRUE(store_->open_trace());
    EXPECT_EQ(store_->open_trace()->id, id);

    install();
    session->poll();
    std::vector<SessionStep> steps;
    const TraceResult result =
        session->stop(StopReason::Manual, [&](SessionStep step) { steps.push_back(step); });
    const std::vector expected_steps{SessionStep::Draining, SessionStep::Files, SessionStep::Registry,
                                     SessionStep::Saving};
    EXPECT_EQ(steps, expected_steps);
    EXPECT_EQ(describe(result.changes), installed());
    expect_installed_known_values(result.known_values);

    const TraceInfo& info = session->info();
    EXPECT_EQ(info.state, TraceState::Complete);
    EXPECT_EQ(info.stop_reason, StopReason::Manual);
    EXPECT_TRUE(info.stopped);
    EXPECT_EQ(info.counts.changes, 4u);
    EXPECT_EQ(info.counts.noise, 2u);
    EXPECT_TRUE(incomplete_reasons(info).empty());
    EXPECT_EQ(info.volumes[0].next_usn, 9);  // Four items created, two records each

    // Avel's own folders for the noise rules: the store's parent and the program's folder.
    const auto avel = system_.avel_paths();
    ASSERT_EQ(avel.size(), 2u);
    EXPECT_TRUE(platform::paths_equal(avel[0], temp_.path()));
    wchar_t module[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, module, MAX_PATH);
    EXPECT_TRUE(platform::paths_equal(avel[1], platform::parent_path(module)));

    // What a finished trace keeps.
    EXPECT_FALSE(exists(folder + L"\\registry.snapshot"));
    EXPECT_FALSE(exists(folder + L"\\volume-0.checkpoint"));
    EXPECT_TRUE(exists(folder + L"\\changes.json"));
    const auto saved_text = changes::read_file(folder + L"\\saved_values.json");
    ASSERT_TRUE(saved_text);
    KnownValues saved;
    ASSERT_TRUE(from_json(Json::parse(*saved_text), saved));
    ASSERT_EQ(saved.keys.size(), 1u);
    ASSERT_EQ(saved.keys[0].values.size(), 1u);  // Path as it was; "Other" didn't change
    EXPECT_EQ(saved.keys[0].values[0].name, L"Path");
    EXPECT_EQ(text_of(saved.keys[0].values[0]), L"C:\\A");

    const auto read = store_->read_result(id);
    ASSERT_TRUE(read);
    EXPECT_EQ(to_json(*read), to_json(result));
    const auto finished = store_->read(id);
    ASSERT_TRUE(finished);
    EXPECT_EQ(finished->state, TraceState::Complete);
    EXPECT_EQ(finished->counts.changes, 4u);
    EXPECT_FALSE(store_->open_trace());

    // Stopping again gives the same result.
    EXPECT_EQ(to_json(session->stop(StopReason::Manual)), to_json(result));
}

TEST_F(TraceSessionTest, ActivityCountsRecordsOutsideQuietFolders) {
    auto session = start();
    ASSERT_TRUE(session);
    EXPECT_EQ(session->activity(), 0u);
    system_.create(kJunk, kTemp, L"junk.tmp");
    system_.create(kUnpacked, kTemp, L"unpacked", kFolder);  // Created in a quiet folder: quiet too
    system_.create(kSetup, kUnpacked, L"setup.dat");
    system_.create(kContoso, kProgramFiles, L"Contoso", kFolder);
    EXPECT_EQ(session->activity(), 2u);
    EXPECT_EQ(session->activity(), 0u);
    // Records a poll read count as well.
    system_.create(kApp, kContoso, L"app.exe");
    session->poll();
    EXPECT_EQ(session->activity(), 2u);
}

TEST_F(TraceSessionTest, AnotherProcessResumesAndStops) {
    std::wstring id;
    {
        auto session = start();
        ASSERT_TRUE(session);
        id = session->info().id;
        install();
        session->poll();
        TraceError error{};
        EXPECT_FALSE(TraceSession::resume(*store_, id, error, system_.sources()));
        EXPECT_EQ(error, TraceError::InUse);
    }  // The process ends without stopping the trace
    EXPECT_FALSE(store_->held(id));
    // The journal overwrote what the first process read: only its checkpoint has it now.
    system_.wrap();
    system_.create(kLate, kProgramFiles, L"late.txt");

    auto resumed = resume(id);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->info().state, TraceState::Open);
    EXPECT_EQ(resumed->info().description, L"Contoso");
    const TraceResult result = resumed->stop(StopReason::Manual);
    auto expected = installed();
    expected.push_back(R"(created file C:\Program Files\late.txt)");
    EXPECT_EQ(describe(result.changes), sorted(expected));
    expect_installed_known_values(result.known_values);
    EXPECT_EQ(resumed->info().state, TraceState::Complete);
}

TEST_F(TraceSessionTest, DrainingInTheBackgroundKeepsCheckpoints) {
    std::wstring id;
    {
        auto session = start();
        ASSERT_TRUE(session);
        id = session->info().id;
        session->start_draining(20ms);
        install();
        const auto drains = system_.drains();
        for (int i = 0; i < 500 && system_.drains() < drains + 2; ++i) {
            std::this_thread::sleep_for(10ms);
        }
        EXPECT_GE(system_.drains(), drains + 2);
    }  // Ends the draining with the session
    system_.wrap();
    auto resumed = resume(id);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(describe(resumed->stop(StopReason::Manual).changes), installed());
    EXPECT_EQ(resumed->info().state, TraceState::Complete);
}

TEST_F(TraceSessionTest, StopRunsAgainAfterACrashWhileStopping) {
    std::wstring id;
    std::wstring folder;
    {
        auto session = start();
        ASSERT_TRUE(session);
        id = session->info().id;
        folder = session->folder();
        install();
        EXPECT_THROW(session->stop(StopReason::Manual,
                                   [](SessionStep step) {
                                       if (step == SessionStep::Files) {
                                           throw SimulatedCrash{};
                                       }
                                   }),
                     SimulatedCrash);
    }
    const auto stored = store_->read(id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->state, TraceState::Stopping);
    EXPECT_EQ(stored->stop_reason, StopReason::Manual);
    EXPECT_TRUE(stored->stopped);
    EXPECT_TRUE(exists(folder + L"\\registry.snapshot"));
    EXPECT_TRUE(exists(folder + L"\\volume-0.checkpoint"));
    ASSERT_TRUE(store_->open_trace());

    // Written after the trace stopped: not part of it.
    system_.create(kLate, kProgramFiles, L"late.txt");
    auto resumed = resume(id);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->info().state, TraceState::Stopping);
    const auto drains = system_.drains();
    const TraceResult result = resumed->stop(StopReason::InstallerDone);
    EXPECT_EQ(system_.drains(), drains);
    EXPECT_EQ(describe(result.changes), installed());
    expect_installed_known_values(result.known_values);
    EXPECT_EQ(resumed->info().state, TraceState::Complete);
    EXPECT_EQ(resumed->info().stop_reason, StopReason::Manual);  // As it was recorded
    EXPECT_FALSE(exists(folder + L"\\registry.snapshot"));
}

TEST_F(TraceSessionTest, ADamagedSnapshotMakesTheTraceIncomplete) {
    auto session = start();
    ASSERT_TRUE(session);
    install();
    write_text(session->folder() + L"\\registry.snapshot", "AVELRSN1 damaged");
    const TraceResult result = session->stop(StopReason::Manual);

    EXPECT_EQ(describe(result.changes), sorted(file_changes()));  // The files and known values still count
    expect_installed_known_values(result.known_values);
    const TraceInfo& info = session->info();
    EXPECT_EQ(info.state, TraceState::Incomplete);
    const auto reasons = incomplete_reasons(info);
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0].kind, IncompleteKind::SnapshotDamaged);
    EXPECT_EQ(to_json(info)["incomplete_reasons"], Json::parse(R"([{"registry": "snapshot-damaged"}])"));
    EXPECT_EQ(to_json(info)["complete"], false);
}

TEST_F(TraceSessionTest, AVolumeThatCantBeFollowedMakesTheTraceIncomplete) {
    auto session = start();
    ASSERT_TRUE(session);
    system_.create(kContoso, kProgramFiles, L"Contoso", kFolder);
    session->poll();
    system_.fail_next_drain(JournalStatus::Wrapped);
    system_.create(kApp, kContoso, L"app.exe");  // Lost
    const TraceResult result = session->stop(StopReason::Manual);

    EXPECT_EQ(describe(result.changes),
              std::vector<std::string>{R"(created folder C:\Program Files\Contoso)"});
    EXPECT_EQ(session->info().state, TraceState::Incomplete);
    EXPECT_EQ(to_json(session->info())["incomplete_reasons"],
              Json::parse(R"([{"volume": "C:\\", "status": "wrapped"}])"));
}

TEST_F(TraceSessionTest, ADamagedCheckpointCountsAsUnreadable) {
    std::wstring id;
    {
        auto session = start();
        ASSERT_TRUE(session);
        id = session->info().id;
        system_.create(kContoso, kProgramFiles, L"Contoso", kFolder);
        session->poll();
        write_text(session->folder() + L"\\volume-0.checkpoint", "damaged");
    }
    auto resumed = resume(id);
    ASSERT_TRUE(resumed);
    ASSERT_EQ(resumed->info().volumes.size(), 1u);
    EXPECT_EQ(resumed->info().volumes[0].status, JournalStatus::Unreadable);
    ASSERT_EQ(resumed->info().diagnostics.size(), 1u);
    EXPECT_EQ(resumed->info().diagnostics[0].code, TraceDiagnosticCode::CheckpointDamaged);
    EXPECT_EQ(resumed->info().diagnostics[0].subject, L"C:\\");

    const TraceResult result = resumed->stop(StopReason::Manual);
    EXPECT_TRUE(result.changes.empty());
    EXPECT_EQ(resumed->info().state, TraceState::Incomplete);
    EXPECT_EQ(to_json(resumed->info())["incomplete_reasons"],
              Json::parse(R"([{"volume": "C:\\", "status": "unreadable"}])"));
}

TEST_F(TraceSessionTest, OnlyOneTraceIsOpenAndAFinishedOneCantBeResumed) {
    auto first = start();
    ASSERT_TRUE(first);
    const std::wstring id = first->info().id;
    TraceError error{};
    EXPECT_FALSE(TraceSession::start(*store_, {}, error, {}, system_.sources()));
    EXPECT_EQ(error, TraceError::AlreadyOpen);
    first->stop(StopReason::Manual);
    first.reset();
    EXPECT_FALSE(TraceSession::resume(*store_, id, error, system_.sources()));
    EXPECT_EQ(error, TraceError::NotOpen);
    EXPECT_FALSE(TraceSession::resume(*store_, L"20261023-000000-00000000", error, system_.sources()));
    EXPECT_EQ(error, TraceError::NotFound);
    EXPECT_TRUE(start(L"second"));
}

TEST_F(TraceSessionTest, CancelRemovesTheTrace) {
    auto session = start();
    ASSERT_TRUE(session);
    const std::wstring folder = session->folder();
    session->start_draining(20ms);
    EXPECT_TRUE(session->cancel());
    EXPECT_FALSE(exists(folder));
    EXPECT_TRUE(store_->list().empty());
    EXPECT_FALSE(store_->open_trace());
    EXPECT_TRUE(session->stop(StopReason::Manual).changes.empty());
    session.reset();
    EXPECT_TRUE(start(L"again"));
}

// A real trace: a folder with a file in a temporary folder, a key with a value and a value in
// HKCU\Software\Avel.Tests\<guid>. Everything else the machine does meanwhile is left out of the
// comparison.
TEST(TraceSessionLiveTest, FindsWhatWasCreatedOnThisMachine) {
    if (!platform::is_process_elevated()) {
        GTEST_SKIP() << "reading the volumes' USN journals needs administrator rights";
    }
    TempFolder store_folder;
    TempFolder files;
    ScratchKey scratch;  // Exists before the trace
    platform::FolderStatus status{};
    auto store = TraceStore::open(test_store_options(store_folder / L"Traces"), status);
    ASSERT_TRUE(store) << platform::to_string(status);
    TraceError error{};
    auto session = TraceSession::start(*store, {L"live test", TraceMode::Manual, L""}, error);
    ASSERT_TRUE(session) << to_string(error);

    next_clock_tick();
    write_file(files / LR"(new\file.txt)", "traced");
    scratch.set_string(L"Contoso", L"Version", L"1.0");
    scratch.set_dword(L"", L"Added", 1);
    const TraceResult result = session->stop(StopReason::Manual);
    const TraceInfo& info = session->info();
    EXPECT_TRUE(info.state == TraceState::Complete || info.state == TraceState::Incomplete)
        << to_string(info.state);
    for (const auto& reason : incomplete_reasons(info)) {
        std::cout << "incomplete: " << to_json(reason).dump() << "\n";
    }

    const std::wstring folder = platform::long_path(files.path());
    const std::wstring sid = platform::current_user_sid();
    std::vector<std::string> lines;
    for (const auto& change : result.changes) {
        std::string where;
        if (const auto* file = std::get_if<leftovers::FileLocation>(&change.location)) {
            if (platform::path_is_inside(file->path, folder)) {
                where =
                    (file->directory ? "folder " : "file ") + to_utf8(file->path.substr(folder.size() + 1));
            }
        } else {
            const platform::RegistryAddress* key = nullptr;
            std::wstring value;
            if (const auto* registry_key = std::get_if<leftovers::RegistryKeyLocation>(&change.location)) {
                key = &registry_key->key;
            } else if (const auto* registry_value =
                           std::get_if<leftovers::RegistryValueLocation>(&change.location)) {
                key = &registry_value->key;
                value = L" : " + registry_value->value_name;
            }
            if (key && key->hive == platform::Hive::User && platform::iequals(key->sid, sid) &&
                (platform::iequals(key->path, scratch.path()) ||
                 platform::istarts_with(key->path, scratch.path() + L"\\"))) {
                const std::wstring relative = key->path.substr(scratch.path().size());
                where = (value.empty() ? "registry-key ." : "registry-value .") + to_utf8(relative + value);
            }
        }
        if (!where.empty()) {
            lines.push_back(std::string(to_string(change.change)) + " " + where +
                            (change.contents ? " [" + std::to_string(change.contents) + "]" : ""));
        }
    }
    std::sort(lines.begin(), lines.end());
    const std::vector<std::string> expected = {
        "created folder new [1]",
        R"(created registry-key .\Contoso [1])",
        "created registry-value . : Added",
    };
    EXPECT_EQ(lines, expected);
}

// The noise rules a trace stops with set aside the current user's hive files and LocalLow caches,
// which only the profile folder (from ProfileList) places.
TEST(TraceSessionLiveTest, NoiseRulesKnowTheCurrentUsersProfile) {
    std::wstring home(MAX_PATH, L'\0');
    home.resize(::GetEnvironmentVariableW(L"USERPROFILE", home.data(), MAX_PATH));
    ASSERT_FALSE(home.empty());
    home = platform::normalize_path(home);
    std::string profiles;
    for (const auto& profile : platform::enumerate_user_profiles()) {
        profiles += to_utf8(profile.sid) + " '" + to_utf8(profile.profile_path) + "'" +
                    (profile.hive_loaded ? " (signed in)" : "") + "; ";
    }
    UserHiveCache hives;
    const NoiseRules rules = live_trace_sources().noise_rules({}, hives);
    EXPECT_EQ(classify_file(rules, home + LR"(\ntuser.dat)"), NoiseReason::SystemActivity) << profiles;
    EXPECT_EQ(classify_file(rules, home + LR"(\NTUSER.DAT.LOG2)"), NoiseReason::SystemActivity) << profiles;
    EXPECT_EQ(
        classify_file(rules, home + LR"(\AppData\LocalLow\Microsoft\CryptnetUrlCache\MetaData\57C8EDB9)"),
        NoiseReason::SystemActivity)
        << profiles;
}

}  // namespace
}  // namespace avel::tracker
