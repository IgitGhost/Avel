#include "core/leftovers/orphans.h"

#include <windows.h>
#include <objbase.h>

#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <cwctype>

#include "core/leftovers/merge.h"
#include "core/leftovers/product_name.h"
#include "core/leftovers/protection.h"
#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers {

namespace {

// Folders Windows itself keeps in these locations, beyond the drive scan's blacklist.
constexpr std::array<std::wstring_view, 12> kWindowsFolders = {
    L"Packages", L"ssh",    L"USOShared", L"USOPrivate",         L"Comms", L"SoftwareDistribution",
    L"Whesvc",   L"dotnet", L"MSBuild",   L"boost_interprocess", L"NuGet", L"ModifiableWindowsApps"};

bool is_word_character(wchar_t c) { return std::iswalnum(c) != 0; }

// `word` (lowercase) appears in `text` (lowercase) between non-alphanumeric characters.
bool contains_word(std::wstring_view text, std::wstring_view word) {
    if (word.empty()) {
        return false;
    }
    for (size_t at = text.find(word); at != std::wstring_view::npos; at = text.find(word, at + 1)) {
        const bool starts = at == 0 || !is_word_character(text[at - 1]);
        const size_t end = at + word.size();
        const bool ends = end == text.size() || !is_word_character(text[end]);
        if (starts && ends) {
            return true;
        }
    }
    return false;
}

bool is_windows_folder(std::wstring_view name) {
    const std::wstring lower = platform::to_lower(name);
    return std::any_of(kWindowsFolders.begin(), kWindowsFolders.end(),
                       [&](auto folder) { return platform::iequals(name, folder); }) ||
           lower.starts_with(L"regid.") || contains_word(lower, L"microsoft") ||
           lower.starts_with(L"windows");
}

// A folder's subfolders, with their attributes.
std::vector<std::pair<std::wstring, DWORD>> subfolders(const std::wstring& folder) {
    std::vector<std::pair<std::wstring, DWORD>> names;
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(::FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                              FindExSearchLimitToDirectories, nullptr,
                                              FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        return names;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") {
            names.emplace_back(name, data.dwFileAttributes);
        }
    } while (::FindNextFileW(find.get(), &data));
    return names;
}

// The publisher without its legal form, lowercase: "contoso" for "Contoso Ltd.". Broader than
// trim_publisher, which keeps BCU's list (corp, inc, limited) for the name matching.
std::wstring company_of(std::wstring_view publisher) {
    constexpr std::array<std::wstring_view, 20> kLegalForms = {
        L"ltd", L"llc", L"gmbh", L"co", L"ag", L"sa",  L"s.a",  L"bv",          L"b.v",     L"plc",
        L"pty", L"srl", L"oy",   L"ab", L"kg", L"inc", L"corp", L"corporation", L"limited", L"incorporated"};
    std::wstring text = platform::to_lower(publisher);
    while (true) {
        while (!text.empty() && (text.back() == L' ' || text.back() == L',' || text.back() == L'.')) {
            text.pop_back();
        }
        const size_t space = text.find_last_of(L' ');
        if (space == std::wstring::npos ||
            std::find(kLegalForms.begin(), kLegalForms.end(), std::wstring_view(text).substr(space + 1)) ==
                kLegalForms.end()) {
            return text;
        }
        text.resize(space);
    }
}

struct Owner {
    ProductNames names;
    std::wstring name;       // Lowercase display name
    std::wstring publisher;  // Lowercase publisher, as recorded
    std::wstring stripped;   // Lowercase name without version or noise
    std::wstring company;    // Lowercase publisher without its legal form (company_of)
};

bool owned_by_name(const std::vector<Owner>& owners, std::wstring_view folder_name) {
    std::wstring item(name_without_extension(folder_name));
    std::replace(item.begin(), item.end(), L'_', L' ');
    item = platform::to_lower(platform::trim(item));
    const std::wstring full = platform::to_lower(folder_name);
    return std::any_of(owners.begin(), owners.end(), [&](const Owner& owner) {
        return match_product_name(owner.names, item) >= 0 || contains_word(owner.name, item) ||
               contains_word(owner.name, full) || contains_word(owner.publisher, item) ||
               (owner.stripped.size() > 4 && item.find(owner.stripped) != std::wstring::npos) ||
               (owner.company.size() >= 3 && contains_word(item, owner.company));
    });
}

}  // namespace

ScanResult find_orphans(const ScanEnvironment& environment, std::span<const inventory::Program> installed) {
    const auto started = std::chrono::steady_clock::now();
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize = wil::scope_exit([&] {
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });
    const auto since = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                     start);
    };

    ScanResult result;
    result.diagnostics = environment.diagnostics;
    const detail::Snapshots snapshots = detail::take_snapshots(environment);
    result.timings = snapshots.timings;
    auto stage = std::chrono::steady_clock::now();

    // Where orphans are looked for: where programs are installed, machine-wide and per user
    // (FOLDERID_UserProgramFiles). Users' AppData holds the data of programs that never register
    // (games, package managers, portable tools), so its folders aren't candidates.
    std::vector<std::pair<std::wstring, const UserScope*>> roots;
    const auto& machine = environment.machine_folders;
    for (const auto* path : {&machine.program_files, &machine.program_files_x86, &machine.program_data}) {
        if (!path->empty()) {
            roots.emplace_back(platform::normalize_path(*path), nullptr);
        }
    }
    for (const auto& user : environment.users) {
        if (!user.folders.local_app_data.empty()) {
            roots.emplace_back(platform::normalize_path(user.folders.local_app_data + LR"(\Programs)"),
                               &user);
        }
    }

    // What installed programs own.
    std::vector<std::wstring> owned_folders;
    std::vector<Owner> owners;
    for (const auto& program : installed) {
        const auto target = make_scan_target(program);
        for (const auto* folder : {&target.install_location, &target.uninstaller_folder}) {
            if (!folder->empty()) {
                owned_folders.push_back(*folder);
            }
        }
        owners.push_back({target.names, platform::to_lower(program.display_name),
                          platform::to_lower(program.publisher), platform::to_lower(target.names.name),
                          company_of(program.publisher)});
    }

    // What points into folders, lowercase and sorted, so the paths inside a folder are a range.
    std::vector<std::wstring> referenced;
    const auto add_reference = [&](const std::wstring& path) {
        if (!path.empty() && platform::is_absolute_path(path)) {
            referenced.push_back(platform::to_lower(platform::normalize_path(path)));
        }
    };
    for (const auto& reference : snapshots.references) {
        if (reference.kind != ReferenceKind::MuiCache && reference.kind != ReferenceKind::SharedDll) {
            std::for_each(reference.paths.begin(), reference.paths.end(), add_reference);
        }
    }
    for (const auto& service : snapshots.services) {
        std::for_each(service.paths.begin(), service.paths.end(), add_reference);
    }
    for (const auto& task : snapshots.tasks) {
        std::for_each(task.paths.begin(), task.paths.end(), add_reference);
    }
    for (const auto& com_class : snapshots.com_classes) {
        std::for_each(com_class.paths.begin(), com_class.paths.end(), add_reference);
    }
    for (const auto& shortcut : snapshots.shortcuts) {
        add_reference(shortcut.target);
    }
    for (const auto& rule : snapshots.firewall_rules) {
        add_reference(rule.program);
    }
    std::sort(referenced.begin(), referenced.end());
    const auto referenced_in = [&](const std::wstring& folder) {
        const std::wstring lower = platform::to_lower(folder);
        const std::wstring prefix = lower + L"\\";
        const auto inside = std::lower_bound(referenced.begin(), referenced.end(), prefix);
        return std::binary_search(referenced.begin(), referenced.end(), lower) ||
               (inside != referenced.end() && inside->starts_with(prefix));
    };

    // The roots are listed here rather than taken from the folder snapshot, which skips
    // Local\Programs as BCU does ("Programs" is a blacklisted name).
    std::vector<Leftover> found;
    for (const auto& [root, user] : roots) {
        for (const auto& [name, attributes] : subfolders(root)) {
            const std::wstring path = root + L"\\" + name;
            if ((attributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
                is_blacklisted_folder_name(name) || is_windows_folder(name) ||
                std::binary_search(snapshots.known_folders.begin(), snapshots.known_folders.end(),
                                   platform::to_lower(path))) {
                continue;
            }
            const bool owned = std::any_of(owned_folders.begin(), owned_folders.end(),
                                           [&](const auto& folder) {
                                               return platform::path_is_under(folder, path) ||
                                                      platform::path_is_under(path, folder);
                                           }) ||
                               owned_by_name(owners, name) || referenced_in(path);
            if (owned) {
                continue;
            }
            Leftover item;
            item.location = FileLocation{path, true};
            item.records = {{Evidence::NoOwner}};
            if (user && !platform::iequals(user->profile.sid, environment.current_user_sid)) {
                item.records.push_back({Evidence::OtherUserData});
            }
            item.owner_ids = {L"folder:" + path};
            item.found_by = Scanner::Orphans;
            item.category = Category::Files;
            found.push_back(std::move(item));
        }
    }
    result.timings.push_back({"matching", since(stage)});

    stage = std::chrono::steady_clock::now();
    const HardFloor& floor = HardFloor::live();
    const ProtectedPolicy policy(snapshots.known_folders);
    for (auto& item : found) {
        if (floor.check(item.location) == ProtectionReason::None &&
            policy.check(item.location) == ProtectionReason::None) {
            result.leftovers.push_back(std::move(item));
        } else {
            ++result.protected_count;
        }
    }
    link_containment(result.leftovers);
    result.timings.push_back({"protection", since(stage)});
    result.elapsed = since(started);
    return result;
}

}  // namespace avel::leftovers
