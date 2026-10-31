// The trace store as the leftover scan's trace source, on a scratch store: a program's newest
// finished trace with its items tied to it (noise left out) and its values as they are now, and a
// target for one trace whether its program is installed or gone. Tying reads a table and values
// come from a fake reader: nothing here reads the live system's keys.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "core/tracker/known_values.h"
#include "core/tracker/linking.h"
#include "core/tracker/trace_source.h"
#include "trace_fakes.h"

namespace avel::tracker {
namespace {

using namespace std::chrono_literals;
using inventory::Program;
using leftovers::FileLocation;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kInstall[] = LR"(C:\Avel.Tests\Contoso Fixture)";
constexpr wchar_t kUninstall[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ContosoFixture)";

RegistryAddress machine(std::wstring path) { return {Hive::Machine, L"", std::move(path)}; }

const std::vector<leftovers::ContainedItem> kInstallInside = {{L"bin", std::nullopt, true},
                                                              {LR"(bin\app.exe)", std::nullopt, false}};

Program contoso() {
    Program program;
    program.id = L"reg:HKLM64:ContosoFixture";
    program.display_name = L"Contoso Fixture 1.0";
    program.publisher = L"Contoso Ltd";
    program.display_version = L"1.0";
    program.install_location = kInstall;
    program.registry = inventory::RegistryLocation{inventory::RegistryView::Machine64, L"ContosoFixture", {}};
    return program;
}

TraceChange change(leftovers::Location location, ChangeKind kind = ChangeKind::Created,
                   NoiseReason noise = NoiseReason::None) {
    TraceChange result;
    result.change = kind;
    result.location = std::move(location);
    result.noise = noise;
    return result;
}

platform::RawValue path_value(std::wstring_view text) {
    platform::RawValue value{L"Path", REG_EXPAND_SZ, {}};
    value.data.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

StoreTraceSourceOptions fake_options(std::vector<std::wstring>* reads = nullptr) {
    TieSources tie;
    tie.strings = [](const leftovers::Location&) { return std::vector<std::wstring>(); };
    tie.expand = [](std::wstring_view text) { return std::wstring(text); };
    tie.file_exists = [](const std::wstring&) { return false; };
    StoreTraceSourceOptions options;
    options.tie = std::move(tie);
    options.read_value = [reads](const RegistryAddress& key, const std::wstring& name) {
        if (reads) {
            reads->push_back(platform::display_path(key) + L" : " + name);
        }
        return leftovers::CapturedValue{true,
                                        path_value(L"C:\\Windows;C:\\Avel.Tests\\Contoso Fixture\\bin")};
    };
    return options;
}

class StoreTraceSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        platform::FolderStatus status{};
        store_ = TraceStore::open(fakes::test_store_options(temp_ / L"Traces"), status);
        ASSERT_TRUE(store_);
    }

    // A finished trace of the fixture's install, `age` old, linked to `linked` (or to nothing).
    std::wstring add_trace(const Program* linked, std::chrono::hours age) {
        TraceInfo info;
        info.state = TraceState::Complete;
        info.description = L"Contoso Fixture";
        info.started = std::chrono::system_clock::now() - age;
        info.stopped = info.started + 1min;
        if (linked) {
            info.links = {make_link(*linked, LinkMatch::CreatedKey)};
            info.links[0].primary = true;
        }
        TraceResult result;
        result.changes = {
            change(FileLocation{kInstall, true}),
            change(FileLocation{LR"(C:\Avel.Tests\Elsewhere\x.bin)", false}),
            change(RegistryValueLocation{machine(LR"(SOFTWARE\Contoso Shared)"), L"Theme"},
                   ChangeKind::Modified),
            change(FileLocation{LR"(C:\Windows\Prefetch\X.pf)", false}, ChangeKind::Created,
                   NoiseReason::SystemActivity),
            change(RegistryKeyLocation{machine(kUninstall)}),
        };
        result.changes[0].contents = 2;
        result.changes[0].inside = kInstallInside;
        result.known_values = {{KnownSpot::MachineEnvironment, fakes::kMachineEnvironment, L"Path",
                                path_value(L"C:\\Windows"),
                                path_value(L"C:\\Windows;C:\\Avel.Tests\\Contoso Fixture\\bin")}};
        TraceError error = TraceError::None;
        const auto folder = store_->add_finished(info, result, KnownValues{}, error);
        EXPECT_TRUE(folder);
        return info.id;
    }

    fakes::TempFolder temp_;
    std::unique_ptr<TraceStore> store_;
};

TEST_F(StoreTraceSourceTest, AddsTheProgramsNewestTraceTiedToIt) {
    const Program program = contoso();
    const std::wstring older = add_trace(&program, 48h);
    const std::wstring newer = add_trace(&program, 1h);
    inventory::Inventory inventory;
    inventory.programs = {program};
    std::vector<std::wstring> reads;
    StoreTraceSource source(*store_, inventory, fake_options(&reads));
    EXPECT_NE(older, newer);
    EXPECT_EQ(source.trace_of(program), newer);

    auto target = leftovers::make_scan_target(program);
    ASSERT_TRUE(source.add_trace(program, target));
    EXPECT_EQ(target.trace_id, newer);
    EXPECT_EQ(target.program_id, program.id);
    // Noise is left out; the rest keeps its order, change and tie.
    ASSERT_EQ(target.traced.size(), 4u);
    EXPECT_TRUE(target.traced[0].tied);
    EXPECT_EQ(target.traced[0].tie, L"install-folder");
    EXPECT_EQ(target.traced[0].category, leftovers::Category::Files);
    EXPECT_EQ(target.traced[0].change, leftovers::TracedChange::Created);
    // What a created folder held when the trace stopped, for the scan to compare with what it holds.
    EXPECT_EQ(target.traced[0].contents, 2u);
    EXPECT_EQ(target.traced[0].inside, kInstallInside);
    EXPECT_FALSE(target.traced[1].inside.has_value());
    EXPECT_FALSE(target.traced[1].tied);
    EXPECT_TRUE(target.traced[1].tie.empty());
    EXPECT_EQ(target.traced[2].change, leftovers::TracedChange::Modified);
    EXPECT_EQ(target.traced[2].category, leftovers::Category::Registry);
    EXPECT_TRUE(target.traced[3].tied);
    EXPECT_EQ(target.traced[3].tie, L"uninstall-key");
    // The values it changed, and each as it is now.
    ASSERT_EQ(target.traced_values.size(), 1u);
    EXPECT_EQ(target.traced_values[0].spot, leftovers::ValueSpot::MachineEnvironment);
    EXPECT_EQ(target.traced_values[0].name, L"Path");
    ASSERT_TRUE(target.traced_values[0].after);
    ASSERT_EQ(target.captured_values.size(), 1u);
    EXPECT_TRUE(target.captured_values[0].read);
    EXPECT_EQ(reads.size(), 1u);

    // A program without a trace keeps its target as it was.
    Program other = contoso();
    other.id = L"reg:HKLM64:Other";
    other.display_name = L"Other";
    other.registry->key_name = L"Other";
    auto plain = leftovers::make_scan_target(other);
    EXPECT_FALSE(source.add_trace(other, plain));
    EXPECT_TRUE(plain.trace_id.empty());
    EXPECT_TRUE(plain.traced.empty());
    EXPECT_FALSE(source.trace_of(other));
}

TEST_F(StoreTraceSourceTest, TargetsATraceWhoseProgramIsGoneByWhatItsLinkRecorded) {
    const Program program = contoso();
    const std::wstring id = add_trace(&program, 1h);
    const inventory::Inventory none;
    StoreTraceSource gone(*store_, none, fake_options());
    TraceError error = TraceError::None;
    const auto target = gone.trace_target(id, error);
    ASSERT_TRUE(target);
    EXPECT_EQ(error, TraceError::None);
    EXPECT_EQ(target->program_id, L"trace:" + id);
    EXPECT_EQ(target->display_name, L"Contoso Fixture 1.0");
    EXPECT_EQ(target->install_location, kInstall);
    ASSERT_TRUE(target->uninstall_key);
    EXPECT_EQ(platform::display_path(*target->uninstall_key), L"HKLM\\" + std::wstring(kUninstall));
    EXPECT_EQ(target->trace_id, id);
    ASSERT_EQ(target->traced.size(), 4u);
    EXPECT_EQ(target->traced[0].tie, L"install-folder");
    EXPECT_EQ(target->traced[3].tie, L"uninstall-key");

    // While the program is installed, its own target.
    inventory::Inventory installed;
    installed.programs = {program};
    StoreTraceSource present(*store_, installed, fake_options());
    const auto own = present.trace_target(id, error);
    ASSERT_TRUE(own);
    EXPECT_EQ(own->program_id, program.id);
    EXPECT_EQ(own->trace_id, id);
}

TEST_F(StoreTraceSourceTest, AnUnlinkedTraceIsNamedByItsDescription) {
    const std::wstring id = add_trace(nullptr, 1h);
    const inventory::Inventory none;
    StoreTraceSource source(*store_, none, fake_options());
    TraceError error = TraceError::None;
    const auto target = source.trace_target(id, error);
    ASSERT_TRUE(target);
    EXPECT_EQ(target->program_id, L"trace:" + id);
    EXPECT_EQ(target->display_name, L"Contoso Fixture");
    EXPECT_TRUE(target->install_location.empty());
    // The folder named like the description is tied by its name; the unrelated file isn't.
    EXPECT_TRUE(target->traced[0].tied);
    EXPECT_EQ(target->traced[0].tie, L"named-after-program");
    EXPECT_FALSE(target->traced[1].tied);
}

TEST_F(StoreTraceSourceTest, RefusesTracesThatDontExistOrArentFinished) {
    const inventory::Inventory none;
    StoreTraceSource source(*store_, none, fake_options());
    TraceError error = TraceError::None;
    EXPECT_FALSE(source.trace_target(L"20261025-000000-00000000", error));
    EXPECT_EQ(error, TraceError::NotFound);
    EXPECT_FALSE(source.trace_target(L"..\\elsewhere", error));
    EXPECT_EQ(error, TraceError::NotFound);

    TraceInfo info;
    auto folder = store_->create(info, error);
    ASSERT_TRUE(folder);
    info.state = TraceState::Open;
    ASSERT_TRUE(folder->write_info(info));
    EXPECT_FALSE(source.trace_target(info.id, error));
    EXPECT_EQ(error, TraceError::NotFinished);
}

// What a trace can bring, counted on its result: what add_trace gives the target, before the scan.
TEST_F(StoreTraceSourceTest, CountsWhatATraceCanOffer) {
    const Program program = contoso();
    const std::wstring id = add_trace(&program, 1h);
    const auto result = store_->read_result(id);
    ASSERT_TRUE(result);
    const TraceOffer offer = trace_offer(*result);
    EXPECT_EQ(offer.created, 3u);  // The prefetch file is noise
    EXPECT_EQ(offer.changed, 1u);
    EXPECT_EQ(offer.values, 1u);

    inventory::Inventory inventory;
    inventory.programs = {program};
    StoreTraceSource source(*store_, inventory, fake_options());
    auto target = leftovers::make_scan_target(program);
    ASSERT_TRUE(source.add_trace(program, target));
    const auto created = std::count_if(target.traced.begin(), target.traced.end(), [](const auto& item) {
        return item.change == leftovers::TracedChange::Created;
    });
    EXPECT_EQ(static_cast<std::size_t>(created), offer.created);
    EXPECT_EQ(target.traced.size() - offer.created, offer.changed);
    EXPECT_EQ(target.traced_values.size(), offer.values);
}

}  // namespace
}  // namespace avel::tracker
