// Avel Trace's registry snapshot in the volatile scratch key: what the diff reports for created,
// deleted and modified keys and values, links, odd names, the snapshot file, threads, and the
// places reported as services, tasks and firewall rules. The roots read the scratch key but report
// their keys under the addresses they stand in for (HKLM\SOFTWARE, ...), which the rules follow.
#include <gtest/gtest.h>

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/platform/elevation.h"
#include "core/platform/nt_registry.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/registry_snapshot.h"
#include "file_tree.h"
#include "scratch_registry.h"

namespace avel::tracker {
namespace {

using platform::Hive;
using platform::RegistryAddress;

const RegistryAddress kSoftware{Hive::Machine, L"", L"SOFTWARE"};
const RegistryAddress kServices{Hive::Machine, L"", LR"(SYSTEM\CurrentControlSet\Services)"};
const RegistryAddress kUserSoftware{Hive::User, L"S-1-5-21-1-2-3-1001", L"Software"};
const RegistryAddress kUserClasses{Hive::User, L"S-1-5-21-1-2-3-1001", LR"(Software\Classes)"};

// A root that reads `subpath` of the scratch key (creating it) and reports its keys at `address`.
SnapshotRoot root_at(const testing::ScratchKey& scratch, std::wstring_view subpath,
                     const RegistryAddress& address) {
    scratch.create(subpath);
    return {address, scratch.get(), std::wstring(subpath), nullptr};
}

std::wstring describe(const SnapshotDiagnostic& diagnostic) {
    return platform::to_utf16(std::string(to_string(diagnostic.code))) + L" " +
           platform::display_path(diagnostic.key) + L" (" + std::to_wstring(diagnostic.error) + L")";
}

std::vector<std::wstring> describe(const std::vector<SnapshotDiagnostic>& diagnostics) {
    std::vector<std::wstring> lines;
    for (const auto& diagnostic : diagnostics) {
        lines.push_back(describe(diagnostic));
    }
    return lines;
}

// "created key HKLM\SOFTWARE\New [3]": the change, the item, and what a created key holds.
std::wstring describe(const TraceChange& change) {
    std::wstring text = platform::to_utf16(std::string(to_string(change.change))) + L" ";
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                text += L"key " + platform::display_path(item.key);
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                text += L"value " + platform::display_path(item.key) + L" : " + item.value_name;
            } else if constexpr (std::is_same_v<T, leftovers::ServiceLocation>) {
                text += L"service " + item.name + (item.driver ? L" (driver)" : L"");
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                text += (item.folder ? L"task folder " : L"task ") + item.path;
            } else if constexpr (std::is_same_v<T, leftovers::FirewallRuleLocation>) {
                text += L"firewall " + item.rule_id;
            } else {
                text += L"other";
            }
        },
        change.location);
    if (change.contents > 0) {
        text += L" [" + std::to_wstring(change.contents) + L"]";
    }
    return text;
}

RegistrySnapshot snapshot_of(std::span<const SnapshotRoot> roots, unsigned threads = 0) {
    std::vector<SnapshotDiagnostic> diagnostics;
    RegistrySnapshot snapshot = take_registry_snapshot(roots, {threads}, diagnostics);
    EXPECT_EQ(describe(diagnostics), std::vector<std::wstring>());
    return snapshot;
}

// The changes in the order diff_registry gives them.
std::vector<std::wstring> changes_of(const RegistrySnapshot& snapshot, std::span<const SnapshotRoot> roots,
                                     unsigned threads = 0) {
    std::vector<SnapshotDiagnostic> diagnostics;
    const auto changes = diff_registry(snapshot, roots, {threads}, diagnostics);
    EXPECT_EQ(describe(diagnostics), std::vector<std::wstring>());
    std::vector<std::wstring> lines;
    for (const auto& change : changes) {
        lines.push_back(describe(change));
    }
    return lines;
}

std::vector<std::wstring> sorted(std::vector<std::wstring> lines) {
    std::sort(lines.begin(), lines.end());
    return lines;
}

// LastWriteTime comes from the system clock, which advances in ticks of up to 16 ms. A change made
// in the tick the snapshot read its key in would keep the key's time (real traces start long before
// the installer writes), so the tests let the clock move first.
void next_clock_tick() { ::Sleep(32); }

std::int64_t last_write(HKEY key) {
    platform::KeyInfo info;
    EXPECT_EQ(platform::query_key_info(key, info), ERROR_SUCCESS);
    return static_cast<std::int64_t>(std::uint64_t{info.last_write.dwHighDateTime} << 32 |
                                     info.last_write.dwLowDateTime);
}

void set_raw(const testing::ScratchKey& scratch, std::wstring_view subpath, const std::wstring& name,
             DWORD type, std::wstring_view text) {
    std::vector<std::byte> data(text.size() * sizeof(wchar_t));
    std::memcpy(data.data(), text.data(), data.size());
    EXPECT_EQ(platform::set_raw_value(scratch.create(subpath).get(), {name, type, data}), ERROR_SUCCESS);
}

void delete_value(const testing::ScratchKey& scratch, std::wstring_view subpath, const std::wstring& name) {
    EXPECT_EQ(::RegDeleteValueW(scratch.create(subpath).get(), name.c_str()), ERROR_SUCCESS);
}

void delete_tree(const testing::ScratchKey& scratch, const std::wstring& subpath) {
    EXPECT_EQ(::RegDeleteTreeW(scratch.get(), subpath.c_str()), ERROR_SUCCESS);
}

std::string file_bytes(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const SnapKey* child_named(const RegistrySnapshot& snapshot, const SnapKey& parent, std::wstring_view name) {
    for (const SnapKey& child : snapshot.children(parent)) {
        if (snapshot.name(child.name) == name) {
            return &child;
        }
    }
    return nullptr;
}

const SnapKey& top_of(const RegistrySnapshot& snapshot, std::size_t root = 0) {
    return snapshot.keys()[snapshot.roots()[root].key];
}

// The key at `path` (names separated by '\') below `from`; null if there is none.
const SnapKey* find_key(const RegistrySnapshot& snapshot, const SnapKey& from, std::wstring_view path) {
    const SnapKey* key = &from;
    while (key && !path.empty()) {
        const std::size_t separator = path.find(L'\\');
        key = child_named(snapshot, *key, path.substr(0, separator));
        path = separator == std::wstring_view::npos ? std::wstring_view() : path.substr(separator + 1);
    }
    return key;
}

TEST(RegistrySnapshotTest, FindsCreatedDeletedAndModifiedItems) {
    testing::ScratchKey scratch;
    scratch.set_string(L"Root", L"changed", L"before");
    scratch.set_dword(LR"(Root\A)", L"kept", 1);
    scratch.set_string(LR"(Root\A)", L"gone", L"x");
    scratch.create(LR"(Root\A\Sub)");
    scratch.set_string(LR"(Root\B\Inner)", L"v", L"x");
    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);
    EXPECT_EQ(snapshot.keys().size(), 5u);  // Root, A, B, A\Sub, B\Inner
    EXPECT_EQ(snapshot.values().size(), 4u);
    EXPECT_EQ(changes_of(snapshot, roots), std::vector<std::wstring>());

    next_clock_tick();
    scratch.set_string(LR"(Root\C)", L"v", L"x");
    scratch.set_dword(LR"(Root\C\D)", L"one", 1);
    scratch.set_dword(LR"(Root\C\D)", L"two", 2);
    scratch.set_dword(LR"(Root\A)", L"new", 1);
    delete_value(scratch, LR"(Root\A)", L"gone");
    delete_tree(scratch, LR"(Root\B)");
    scratch.set_string(L"Root", L"changed", L"after");

    // A created key is one item holding its subkeys and values; a deleted key is one item too.
    EXPECT_EQ(sorted(changes_of(snapshot, roots)), sorted({
                                                       LR"(created key HKLM\SOFTWARE\C [4])",
                                                       LR"(created value HKLM\SOFTWARE\A : new)",
                                                       LR"(deleted value HKLM\SOFTWARE\A : gone)",
                                                       LR"(deleted key HKLM\SOFTWARE\B)",
                                                       LR"(modified value HKLM\SOFTWARE : changed)",
                                                   }));
}

// What a created key holds is kept by name, so the leftover scan can tell what was added since.
TEST(RegistrySnapshotTest, ACreatedKeyKeepsWhatItHolds) {
    testing::ScratchKey scratch;
    scratch.create(L"Root");
    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);

    next_clock_tick();
    scratch.set_string(LR"(Root\C)", L"", L"default");
    scratch.set_dword(LR"(Root\C\D)", L"one", 1);
    scratch.create(LR"(Root\C\D\Empty)");
    scratch.create(LR"(Root\E)");
    std::vector<SnapshotDiagnostic> diagnostics;
    const auto changes = diff_registry(snapshot, roots, {}, diagnostics);
    EXPECT_EQ(describe(diagnostics), std::vector<std::wstring>());
    ASSERT_EQ(changes.size(), 2u);
    const auto& c = changes[0];
    ASSERT_EQ(describe(c), LR"(created key HKLM\SOFTWARE\C [4])");
    ASSERT_TRUE(c.inside.has_value());
    const std::vector<leftovers::ContainedItem> expected = {
        {L"", L"", false},
        {L"D", std::nullopt, true},
        {L"D", L"one", false},
        {LR"(D\Empty)", std::nullopt, true},
    };
    EXPECT_EQ(*c.inside, expected);
    // An empty key holds nothing, which is known too.
    ASSERT_EQ(describe(changes[1]), LR"(created key HKLM\SOFTWARE\E)");
    ASSERT_TRUE(changes[1].inside.has_value());
    EXPECT_TRUE(changes[1].inside->empty());
}

TEST(RegistrySnapshotTest, RewritingAValueWithTheSameDataIsNoChange) {
    testing::ScratchKey scratch;
    scratch.set_string(LR"(Root\Key)", L"value", L"same");
    scratch.set_dword(LR"(Root\Key)", L"other", 7);
    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);

    // Windows leaves LastWriteTime alone when a value is set to the data it already holds, so the
    // value is changed and changed back: the key's time moves, its values end up as they were.
    const auto key = scratch.create(LR"(Root\Key)");
    const std::int64_t before = last_write(key.get());
    for (int attempt = 0; attempt < 10 && last_write(key.get()) == before; ++attempt) {
        next_clock_tick();
        scratch.set_string(LR"(Root\Key)", L"value", L"different");
        scratch.set_string(LR"(Root\Key)", L"value", L"same");
    }
    // The key's time changed, so its values were read again, and compared equal.
    ASSERT_NE(last_write(key.get()), before);
    EXPECT_EQ(changes_of(snapshot, roots), std::vector<std::wstring>());
}

TEST(RegistrySnapshotTest, LinksAreRecordedButNotFollowed) {
    testing::ScratchKey scratch;
    scratch.set_string(LR"(Target\Inside)", L"v", L"x");
    const auto parent = scratch.create(L"Root");
    HKEY link = nullptr;
    if (::RegCreateKeyExW(parent.get(), L"Link", 0, nullptr, REG_OPTION_VOLATILE | REG_OPTION_CREATE_LINK,
                          KEY_ALL_ACCESS, nullptr, &link, nullptr) != ERROR_SUCCESS) {
        GTEST_SKIP() << "this account can't create registry links";
    }
    const std::wstring target =
        L"\\Registry\\User\\" + platform::current_user_sid() + L"\\" + scratch.path() + L"\\Target";
    ::RegSetValueExW(link, L"SymbolicLinkValue", 0, REG_LINK, reinterpret_cast<const BYTE*>(target.c_str()),
                     static_cast<DWORD>(target.size() * sizeof(wchar_t)));
    ::RegCloseKey(link);
    // Remove the link itself, not its target.
    const auto remove_link = wil::scope_exit([&] {
        HKEY opened = nullptr;
        if (::RegOpenKeyExW(parent.get(), L"Link", REG_OPTION_OPEN_LINK, DELETE, &opened) == ERROR_SUCCESS) {
            using NtDeleteKeyFunction = LONG(NTAPI*)(HANDLE);
            const auto nt_delete_key = reinterpret_cast<NtDeleteKeyFunction>(
                ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtDeleteKey"));
            nt_delete_key(opened);
            ::RegCloseKey(opened);
        }
    });

    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);
    ASSERT_EQ(snapshot.keys().size(), 2u);
    const SnapKey* recorded = child_named(snapshot, top_of(snapshot), L"Link");
    ASSERT_NE(recorded, nullptr);
    EXPECT_EQ(recorded->flags, kKeyLink);
    EXPECT_EQ(recorded->child_count + recorded->value_count, 0u);

    next_clock_tick();
    scratch.set_string(LR"(Target\Inside)", L"v", L"changed");
    scratch.create(LR"(Target\New)");
    EXPECT_EQ(changes_of(snapshot, roots), std::vector<std::wstring>());
}

TEST(RegistrySnapshotTest, OddNamesSortAndCompareExactly) {
    testing::ScratchKey scratch;
    const std::vector<std::wstring> names = {L"Zeta",
                                             L"alpha",
                                             L"Beta",
                                             L"_under",
                                             L"10",
                                             L"9",
                                             L"\x0531\x057E\x0565\x056C",  // Armenian, capital first letter
                                             L"\x0561\x0562",              // Armenian, small letters
                                             std::wstring(L"x\xD800y"),    // An unpaired high surrogate
                                             std::wstring(L"\xDFFF")};     // An unpaired low surrogate
    for (const auto& name : names) {
        scratch.create(L"Root\\" + name);
    }
    const std::vector<std::wstring> value_names = {L"", L"b", L"A", L"\x0531", std::wstring(L"v\xD800")};
    for (const auto& name : value_names) {
        set_raw(scratch, L"Root", name, REG_SZ, L"data");
    }
    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);

    // Kept exactly, in the snapshot's order.
    const auto in_order = [](std::vector<std::wstring> expected) {
        std::sort(expected.begin(), expected.end(), [](const std::wstring& a, const std::wstring& b) {
            return compare_registry_names(a, b) < 0;
        });
        return expected;
    };
    std::vector<std::wstring> children;
    for (const SnapKey& child : snapshot.children(top_of(snapshot))) {
        children.emplace_back(snapshot.name(child.name));
    }
    EXPECT_EQ(children, in_order(names));
    std::vector<std::wstring> values;
    for (const SnapValue& value : snapshot.values(top_of(snapshot))) {
        values.emplace_back(snapshot.name(value.name));
    }
    EXPECT_EQ(values, in_order(value_names));
    EXPECT_LT(compare_registry_names(L"alpha", L"Beta"), 0);  // Case is ignored...
    EXPECT_LT(compare_registry_names(L"A", L"a"), 0);         // ...until the names are otherwise equal

    next_clock_tick();
    scratch.create(std::wstring(L"Root\\new\xDC00"));
    set_raw(scratch, std::wstring(L"Root\\x\xD800y"), std::wstring(L"w\xDBFF"), REG_SZ, L"x");
    set_raw(scratch, L"Root", std::wstring(L"v\xD800"), REG_SZ, L"other");
    set_raw(scratch, L"Root\\\x0531\x057E\x0565\x056C", L"\x0565", REG_SZ, L"x");
    EXPECT_EQ(sorted(changes_of(snapshot, roots)),
              sorted({std::wstring(L"created key HKLM\\SOFTWARE\\new\xDC00"),
                      std::wstring(L"created value HKLM\\SOFTWARE\\x\xD800y : w\xDBFF"),
                      std::wstring(L"modified value HKLM\\SOFTWARE : v\xD800"),
                      std::wstring(L"created value HKLM\\SOFTWARE\\\x0531\x057E\x0565\x056C : \x0565")}));
}

TEST(RegistrySnapshotTest, TheFileRoundTripsExactly) {
    testing::ScratchKey scratch;
    testing::TempFolder temp;
    for (int i = 0; i < 40; ++i) {
        const std::wstring key = L"Root\\Key" + std::to_wstring(i % 7) + L"\\Sub" + std::to_wstring(i);
        scratch.set_dword(key, L"n", static_cast<DWORD>(i));
        scratch.set_string(key, L"same name", L"text " + std::to_wstring(i));
    }
    set_raw(scratch, L"Root", std::wstring(L"odd\xD800"), REG_BINARY, L"\x0102");
    const std::vector roots{root_at(scratch, L"Root", kSoftware), root_at(scratch, L"User", kUserSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);
    const std::wstring first = temp / L"first.bin";
    const std::wstring second = temp / L"second.bin";
    ASSERT_TRUE(write_snapshot(snapshot, first));
    const auto read = read_snapshot(first);
    ASSERT_TRUE(read.has_value());

    ASSERT_EQ(read->roots().size(), snapshot.roots().size());
    for (std::size_t i = 0; i < snapshot.roots().size(); ++i) {
        EXPECT_TRUE(platform::same_address(read->roots()[i].address, snapshot.roots()[i].address));
        EXPECT_EQ(read->roots()[i].address.sid, snapshot.roots()[i].address.sid);
        EXPECT_EQ(read->roots()[i].key, snapshot.roots()[i].key);
    }
    ASSERT_EQ(read->keys().size(), snapshot.keys().size());
    EXPECT_EQ(std::memcmp(read->keys().data(), snapshot.keys().data(), snapshot.keys().size_bytes()), 0);
    ASSERT_EQ(read->values().size(), snapshot.values().size());
    EXPECT_EQ(std::memcmp(read->values().data(), snapshot.values().data(), snapshot.values().size_bytes()),
              0);
    ASSERT_EQ(read->name_count(), snapshot.name_count());
    for (std::uint32_t i = 0; i < snapshot.name_count(); ++i) {
        EXPECT_EQ(read->name(i), snapshot.name(i));
    }
    // Names are stored once each.
    EXPECT_LT(snapshot.name_count(), snapshot.keys().size() + snapshot.values().size());

    ASSERT_TRUE(write_snapshot(*read, second));
    EXPECT_EQ(file_bytes(first), file_bytes(second));
    EXPECT_EQ(changes_of(*read, roots), std::vector<std::wstring>());
}

TEST(RegistrySnapshotTest, ADamagedFileIsRejected) {
    testing::ScratchKey scratch;
    testing::TempFolder temp;
    scratch.set_string(LR"(Root\Key)", L"value", L"text");
    scratch.set_dword(LR"(Root\Other)", L"n", 1);
    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const std::wstring path = temp / L"snapshot.bin";
    ASSERT_TRUE(write_snapshot(snapshot_of(roots), path));
    const std::string good = file_bytes(path);
    ASSERT_TRUE(read_snapshot(path).has_value());

    const auto rejects = [&](std::string bytes) {
        write_bytes(path, bytes);
        return !read_snapshot(path).has_value();
    };
    for (const std::size_t at : {std::size_t{0}, std::size_t{9}, std::size_t{16}, std::size_t{30},
                                 good.size() / 2, good.size() - 1}) {
        std::string flipped = good;
        flipped[at] = static_cast<char>(flipped[at] ^ 0x10);
        EXPECT_TRUE(rejects(flipped)) << "byte " << at;
    }
    EXPECT_TRUE(rejects(good.substr(0, good.size() - 1)));
    EXPECT_TRUE(rejects(good + '\0'));
    EXPECT_TRUE(rejects(good.substr(0, 20)));
    EXPECT_TRUE(rejects(std::string()));
    EXPECT_FALSE(rejects(good));
    EXPECT_FALSE(read_snapshot(temp / L"missing.bin").has_value());
}

TEST(RegistrySnapshotTest, ThreadsDontChangeTheResult) {
    testing::ScratchKey scratch;
    testing::TempFolder temp;
    // Wide enough to be split into jobs (at least 256 children near the root), at two depths.
    for (int i = 0; i < 600; ++i) {
        wchar_t name[16] = {};
        std::swprintf(name, 16, L"Item%03d", i);
        scratch.set_dword(LR"(Root\Wide\)" + std::wstring(name), L"n", static_cast<DWORD>(i));
        scratch.set_string(LR"(Root\Wide\)" + std::wstring(name) + LR"(\Sub)", L"s", name);
    }
    for (int i = 0; i < 300; ++i) {
        scratch.set_dword(LR"(Root\Level1\Level2\Many\K)" + std::to_wstring(i), L"n", static_cast<DWORD>(i));
    }
    std::wstring deep = LR"(Root\Deep)";
    for (int i = 0; i < 12; ++i) {
        deep += L"\\L" + std::to_wstring(i);
        scratch.set_dword(deep, L"depth", static_cast<DWORD>(i));
    }
    scratch.set_string(LR"(User\Key)", L"v", L"x");
    const std::vector roots{root_at(scratch, L"Root", kSoftware), root_at(scratch, L"User", kUserSoftware)};

    const RegistrySnapshot single = snapshot_of(roots, 1);
    const RegistrySnapshot parallel = snapshot_of(roots, 8);
    EXPECT_GT(single.keys().size(), 1500u);
    ASSERT_TRUE(write_snapshot(single, temp / L"single.bin"));
    ASSERT_TRUE(write_snapshot(parallel, temp / L"parallel.bin"));
    EXPECT_EQ(file_bytes(temp / L"single.bin"), file_bytes(temp / L"parallel.bin"));

    next_clock_tick();
    scratch.set_dword(LR"(Root\Wide\Item010)", L"n", 1000);
    scratch.create(LR"(Root\Wide\Item700)");
    delete_tree(scratch, LR"(Root\Wide\Item020)");
    scratch.set_string(LR"(Root\Wide\Item599\Sub\New)", L"v", L"x");
    scratch.set_dword(LR"(Root\Level1\Level2\Many\K150)", L"added", 1);
    delete_tree(scratch, LR"(Root\Deep\L0\L1\L2\L3\L4\L5\L6\L7\L8\L9\L10\L11)");
    const auto one = changes_of(single, roots, 1);
    EXPECT_EQ(one, changes_of(single, roots, 8));
    EXPECT_EQ(sorted(one), sorted({
                               LR"(modified value HKLM\SOFTWARE\Wide\Item010 : n)",
                               LR"(created key HKLM\SOFTWARE\Wide\Item700)",
                               LR"(deleted key HKLM\SOFTWARE\Wide\Item020)",
                               LR"(created key HKLM\SOFTWARE\Wide\Item599\Sub\New [1])",
                               LR"(created value HKLM\SOFTWARE\Level1\Level2\Many\K150 : added)",
                               LR"(deleted key HKLM\SOFTWARE\Deep\L0\L1\L2\L3\L4\L5\L6\L7\L8\L9\L10\L11)",
                           }));
}

TEST(RegistrySnapshotTest, ServicesTasksAndFirewallRulesAreNamed) {
    testing::ScratchKey scratch;
    constexpr wchar_t kRules[] = LR"(Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)";
    constexpr wchar_t kTaskCache[] = LR"(Software\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache)";
    scratch.set_dword(LR"(Services\Existing)", L"Type", 16);
    for (const wchar_t* rule : {L"rule-kept", L"rule-changed", L"rule-deleted"}) {
        scratch.set_string(kRules, rule, L"v2.31|Action=Allow|");
    }
    scratch.set_string(std::wstring(kTaskCache) + LR"(\Tree\Existing)", L"Id", L"{1}");
    scratch.create(std::wstring(kTaskCache) + LR"(\Tasks)");
    const std::vector roots{root_at(scratch, L"Services", kServices),
                            root_at(scratch, L"Software", kSoftware)};
    const RegistrySnapshot snapshot = snapshot_of(roots);

    next_clock_tick();
    scratch.set_dword(LR"(Services\NewService)", L"Type", 16);
    scratch.set_string(LR"(Services\NewService)", L"ImagePath", L"C:\\Contoso\\service.exe");
    scratch.set_dword(LR"(Services\NewDriver)", L"Type", 1);
    scratch.set_dword(LR"(Services\Existing\Parameters)", L"Port", 80);
    scratch.set_string(kRules, L"rule-new", L"v2.31|Action=Allow|");
    scratch.set_string(kRules, L"rule-changed", L"v2.31|Action=Block|");
    delete_value(scratch, kRules, L"rule-deleted");
    // Keys with an Id are tasks; the others task folders, reported apart from their tasks.
    scratch.set_string(std::wstring(kTaskCache) + LR"(\Tree\Contoso\Updater)", L"Id", L"{2}");
    scratch.set_dword(std::wstring(kTaskCache) + LR"(\Tree\Contoso\Updater)", L"Index", 3);
    scratch.set_string(std::wstring(kTaskCache) + LR"(\Tree\Solo)", L"Id", L"{3}");
    scratch.set_string(std::wstring(kTaskCache) + LR"(\Tasks\{2})", L"Path", L"\\Contoso\\Updater");

    EXPECT_EQ(sorted(changes_of(snapshot, roots)),
              sorted({
                  L"created service NewService [2]",
                  L"created service NewDriver (driver) [1]",
                  LR"(created key HKLM\SYSTEM\CurrentControlSet\Services\Existing\Parameters [1])",
                  L"created firewall rule-new",
                  L"modified firewall rule-changed",
                  L"deleted firewall rule-deleted",
                  LR"(created task folder \Contoso [3])",
                  LR"(created task \Contoso\Updater [2])",
                  LR"(created task \Solo [1])",
              }));
}

TEST(RegistrySnapshotTest, ChurnKnownValuesAndNestedRootsAreLeftOut) {
    testing::ScratchKey scratch;
    scratch.set_string(LR"(Software\Microsoft\Windows\CurrentVersion\SharedDLLs)", L"C:\\Shared\\a.dll",
                       L"x");
    scratch.create(LR"(Software\Microsoft\Windows\CurrentVersion\Component Based Servicing\Packages)");
    // The parents exist at the start: a created parent would be reported as one created key.
    scratch.create(LR"(User\Microsoft\Windows\CurrentVersion)");
    scratch.create(LR"(User\Classes)");
    scratch.create(LR"(UserClasses\Local Settings\Software\Microsoft\Windows\CurrentVersion)");
    const std::vector roots{
        root_at(scratch, L"Software", kSoftware), root_at(scratch, L"Services", kServices),
        root_at(scratch, L"User", kUserSoftware), root_at(scratch, L"UserClasses", kUserClasses)};
    const RegistrySnapshot snapshot = snapshot_of(roots);
    // Excluded subtrees aren't read at all; SharedDLLs keeps no values.
    const SnapKey* version = find_key(snapshot, top_of(snapshot), LR"(Microsoft\Windows\CurrentVersion)");
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(child_named(snapshot, *version, L"Component Based Servicing"), nullptr);
    const SnapKey* shared = child_named(snapshot, *version, L"SharedDLLs");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->value_count, 0u);
    EXPECT_EQ(child_named(snapshot, top_of(snapshot, 2), L"Classes"), nullptr);

    next_clock_tick();
    scratch.set_dword(LR"(Software\Microsoft\Windows\CurrentVersion\SharedDLLs)", L"C:\\Shared\\b.dll", 1);
    scratch.set_dword(LR"(Software\Microsoft\Windows\CurrentVersion\Component Based Servicing\Packages\P)",
                      L"n", 1);
    scratch.set_dword(LR"(Software\Microsoft\Windows Defender\Signatures)", L"n", 1);
    scratch.set_dword(LR"(Services\bam\State\UserSettings)", L"n", 1);
    scratch.set_dword(LR"(User\Classes\.contoso)", L"n", 1);  // Read through the classes root only
    scratch.set_dword(LR"(UserClasses\.contoso)", L"n", 1);
    scratch.set_dword(LR"(Software\Contoso)", L"n", 1);
    // Packages' and the Start menu's state is read: Windows writes it for what an installer
    // registers (the noise filter sets it aside).
    scratch.set_dword(LR"(Software\Microsoft\Windows\CurrentVersion\Appx\X)", L"n", 1);
    scratch.set_dword(LR"(User\Microsoft\Windows\CurrentVersion\CloudStore\Store\Cache)", L"n", 1);
    scratch.set_dword(
        LR"(UserClasses\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository)", L"n",
        1);

    const std::wstring user = LR"(created key HKU\S-1-5-21-1-2-3-1001\Software)";
    EXPECT_EQ(sorted(changes_of(snapshot, roots)),
              sorted({
                  LR"(created key HKLM\SOFTWARE\Contoso [1])",
                  LR"(created key HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Appx [2])",
                  user + LR"(\Classes\.contoso [1])",
                  user + LR"(\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel [2])",
                  user + LR"(\Microsoft\Windows\CurrentVersion\CloudStore [3])",
              }));
}

TEST(RegistrySnapshotTest, KeysThatDenyAccessAreUnreadable) {
    if (platform::enable_privilege(SE_BACKUP_NAME)) {
        GTEST_SKIP() << "with SeBackupPrivilege every key can be read";
    }
    testing::ScratchKey scratch;
    scratch.set_string(LR"(Root\Locked\Inner)", L"v", L"x");
    scratch.set_string(LR"(Root\Open)", L"v", L"x");
    // An empty DACL denies everyone; this handle keeps the access it was opened with, to undo it.
    const auto locked = scratch.create(LR"(Root\Locked)");
    const auto set_dacl = [&](PACL dacl) {
        SECURITY_DESCRIPTOR descriptor{};
        ::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
        ::SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE);
        EXPECT_EQ(::RegSetKeySecurity(locked.get(), DACL_SECURITY_INFORMATION, &descriptor), ERROR_SUCCESS);
    };
    ACL empty{};
    ::InitializeAcl(&empty, sizeof(empty), ACL_REVISION);
    set_dacl(&empty);
    const auto unlock = wil::scope_exit([&] { set_dacl(nullptr); });

    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    std::vector<SnapshotDiagnostic> diagnostics;
    const RegistrySnapshot snapshot = take_registry_snapshot(roots, {}, diagnostics);
    EXPECT_EQ(describe(diagnostics),
              std::vector<std::wstring>{LR"(key-unreadable HKLM\SOFTWARE\Locked (5))"});
    const SnapKey* recorded = child_named(snapshot, top_of(snapshot), L"Locked");
    ASSERT_NE(recorded, nullptr);
    EXPECT_EQ(recorded->flags, kKeyUnreadable);

    // Its subtree isn't compared, and the diff says so.
    diagnostics.clear();
    EXPECT_TRUE(diff_registry(snapshot, roots, {}, diagnostics).empty());
    EXPECT_EQ(describe(diagnostics),
              std::vector<std::wstring>{LR"(key-unreadable-at-start HKLM\SOFTWARE\Locked (0))"});
}

TEST(RegistrySnapshotTest, AMissingRootIsReported) {
    testing::ScratchKey scratch;
    const std::vector<SnapshotRoot> missing{{kSoftware, scratch.get(), L"Missing", nullptr}};
    std::vector<SnapshotDiagnostic> diagnostics;
    const RegistrySnapshot snapshot = take_registry_snapshot(missing, {}, diagnostics);
    EXPECT_EQ(describe(diagnostics), std::vector<std::wstring>{LR"(root-unavailable HKLM\SOFTWARE (2))"});
    ASSERT_EQ(snapshot.roots().size(), 1u);
    EXPECT_EQ(snapshot.roots()[0].key, RegistrySnapshot::kNoKey);
    EXPECT_TRUE(snapshot.keys().empty());

    // At the stop: a root missing from the roots given, or one that can't be opened.
    const std::vector roots{root_at(scratch, L"Root", kSoftware)};
    const RegistrySnapshot taken = snapshot_of(roots);
    diagnostics.clear();
    EXPECT_TRUE(diff_registry(taken, {}, {}, diagnostics).empty());
    EXPECT_EQ(describe(diagnostics), std::vector<std::wstring>{LR"(root-unavailable HKLM\SOFTWARE (2))"});
    delete_tree(scratch, L"Root");
    diagnostics.clear();
    EXPECT_TRUE(diff_registry(taken, roots, {}, diagnostics).empty());
    EXPECT_EQ(describe(diagnostics), std::vector<std::wstring>{LR"(root-unavailable HKLM\SOFTWARE (2))"});
}

}  // namespace
}  // namespace avel::tracker
