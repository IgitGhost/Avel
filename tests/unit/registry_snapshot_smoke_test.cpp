// Snapshots this machine's registry, read-only: HKLM\SOFTWARE and the current user's Software and
// classes, as far as they open unelevated. Writes the snapshot to a temporary file, reads it back
// and compares the live registry with it at once, which finds only a handful of changes. Prints the
// sizes and times for the record.
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/registry_snapshot.h"
#include "file_tree.h"

namespace avel::tracker {
namespace {

using platform::Hive;

long long milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
        .count();
}

std::string describe(const TraceChange& change) {
    std::string text = std::string(to_string(change.change)) + " ";
    if (const auto* key = std::get_if<leftovers::RegistryKeyLocation>(&change.location)) {
        text += "key " + platform::to_utf8(platform::display_path(key->key));
    } else if (const auto* value = std::get_if<leftovers::RegistryValueLocation>(&change.location)) {
        text += "value " + platform::to_utf8(platform::display_path(value->key)) + " : " +
                platform::to_utf8(value->value_name);
    } else {
        text += platform::to_utf8(leftovers::identity(change.location));
    }
    return text;
}

TEST(RegistrySnapshotSmokeTest, SnapshotsAndComparesTheLiveRegistry) {
    // The roots a trace starts with, without Services and other users.
    const std::wstring sid = platform::current_user_sid();
    std::vector<SnapshotRoot> roots;
    for (auto& root : live_snapshot_roots()) {
        if (root.address.hive == Hive::Machine ? platform::iequals(root.address.path, L"SOFTWARE")
                                               : platform::iequals(root.address.sid, sid)) {
            roots.push_back(std::move(root));
        }
    }
    if (std::none_of(roots.begin(), roots.end(),
                     [](const SnapshotRoot& root) { return root.address.hive == Hive::User; })) {
        roots.push_back({{Hive::User, sid, L"Software"}, HKEY_CURRENT_USER, L"Software", nullptr});
    }

    std::vector<SnapshotDiagnostic> start_diagnostics;
    auto started = std::chrono::steady_clock::now();
    const RegistrySnapshot snapshot = take_registry_snapshot(roots, {}, start_diagnostics);
    const long long snapshot_ms = milliseconds_since(started);
    ASSERT_EQ(snapshot.roots().size(), roots.size());
    for (const auto& root : snapshot.roots()) {
        EXPECT_NE(root.key, RegistrySnapshot::kNoKey)
            << platform::to_utf8(platform::display_path(root.address));
    }

    testing::TempFolder temp;
    const std::wstring path = temp / L"registry.snapshot";
    started = std::chrono::steady_clock::now();
    ASSERT_TRUE(write_snapshot(snapshot, path));
    const long long write_ms = milliseconds_since(started);
    started = std::chrono::steady_clock::now();
    const auto read = read_snapshot(path);
    const long long read_ms = milliseconds_since(started);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->keys().size(), snapshot.keys().size());

    // At the stop the roots are opened again from what the snapshot recorded.
    UserHiveCache hives;
    const auto stop_roots = live_snapshot_roots(*read, hives);
    EXPECT_EQ(stop_roots.size(), roots.size());
    std::vector<SnapshotDiagnostic> stop_diagnostics;
    started = std::chrono::steady_clock::now();
    const auto changes = diff_registry(*read, stop_roots, {}, stop_diagnostics);
    const long long diff_ms = milliseconds_since(started);

    std::vector<SnapshotDiagnostic> ignored;
    started = std::chrono::steady_clock::now();
    const RegistrySnapshot single = take_registry_snapshot(roots, {1}, ignored);
    const long long single_ms = milliseconds_since(started);
    EXPECT_GT(single.keys().size(), snapshot.keys().size() * 9 / 10);

    const double megabytes = static_cast<double>(std::filesystem::file_size(path)) / (1024.0 * 1024.0);
    std::cout << "  " << roots.size() << " roots: " << snapshot.keys().size() << " keys, "
              << snapshot.values().size() << " values, " << snapshot.name_count() << " names; "
              << start_diagnostics.size() << " unreadable at the start\n"
              << "  snapshot " << snapshot_ms << " ms (one thread " << single_ms << " ms), file " << megabytes
              << " MB written in " << write_ms << " ms, read in " << read_ms << " ms\n"
              << "  diff " << diff_ms << " ms: " << changes.size() << " changes, " << stop_diagnostics.size()
              << " diagnostics\n";
    for (std::size_t i = 0; i < changes.size() && i < 20; ++i) {
        std::cout << "    " << describe(changes[i]) << "\n";
    }

    EXPECT_GT(snapshot.keys().size(), 1000u);
    EXPECT_LT(changes.size(), 200u);  // Only what Windows and running programs changed in a few seconds
}

}  // namespace
}  // namespace avel::tracker
