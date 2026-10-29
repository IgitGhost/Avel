#include "core/tracker/trace_items.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <set>

#include "core/inventory/msi_codes.h"
#include "core/leftovers/product_name.h"
#include "core/platform/known_folders.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::tracker {

namespace {

using leftovers::FileLocation;
using leftovers::Location;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using platform::RegistryAddress;

constexpr int kRounds = 3;
constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;
// A created key is read this deep, and for at most this many strings, for its references.
constexpr int kMaxDepth = 16;
constexpr std::size_t kMaxStrings = 4096;

std::vector<std::wstring_view> components(std::wstring_view path) {
    std::vector<std::wstring_view> parts;
    while (!path.empty()) {
        const std::size_t separator = path.find(L'\\');
        parts.push_back(path.substr(0, separator));
        if (separator == std::wstring_view::npos) {
            break;
        }
        path.remove_prefix(separator + 1);
    }
    return parts;
}

bool key_is_under(const RegistryAddress& key, const RegistryAddress& ancestor) {
    return key.hive == ancestor.hive && platform::iequals(key.sid, ancestor.sid) &&
           platform::istarts_with(key.path, ancestor.path) &&
           (key.path.size() == ancestor.path.size() || key.path[ancestor.path.size()] == L'\\');
}

const RegistryAddress* key_of(const Location& location) {
    if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        return &key->key;
    }
    if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        return &value->key;
    }
    return nullptr;
}

// The name a created root is known by: a file's or folder's, a key's, a value's, a service's or a
// task's own name.
std::wstring_view name_of(const Location& location) {
    if (const auto* file = std::get_if<FileLocation>(&location)) {
        return platform::last_path_component(file->path);
    }
    if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        return platform::last_path_component(key->key.path);
    }
    if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        return value->value_name;
    }
    if (const auto* service = std::get_if<leftovers::ServiceLocation>(&location)) {
        return service->name;
    }
    if (const auto* task = std::get_if<leftovers::TaskLocation>(&location)) {
        return platform::last_path_component(task->path);
    }
    return {};
}

struct OwnFolder {
    std::wstring path;
    TieReason reason;
};

class Tying {
public:
    Tying(const TraceResult& result, const ProgramPlaces& places, const TieSources& sources)
        : result_(result), places_(places), sources_(sources) {
        const auto& target = places.target;
        add_own(target.install_location, TieReason::InstallFolder);
        add_own(target.uninstaller_folder, TieReason::UninstallerFolder);
        add_own(places.icon_folder, TieReason::IconFolder);
        if (!target.msi_product_code.empty()) {
            if (auto packed = inventory::pack_guid(target.msi_product_code)) {
                packed_ = std::move(*packed);
                braced_ = target.msi_product_code;
            }
        }
        publisher_names_.name = target.names.publisher;
    }

    std::vector<TiedItem> run() {
        for (std::size_t i = 0; i < result_.changes.size(); ++i) {
            if (result_.changes[i].noise == NoiseReason::None) {
                TiedItem item;
                item.change = i;
                item.reason = direct_reason(result_.changes[i]);
                item.tied = item.reason != TieReason::None;
                items_.push_back(item);
            }
        }
        strings_.resize(items_.size());
        for (int round = 1; round <= kRounds; ++round) {
            // What was tied before this round; what this round ties counts from the next one.
            const Anchors anchors = collect_anchors();
            std::vector<std::pair<std::size_t, TiedItem>> tied_now;
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].tied) {
                    continue;
                }
                if (auto found = tie_by_anchors(i, anchors)) {
                    found->round = round;
                    tied_now.emplace_back(i, *found);
                }
            }
            if (tied_now.empty()) {
                break;
            }
            for (auto& [i, item] : tied_now) {
                items_[i] = item;
            }
        }
        return std::move(items_);
    }

private:
    struct Anchors {
        struct Folder {
            std::wstring path;
            bool directory = true;
            std::optional<std::size_t> via;  // The tied change, or nullopt for an entry's own folder
        };
        std::vector<Folder> files;
        struct ClassName {
            std::wstring name;  // A CLSID "{...}" or a ProgID
            std::size_t via;
        };
        std::vector<ClassName> classes;
        std::vector<std::size_t> tied;  // Changes tied so far
    };

    void add_own(const std::wstring& folder, TieReason reason) {
        if (folder.empty() || !platform::is_absolute_path(folder) || platform::is_root_path(folder)) {
            return;
        }
        const std::wstring path = platform::normalize_path(folder);
        for (const auto& broad : sources_.broad_folders) {
            if (platform::paths_equal(path, broad)) {
                return;
            }
        }
        own_.push_back({path, reason});
    }

    TieReason direct_reason(const TraceChange& change) const {
        const Location& location = change.location;
        if (const auto* file = std::get_if<FileLocation>(&location)) {
            for (const auto& own : own_) {
                if (platform::path_is_under(file->path, own.path)) {
                    return own.reason;
                }
            }
        } else if (const auto* key = key_of(location)) {
            if (places_.target.uninstall_key && key_is_under(*key, *places_.target.uninstall_key)) {
                return TieReason::UninstallKey;
            }
            if (is_msi_registration(location)) {
                return TieReason::MsiRegistration;
            }
        }
        if (change.change == ChangeKind::Created) {
            return named_after(location);
        }
        return TieReason::None;
    }

    // Windows Installer's keys for the product: ...\Installer\Products\<packed> and Features\<packed>
    // (also below UserData\<sid> and Managed\<sid>), Burn's Dependencies\{code}, and the product's
    // value in an UpgradeCodes or Components key.
    bool is_msi_registration(const Location& location) const {
        if (packed_.empty()) {
            return false;
        }
        const RegistryAddress* key = key_of(location);
        const auto parts = components(key->path);
        bool installer = false;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            installer = installer || platform::iequals(parts[i], L"Installer");
            if (!installer) {
                continue;
            }
            if ((platform::iequals(parts[i], L"Products") || platform::iequals(parts[i], L"Features")) &&
                platform::iequals(parts[i + 1], packed_)) {
                return true;
            }
            if (platform::iequals(parts[i], L"Dependencies") && platform::iequals(parts[i + 1], braced_)) {
                return true;
            }
        }
        const auto* value = std::get_if<RegistryValueLocation>(&location);
        return value && installer && parts.size() >= 2 && platform::iequals(value->value_name, packed_) &&
               (platform::iequals(parts[parts.size() - 2], L"UpgradeCodes") ||
                platform::iequals(parts[parts.size() - 2], L"Components"));
    }

    TieReason named_after(const Location& location) const {
        const std::wstring_view name = name_of(location);
        if (name.empty()) {
            return TieReason::None;
        }
        const auto* file = std::get_if<FileLocation>(&location);
        const auto matches = [&](const leftovers::ProductNames& names) {
            return !names.name.empty() &&
                   (leftovers::match_product_name(names, name) >= 0 ||
                    (file && !file->directory &&
                     leftovers::match_product_name(names, leftovers::name_without_extension(name)) >= 0));
        };
        if (matches(places_.target.names)) {
            return TieReason::NamedAfterProgram;
        }
        if (matches(publisher_names_)) {
            return TieReason::NamedAfterPublisher;
        }
        return TieReason::None;
    }

    // What was tied so far, then the entry's own folders: a reference names the traced item when
    // it can.
    Anchors collect_anchors() const {
        Anchors anchors;
        for (const auto& item : items_) {
            if (!item.tied) {
                continue;
            }
            anchors.tied.push_back(item.change);
            const Location& location = result_.changes[item.change].location;
            if (const auto* file = std::get_if<FileLocation>(&location)) {
                anchors.files.push_back({file->path, file->directory, item.change});
            } else if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
                if (auto name = class_name(key->key)) {
                    anchors.classes.push_back({std::move(*name), item.change});
                }
            }
        }
        for (const auto& own : own_) {
            anchors.files.push_back({own.path, true, std::nullopt});
        }
        return anchors;
    }

    // The class a key below Classes is, for references by name: "{CLSID}" for Classes\CLSID\{...}
    // (and its WOW6432Node twin), the ProgID for Classes\<ProgID>. Shared containers (file types,
    // `*`, Directory, ...) aren't classes.
    static std::optional<std::wstring> class_name(const RegistryAddress& key) {
        static constexpr std::array kContainers{L"Interface",
                                                L"TypeLib",
                                                L"AppID",
                                                L"WOW6432Node",
                                                L"*",
                                                L"Directory",
                                                L"Folder",
                                                L"Drive",
                                                L"Applications",
                                                L"SystemFileAssociations",
                                                L"AllFilesystemObjects",
                                                L"DesktopBackground",
                                                L"Installer",
                                                L"Local Settings",
                                                L"MIME",
                                                L"PROTOCOLS",
                                                L"Unknown",
                                                L"Network",
                                                L"Printers",
                                                L"LibraryFolder"};
        const auto parts = components(key.path);
        std::size_t classes = parts.size();
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (platform::iequals(parts[i], L"Classes")) {
                classes = i;
            }
        }
        if (classes + 1 >= parts.size()) {
            return std::nullopt;
        }
        std::size_t at = classes + 1;
        if (platform::iequals(parts[at], L"WOW6432Node") && at + 1 < parts.size()) {
            ++at;
        }
        if (platform::iequals(parts[at], L"CLSID")) {
            if (at + 2 == parts.size() && parts[at + 1].starts_with(L'{')) {
                return std::wstring(parts[at + 1]);
            }
            return std::nullopt;
        }
        if (at + 1 != parts.size() || parts[at].starts_with(L'.') ||
            std::any_of(kContainers.begin(), kContainers.end(),
                        [&](const wchar_t* container) { return platform::iequals(parts[at], container); })) {
            return std::nullopt;
        }
        return std::wstring(parts[at]);
    }

    std::optional<TiedItem> tie_by_anchors(std::size_t index, const Anchors& anchors) {
        TiedItem item = items_[index];
        const Location& location = result_.changes[item.change].location;
        for (const std::size_t tied : anchors.tied) {
            if (tied != item.change && leftovers::contains(result_.changes[tied].location, location)) {
                item.tied = true;
                item.reason = TieReason::InsideTied;
                item.via = tied;
                return item;
            }
        }
        if (anchors.files.empty() && anchors.classes.empty()) {
            return std::nullopt;
        }
        // A value's name can be a class's: an "Approved" shell extension is named by its CLSID.
        if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
            if (const auto via = class_reference(value->value_name, anchors)) {
                item.tied = true;
                item.reason = TieReason::ReferencesTied;
                item.via = via;
                return item;
            }
        }
        for (const auto& text : strings_of(index)) {
            if (const auto via = class_reference(text, anchors)) {
                item.tied = true;
                item.reason = TieReason::ReferencesTied;
                item.via = via;
                return item;
            }
            for (const auto& path : paths_in(text)) {
                for (const auto& anchor : anchors.files) {
                    if (anchor.directory ? platform::path_is_under(path, anchor.path)
                                         : platform::paths_equal(path, anchor.path)) {
                        item.tied = true;
                        item.reason = TieReason::ReferencesTied;
                        item.via = anchor.via;
                        return item;
                    }
                }
            }
        }
        return std::nullopt;
    }

    static std::optional<std::size_t> class_reference(std::wstring_view text, const Anchors& anchors) {
        const std::wstring_view name = platform::trim(text);
        if (name.empty()) {
            return std::nullopt;
        }
        for (const auto& item : anchors.classes) {
            if (platform::iequals(name, item.name)) {
                return item.via;
            }
        }
        return std::nullopt;
    }

    // The files and folders a string names: as a command line (referenced_paths), and as a path of
    // its own ("C:\App", "C:\App\app.exe,0" for an icon).
    std::vector<std::wstring> paths_in(const std::wstring& text) const {
        std::vector<std::wstring> paths =
            platform::referenced_paths(text, sources_.expand, sources_.file_exists);
        std::wstring whole(
            platform::trim(sources_.expand ? sources_.expand(text) : platform::expand_environment(text)));
        if (whole.size() >= 2 && whole.front() == L'"' && whole.back() == L'"') {
            whole = whole.substr(1, whole.size() - 2);
        }
        if (const std::size_t comma = whole.find_last_of(L',');
            comma != std::wstring::npos && whole.find_first_of(L"\\/", comma) == std::wstring::npos) {
            const std::wstring_view index = std::wstring_view(whole).substr(comma + 1);
            if (!index.empty() && std::all_of(index.begin(), index.end(),
                                              [](wchar_t c) { return std::iswdigit(c) || c == L'-'; })) {
                whole.resize(comma);
            }
        }
        if (platform::is_absolute_path(whole)) {
            paths.push_back(platform::normalize_path(whole));
        }
        return paths;
    }

    const std::vector<std::wstring>& strings_of(std::size_t index) {
        auto& cached = strings_[index];
        if (!cached) {
            cached = sources_.strings ? sources_.strings(result_.changes[items_[index].change].location)
                                      : std::vector<std::wstring>();
        }
        return *cached;
    }

    const TraceResult& result_;
    const ProgramPlaces& places_;
    const TieSources& sources_;
    std::vector<OwnFolder> own_;
    std::wstring packed_;  // The product code packed as Windows Installer's keys name it
    std::wstring braced_;
    leftovers::ProductNames publisher_names_;  // The publisher as a name to match
    std::vector<TiedItem> items_;
    std::vector<std::optional<std::vector<std::wstring>>> strings_;  // Per item, read once
};

// Every string a value holds: REG_SZ and REG_EXPAND_SZ whole, a REG_MULTI_SZ string by string.
void add_strings(const platform::RegistryValue& value, std::vector<std::wstring>& strings) {
    if (value.type != REG_SZ && value.type != REG_EXPAND_SZ && value.type != REG_MULTI_SZ) {
        return;
    }
    std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
    if (!text.empty()) {
        std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
    }
    if (value.type != REG_MULTI_SZ) {
        text.resize(std::wcslen(text.c_str()));
        if (!text.empty()) {
            strings.push_back(std::move(text));
        }
        return;
    }
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(L'\0', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        if (end > start) {
            strings.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
}

LSTATUS open_live(const RegistryAddress& address, platform::RegistryKey& key) {
    if (address.hive == platform::Hive::Machine) {
        return platform::RegistryKey::open(HKEY_LOCAL_MACHINE, address.path, kRead, key);
    }
    return platform::RegistryKey::open(HKEY_USERS, address.sid + L"\\" + address.path, kRead, key);
}

void key_strings(const platform::RegistryKey& key, int depth, std::vector<std::wstring>& strings) {
    for (const auto& value : key.values()) {
        add_strings(value, strings);
    }
    if (depth >= kMaxDepth || strings.size() >= kMaxStrings) {
        return;
    }
    for (const auto& name : key.subkey_names()) {
        platform::RegistryKey child;
        if (platform::RegistryKey::open(key.get(), name, kRead, child) == ERROR_SUCCESS) {
            key_strings(child, depth + 1, strings);
        }
        if (strings.size() >= kMaxStrings) {
            return;
        }
    }
}

// A task's Exec actions as command lines, from the Task Scheduler.
std::vector<std::wstring> task_commands(const std::wstring& path) {
    std::vector<std::wstring> commands;
    wil::com_ptr_nothrow<ITaskService> service;
    const VARIANT empty{};
    wil::com_ptr_nothrow<ITaskFolder> root;
    wil::com_ptr_nothrow<IRegisteredTask> task;
    wil::com_ptr_nothrow<ITaskDefinition> definition;
    wil::com_ptr_nothrow<IActionCollection> actions;
    const auto root_path = wil::make_bstr_nothrow(L"\\");
    const auto task_path = wil::make_bstr_nothrow(path.c_str());
    if (!root_path || !task_path ||
        FAILED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(service.put()))) ||
        FAILED(service->Connect(empty, empty, empty, empty)) ||
        FAILED(service->GetFolder(root_path.get(), root.put())) ||
        FAILED(root->GetTask(task_path.get(), task.put())) ||
        FAILED(task->get_Definition(definition.put())) || FAILED(definition->get_Actions(actions.put()))) {
        return commands;
    }
    LONG count = 0;
    actions->get_Count(&count);
    for (LONG i = 1; i <= count; ++i) {
        wil::com_ptr_nothrow<IAction> action;
        TASK_ACTION_TYPE type{};
        if (FAILED(actions->get_Item(i, action.put())) || FAILED(action->get_Type(&type)) ||
            type != TASK_ACTION_EXEC) {
            continue;
        }
        const auto exec = action.try_query<IExecAction>();
        wil::unique_bstr program;
        wil::unique_bstr arguments;
        if (!exec || FAILED(exec->get_Path(&program)) || !program) {
            continue;
        }
        exec->get_Arguments(&arguments);
        std::wstring command(platform::trim(program.get()));
        if (command.find(L' ') != std::wstring::npos && command.front() != L'"') {
            command = L"\"" + command + L"\"";
        }
        if (arguments && *arguments.get()) {
            command += L" " + std::wstring(arguments.get());
        }
        commands.push_back(std::move(command));
    }
    return commands;
}

std::vector<std::wstring> live_strings(const Location& location) {
    std::vector<std::wstring> strings;
    if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        platform::RegistryKey opened;
        if (open_live(key->key, opened) == ERROR_SUCCESS) {
            key_strings(opened, 0, strings);
        }
    } else if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        platform::RegistryKey opened;
        if (open_live(value->key, opened) == ERROR_SUCCESS) {
            const auto values = opened.values();
            if (const auto* found = platform::find_value(values, value->value_name)) {
                add_strings(*found, strings);
            }
        }
    } else if (const auto* service = std::get_if<leftovers::ServiceLocation>(&location)) {
        platform::RegistryKey opened;
        const std::wstring path = LR"(SYSTEM\CurrentControlSet\Services\)" + service->name;
        if (platform::RegistryKey::open(HKEY_LOCAL_MACHINE, path, kRead, opened) == ERROR_SUCCESS) {
            const auto values = opened.values();
            if (const auto* image = platform::find_value(values, L"ImagePath")) {
                add_strings(*image, strings);
            }
            platform::RegistryKey parameters;
            if (platform::RegistryKey::open(opened.get(), L"Parameters", kRead, parameters) ==
                ERROR_SUCCESS) {
                const auto parameter_values = parameters.values();
                if (const auto* dll = platform::find_value(parameter_values, L"ServiceDll")) {
                    add_strings(*dll, strings);
                }
            }
        }
    } else if (const auto* task = std::get_if<leftovers::TaskLocation>(&location)) {
        strings = task_commands(task->path);
    } else if (const auto* rule = std::get_if<leftovers::FirewallRuleLocation>(&location)) {
        // "v2.31|Action=Allow|...|App=C:\path\app.exe|Name=...|": its program.
        platform::RegistryKey rules;
        if (platform::RegistryKey::open(
                HKEY_LOCAL_MACHINE,
                LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)",
                kRead, rules) == ERROR_SUCCESS) {
            const auto values = rules.values();
            if (const auto* found = platform::find_value(values, rule->rule_id)) {
                const std::wstring text = found->as_string().value_or(L"");
                if (const std::size_t start = text.find(L"|App="); start != std::wstring::npos) {
                    const std::size_t end = text.find(L'|', start + 5);
                    strings.push_back(
                        text.substr(start + 5, end == std::wstring::npos ? end : end - start - 5));
                }
            }
        }
    }
    return strings;
}

}  // namespace

std::string_view to_string(TieReason reason) {
    switch (reason) {
        case TieReason::None:
            return "none";
        case TieReason::InstallFolder:
            return "install-folder";
        case TieReason::UninstallerFolder:
            return "uninstaller-folder";
        case TieReason::IconFolder:
            return "icon-folder";
        case TieReason::UninstallKey:
            return "uninstall-key";
        case TieReason::MsiRegistration:
            return "msi-registration";
        case TieReason::NamedAfterProgram:
            return "named-after-program";
        case TieReason::NamedAfterPublisher:
            return "named-after-publisher";
        case TieReason::InsideTied:
            return "inside-tied";
        case TieReason::ReferencesTied:
            return "references-tied";
    }
    return "unknown";
}

ProgramPlaces program_places(const inventory::Program& program) {
    ProgramPlaces places;
    places.target = leftovers::make_scan_target(program);
    std::wstring icon = program.display_icon;
    if (const std::size_t comma = icon.find_last_of(L',');
        comma != std::wstring::npos && icon.find_first_of(L"\\/", comma) == std::wstring::npos) {
        icon.resize(comma);  // "path,index"
    }
    icon = std::wstring(platform::trim(icon));
    if (icon.size() >= 2 && icon.front() == L'"' && icon.back() == L'"') {
        icon = icon.substr(1, icon.size() - 2);
    }
    if (platform::is_absolute_path(icon)) {
        std::wstring folder = platform::parent_path(platform::long_path(icon));
        wchar_t windows[MAX_PATH] = {};
        const UINT length = ::GetWindowsDirectoryW(windows, MAX_PATH);
        if (!folder.empty() && (length == 0 || length >= MAX_PATH ||
                                !platform::path_is_under(folder, std::wstring_view(windows, length)))) {
            places.icon_folder = std::move(folder);
        }
    }
    return places;
}

TieSources live_tie_sources() {
    TieSources sources;
    sources.strings = live_strings;
    const auto machine = platform::machine_folders();
    sources.broad_folders = machine.all;
    for (const auto* folder : {&machine.windows, &machine.program_files, &machine.program_files_x86,
                               &machine.program_data, &machine.users}) {
        if (!folder->empty()) {
            sources.broad_folders.push_back(*folder);
        }
    }
    for (const auto& profile : platform::enumerate_user_profiles()) {
        sources.broad_folders.push_back(profile.profile_path);
        for (const wchar_t* folder : {LR"(AppData\Local)", LR"(AppData\Roaming)", LR"(AppData\LocalLow)",
                                      LR"(AppData\Local\Programs)", L"Desktop", L"Documents", L"Downloads"}) {
            sources.broad_folders.push_back(profile.profile_path + L"\\" + folder);
        }
    }
    return sources;
}

std::vector<TiedItem> tie_trace_items(const TraceResult& result, const ProgramPlaces& places,
                                      const TieSources& sources) {
    return Tying(result, places, sources).run();
}

}  // namespace avel::tracker
