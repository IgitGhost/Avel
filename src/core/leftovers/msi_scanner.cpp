// Windows Installer's own registration of a product. A failed or skipped msiexec leaves it behind,
// and as long as it is there Windows Installer considers the product installed.
//
// For a product code {P}, packed as p (see inventory/msi_codes.h), Windows Installer writes:
//   - per machine: SOFTWARE\Classes\Installer\{Products,Features}\p, a p value under
//     Classes\Installer\UpgradeCodes\<upgrade code>, ...\CurrentVersion\Installer\UserData\S-1-5-18\
//     Products\p, and a p value under UserData\S-1-5-18\Components\<component> per component;
//   - per user: the same in the user's Software\Microsoft\Installer, or for a managed install in
//     ...\Installer\Managed\<sid>\Installer, and UserData\<sid>.
// WiX Burn records the MSI packages it installs under Classes\Installer\Dependencies\{P}.
// Patches are shared between products and left alone.
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <thread>

#include "core/inventory/msi_codes.h"
#include "core/leftovers/product_name.h"
#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kClasses[] = LR"(SOFTWARE\Classes\Installer)";
constexpr wchar_t kInstaller[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer)";
constexpr wchar_t kUserInstaller[] = LR"(Software\Microsoft\Installer)";
constexpr wchar_t kLocalSystem[] = L"S-1-5-18";
constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;

// Where Windows Installer keeps one product's registration.
struct Places {
    std::vector<RegistryAddress> product_keys;   // Keys named after the packed code
    std::vector<RegistryAddress> upgrade_codes;  // UpgradeCodes keys; their subkeys may list the product
    std::optional<RegistryAddress> dependency;   // Burn's Dependencies\{P}
    std::wstring user_data_sid;                  // UserData\<sid> holds its Products and Components
};

std::optional<Places> places_of(const ScanTarget& target, const std::wstring& packed) {
    Places places;
    const RegistryAddress machine{Hive::Machine, L"", L""};
    const auto at = [](RegistryAddress root, std::wstring path) {
        root.path = std::move(path);
        return root;
    };
    if (target.scope == inventory::Scope::Machine) {
        places.product_keys = {at(machine, kClasses + std::wstring(LR"(\Products\)") + packed),
                               at(machine, kClasses + std::wstring(LR"(\Features\)") + packed)};
        places.upgrade_codes = {at(machine, kClasses + std::wstring(LR"(\UpgradeCodes)"))};
        places.dependency =
            at(machine, kClasses + std::wstring(LR"(\Dependencies\)") + target.msi_product_code);
        places.user_data_sid = kLocalSystem;
    } else {
        if (target.user_sid.empty()) {
            return std::nullopt;
        }
        const RegistryAddress user{Hive::User, target.user_sid, L""};
        const std::wstring managed =
            kInstaller + std::wstring(LR"(\Managed\)") + target.user_sid + LR"(\Installer)";
        places.product_keys = {at(user, kUserInstaller + std::wstring(LR"(\Products\)") + packed),
                               at(user, kUserInstaller + std::wstring(LR"(\Features\)") + packed),
                               at(machine, managed + LR"(\Products\)" + packed),
                               at(machine, managed + LR"(\Features\)" + packed)};
        places.upgrade_codes = {at(user, kUserInstaller + std::wstring(LR"(\UpgradeCodes)")),
                                at(machine, managed + LR"(\UpgradeCodes)")};
        places.dependency =
            at(user, LR"(Software\Classes\Installer\Dependencies\)" + target.msi_product_code);
        places.user_data_sid = target.user_sid;
    }
    places.product_keys.push_back(at(machine, kInstaller + std::wstring(LR"(\UserData\)") +
                                                  places.user_data_sid + LR"(\Products\)" + packed));
    return places;
}

bool key_exists(const ScanEnvironment& environment, const RegistryAddress& address) {
    platform::RegistryKey key;
    return open_key(environment, address, key) == ERROR_SUCCESS;
}

// The keys whose subkeys list the product among their values.
std::vector<RegistryAddress> client_parents(const Places& places) {
    std::vector<RegistryAddress> parents = places.upgrade_codes;
    parents.push_back(
        {Hive::Machine, L"",
         kInstaller + std::wstring(LR"(\UserData\)") + places.user_data_sid + LR"(\Components)"});
    return parents;
}

// A REG_SZ value's text; empty for other types.
std::wstring string_value(HKEY key, const std::wstring& name) {
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (::RegQueryValueExW(key, name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(text.data()),
                           &size) != ERROR_SUCCESS) {
        return {};
    }
    text.resize(::wcsnlen(text.c_str(), size / sizeof(wchar_t)));
    return text;
}

const std::wstring& product_of(const MsiClient& client) { return client.product; }

const std::wstring& product_of(const std::wstring& product) { return product; }

// Which of `products` (packed codes) the key lists as values. For a few products asking for each
// by name is cheapest; for many, one pass over the key's value names.
std::vector<std::wstring> clients_of(HKEY key, const std::set<std::wstring>& products) {
    std::vector<std::wstring> found;
    if (products.size() <= 8) {
        for (const auto& product : products) {
            if (::RegQueryValueExW(key, product.c_str(), nullptr, nullptr, nullptr, nullptr) ==
                ERROR_SUCCESS) {
                found.push_back(product);
            }
        }
        return found;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t value[64] = {};
        DWORD length = 64;
        const LSTATUS status =
            ::RegEnumValueW(key, index, value, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            continue;  // A longer name than a product code's
        }
        std::wstring product(value, length);
        std::transform(product.begin(), product.end(), product.begin(), ::towupper);
        if (products.contains(product)) {
            found.push_back(std::move(product));
        }
    }
    return found;
}

// The upgrade codes and components that list the product in its own places, not in another
// user's registration of the same product.
std::vector<const MsiClient*> own_clients(const Snapshots& snapshots, const Places& places,
                                          const std::wstring& packed) {
    std::set<std::wstring> own_parents;
    for (const auto& parent : client_parents(places)) {
        own_parents.insert(platform::to_lower(platform::display_path(parent)));
    }
    const auto& clients = snapshots.msi_clients;
    const auto [first, last] =
        std::equal_range(clients.begin(), clients.end(), packed,
                         [](const auto& a, const auto& b) { return product_of(a) < product_of(b); });
    std::vector<const MsiClient*> own;
    for (auto client = first; client != last; ++client) {
        if (own_parents.contains(client->parent)) {
            own.push_back(&*client);
        }
    }
    return own;
}

DWORD value_count(HKEY key) {
    DWORD count = 0;
    ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, nullptr, nullptr,
                       nullptr, nullptr);
    return count;
}

std::optional<std::wstring> packed_code(const ScanTarget& target) {
    return target.msi_product_code.empty() ? std::nullopt : inventory::pack_guid(target.msi_product_code);
}

Leftover make_item(const ScanContext& context, Location location) {
    Leftover item;
    item.location = std::move(location);
    item.records = {
        {Evidence::InstallerRegistration, 0, ReferenceKind::None, context.target.msi_product_code}};
    item.owner_ids = {context.target.program_id};
    item.found_by = Scanner::MsiRegistration;
    item.category = Category::Registry;
    return item;
}

}  // namespace

void take_msi_clients(const ScanEnvironment& environment, std::span<const ScanTarget> targets,
                      Snapshots& snapshots) {
    // The packed codes of products still registered, and the keys that may list them.
    std::set<std::wstring> products;
    std::map<std::wstring, RegistryAddress> parents;  // By lowercase display path
    for (const auto& target : targets) {
        const auto packed = packed_code(target);
        const auto places = packed ? places_of(target, *packed) : std::nullopt;
        if (!places || std::none_of(places->product_keys.begin(), places->product_keys.end(),
                                    [&](const auto& key) { return key_exists(environment, key); })) {
            continue;
        }
        products.insert(*packed);
        for (const auto& parent : client_parents(*places)) {
            parents.emplace(platform::to_lower(platform::display_path(parent)), parent);
        }
    }
    for (const auto& [parent_name, parent_address] : parents) {
        platform::RegistryKey parent;
        if (open_key(environment, parent_address, parent) != ERROR_SUCCESS) {
            continue;
        }
        // UserData\S-1-5-18\Components holds over 100,000 keys on a typical machine, so they are
        // read in parallel, a slice per thread.
        const auto names = parent.subkey_names();
        const std::size_t workers = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
        const std::size_t slice = (names.size() + workers - 1) / workers;
        std::vector<std::future<std::vector<MsiClient>>> parts;
        for (std::size_t begin = 0; begin < names.size(); begin += slice) {
            const std::size_t end = std::min(begin + slice, names.size());
            parts.push_back(std::async(std::launch::async, [&, begin, end] {
                std::vector<MsiClient> found;
                for (std::size_t i = begin; i < end; ++i) {
                    platform::RegistryKey client;
                    if (platform::RegistryKey::open(parent.get(), names[i], kRead, client) != ERROR_SUCCESS) {
                        continue;
                    }
                    for (const auto& product : clients_of(client.get(), products)) {
                        RegistryAddress key = parent_address;
                        key.path += L"\\" + names[i];
                        found.push_back({std::move(key), parent_name, product,
                                         string_value(client.get(), product),
                                         value_count(client.get()) == 1});
                    }
                }
                return found;
            }));
        }
        for (auto& part : parts) {
            auto found = part.get();
            snapshots.msi_clients.insert(snapshots.msi_clients.end(), std::make_move_iterator(found.begin()),
                                         std::make_move_iterator(found.end()));
        }
    }
    std::sort(snapshots.msi_clients.begin(), snapshots.msi_clients.end(),
              [](const MsiClient& a, const MsiClient& b) { return a.product < b.product; });
}

void scan_msi_registration(const ScanContext& context, std::vector<Leftover>& found) {
    const auto packed = packed_code(context.target);
    const auto places = packed ? places_of(context.target, *packed) : std::nullopt;
    if (!places) {
        return;
    }
    const auto& environment = context.environment;
    for (const auto& key : places->product_keys) {
        if (key_exists(environment, key)) {
            found.push_back(make_item(context, RegistryKeyLocation{key}));
        }
    }
    if (places->dependency && key_exists(environment, *places->dependency)) {
        found.push_back(make_item(context, RegistryKeyLocation{*places->dependency}));
    }
    // An upgrade code or a component the product was the only client of goes as a whole.
    for (const auto* client : own_clients(context.snapshots, *places, *packed)) {
        if (client->only_client) {
            found.push_back(make_item(context, RegistryKeyLocation{client->key}));
        } else {
            found.push_back(make_item(context, RegistryValueLocation{client->key, *packed}));
        }
    }
}

std::wstring msi_install_folder(const ScanEnvironment& environment, const Snapshots& snapshots,
                                const ScanTarget& target,
                                const std::function<bool(const std::wstring& folder)>& is_protected) {
    const auto packed = packed_code(target);
    const auto places = packed ? places_of(target, *packed) : std::nullopt;
    if (!places) {
        return {};
    }
    // The folders of the components' files, by the program folder they lie in: the highest one
    // below a protected folder ("C:\Program Files\7-Zip" for "C:\Program Files\7-Zip\Lang\de.txt").
    std::map<std::wstring, std::vector<std::wstring>> by_root;
    std::map<std::wstring, std::wstring> roots;  // Folder -> its program folder ("" if protected)
    const std::wstring& windows = environment.machine_folders.windows;
    for (const auto* client : own_clients(snapshots, *places, *packed)) {
        const std::wstring& path = client->data;
        // "C:\dir\file" or "C:\dir\"; registry key paths ("02:\SOFTWARE\...") and files run from
        // the source ("C?\...") say nothing about the install folder.
        if (path.size() < 4 || path[1] != L':' || path[2] != L'\\' ||
            client->parent.find(L"\\components") == std::wstring::npos) {
            continue;
        }
        const std::wstring folder =
            path.back() == L'\\' ? platform::normalize_path(path) : platform::parent_path(path);
        if (folder.empty()) {
            continue;
        }
        // A product's files share a few folders; each is judged once.
        auto [known, added] = roots.try_emplace(platform::to_lower(folder));
        if (added && !(!windows.empty() && platform::path_is_under(folder, windows)) &&
            !is_protected(folder)) {
            std::wstring root = folder;
            for (std::wstring parent = platform::parent_path(root);
                 !parent.empty() && !platform::is_root_path(parent) && !is_protected(parent);
                 parent = platform::parent_path(root)) {
                root = parent;
            }
            known->second = platform::to_lower(root);
        }
        if (!known->second.empty()) {
            by_root[known->second].push_back(folder);
        }
    }
    const auto most = std::max_element(by_root.begin(), by_root.end(), [](const auto& a, const auto& b) {
        return a.second.size() < b.second.size();
    });
    if (most == by_root.end()) {
        return {};
    }
    // The deepest folder holding all of that program folder's files.
    std::wstring common = most->second.front();
    for (const auto& folder : most->second) {
        while (!common.empty() && !platform::path_is_under(folder, common)) {
            common = platform::parent_path(common);
        }
    }
    const auto name = platform::last_path_component(common);
    if (common.empty() || is_protected(common) || match_product_name(target.names, name) < 0 ||
        item_name_equals_company_name(target.names, name)) {
        return {};
    }
    return common;
}

}  // namespace avel::leftovers::detail
