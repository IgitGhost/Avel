#include "core/tools/browser_extensions.h"

#include <windows.h>
#include <lm.h>

#include <wil/resource.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

#include "core/force/force.h"
#include "core/platform/hash.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::tools {

namespace {

using Json = nlohmann::json;
using leftovers::UserScope;
using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;

// Chromium's extension locations (extensions::mojom::ManifestLocation).
enum ChromiumLocation : int {
    kInternal = 1,
    kExternalPref = 2,
    kExternalRegistry = 3,
    kUnpacked = 4,
    kComponent = 5,
    kExternalPrefDownload = 6,
    kExternalPolicyDownload = 7,
    kCommandLine = 8,
    kExternalPolicy = 9,
    kExternalComponent = 10,
};

// Microseconds from 1601 (Chromium's install_time) to 1970.
constexpr std::int64_t kWindowsToUnixMs = 11644473600000LL;

constexpr std::array<BrowserKind, 4> kChromeChannels = {BrowserKind::Chrome, BrowserKind::ChromeBeta,
                                                        BrowserKind::ChromeDev, BrowserKind::ChromeCanary};
constexpr std::array<BrowserKind, 4> kEdgeChannels = {BrowserKind::Edge, BrowserKind::EdgeBeta,
                                                      BrowserKind::EdgeDev, BrowserKind::EdgeCanary};

struct ChromiumBrowser {
    BrowserKind kind;
    bool roaming;            // Under %APPDATA% rather than %LOCALAPPDATA%
    bool single_profile;     // The folder is the profile (Opera)
    std::wstring_view data;  // The User Data folder (or Opera's profile), below the AppData folder
    // The program, below %LOCALAPPDATA%, Program Files and Program Files (x86)
    std::array<std::wstring_view, 2> programs;
    std::wstring_view app_path;  // Its App Paths name, for the stable channels
    std::wstring_view url;
};

constexpr std::array<ChromiumBrowser, 13> kChromiumBrowsers = {{
    {BrowserKind::Chrome,
     false,
     false,
     LR"(Google\Chrome\User Data)",
     {LR"(Google\Chrome\Application\chrome.exe)"},
     L"chrome.exe",
     L"chrome://extensions"},
    {BrowserKind::ChromeBeta,
     false,
     false,
     LR"(Google\Chrome Beta\User Data)",
     {LR"(Google\Chrome Beta\Application\chrome.exe)"},
     L"",
     L"chrome://extensions"},
    {BrowserKind::ChromeDev,
     false,
     false,
     LR"(Google\Chrome Dev\User Data)",
     {LR"(Google\Chrome Dev\Application\chrome.exe)"},
     L"",
     L"chrome://extensions"},
    {BrowserKind::ChromeCanary,
     false,
     false,
     LR"(Google\Chrome SxS\User Data)",
     {LR"(Google\Chrome SxS\Application\chrome.exe)"},
     L"",
     L"chrome://extensions"},
    {BrowserKind::Edge,
     false,
     false,
     LR"(Microsoft\Edge\User Data)",
     {LR"(Microsoft\Edge\Application\msedge.exe)"},
     L"msedge.exe",
     L"edge://extensions"},
    {BrowserKind::EdgeBeta,
     false,
     false,
     LR"(Microsoft\Edge Beta\User Data)",
     {LR"(Microsoft\Edge Beta\Application\msedge.exe)"},
     L"",
     L"edge://extensions"},
    {BrowserKind::EdgeDev,
     false,
     false,
     LR"(Microsoft\Edge Dev\User Data)",
     {LR"(Microsoft\Edge Dev\Application\msedge.exe)"},
     L"",
     L"edge://extensions"},
    {BrowserKind::EdgeCanary,
     false,
     false,
     LR"(Microsoft\Edge SxS\User Data)",
     {LR"(Microsoft\Edge SxS\Application\msedge.exe)"},
     L"",
     L"edge://extensions"},
    {BrowserKind::Brave,
     false,
     false,
     LR"(BraveSoftware\Brave-Browser\User Data)",
     {LR"(BraveSoftware\Brave-Browser\Application\brave.exe)"},
     L"brave.exe",
     L"brave://extensions"},
    {BrowserKind::Vivaldi,
     false,
     false,
     LR"(Vivaldi\User Data)",
     {LR"(Vivaldi\Application\vivaldi.exe)"},
     L"vivaldi.exe",
     L"vivaldi://extensions"},
    {BrowserKind::Opera,
     true,
     true,
     LR"(Opera Software\Opera Stable)",
     {LR"(Programs\Opera\opera.exe)", LR"(Opera\opera.exe)"},
     L"opera.exe",
     L"opera://extensions"},
    {BrowserKind::OperaGx,
     true,
     true,
     LR"(Opera Software\Opera GX Stable)",
     {LR"(Programs\Opera GX\opera.exe)", LR"(Opera GX\opera.exe)"},
     L"",
     L"opera://extensions"},
    {BrowserKind::Chromium,
     false,
     false,
     LR"(Chromium\User Data)",
     {LR"(Chromium\Application\chrome.exe)"},
     L"",
     L"chrome://extensions"},
}};

// Where Chromium browsers read their policies: Software\Policies\<vendor>. Opera reads none that is
// documented (see docs/tools.md); Vivaldi's is from its forum, not its documentation.
struct PolicyVendor {
    std::wstring_view path;
    std::vector<BrowserKind> browsers;
};

const std::vector<PolicyVendor>& policy_vendors() {
    static const std::vector<PolicyVendor> vendors = {
        {LR"(Google\Chrome)", {kChromeChannels.begin(), kChromeChannels.end()}},
        {LR"(Microsoft\Edge)", {kEdgeChannels.begin(), kEdgeChannels.end()}},
        {LR"(BraveSoftware\Brave)", {BrowserKind::Brave}},
        {L"Chromium", {BrowserKind::Chromium}},
        {L"Vivaldi", {BrowserKind::Vivaldi}},
    };
    return vendors;
}

// Chromium's external registration keys: Software\<vendor>\Extensions. Chromium itself reads
// Google\Chrome's (hard-coded upstream), Edge its own; the others are the vendors' likely forks.
const std::vector<PolicyVendor>& registration_vendors() {
    static const std::vector<PolicyVendor> vendors = [] {
        std::vector<BrowserKind> chrome(kChromeChannels.begin(), kChromeChannels.end());
        chrome.push_back(BrowserKind::Chromium);
        return std::vector<PolicyVendor>{
            {LR"(Google\Chrome\Extensions)", chrome},
            {LR"(Microsoft\Edge\Extensions)", {kEdgeChannels.begin(), kEdgeChannels.end()}},
            {LR"(BraveSoftware\Brave-Browser\Extensions)", {BrowserKind::Brave}},
            {LR"(Chromium\Extensions)", {BrowserKind::Chromium}},
            {LR"(Vivaldi\Extensions)", {BrowserKind::Vivaldi}},
        };
    }();
    return vendors;
}

constexpr std::array<std::wstring_view, 4> kFirefoxBuiltIn = {L"app-builtin", L"app-builtin-addons",
                                                              L"app-system-defaults", L"app-system-addons"};
constexpr std::array<std::wstring_view, 3> kFirefoxPrograms = {
    L"Mozilla Firefox", L"Firefox Developer Edition", L"Firefox Nightly"};

// What the list keeps about an entry until the sources are matched.
struct Pending {
    std::wstring source_uri;  // Firefox's sourceURI, lowercase, %XX decoded
};

struct Collector {
    explicit Collector(const BrowserExtensionsEnvironment& environment) : environment(environment) {}

    const BrowserExtensionsEnvironment& environment;
    BrowserExtensionsList list;
    std::vector<Pending> pending;      // One per entry
    std::set<std::wstring> seen_keys;  // Kernel names of the keys read, lowercase
};

// ---- Files and JSON ----

std::optional<std::string> read_text(const std::wstring& path, DWORD& error) {
    wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        error = ::GetLastError();
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart > 256LL * 1024 * 1024) {
        error = ERROR_FILE_TOO_LARGE;
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!text.empty() &&
        !::ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
        error = ::GetLastError();
        return std::nullopt;
    }
    text.resize(read);
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    error = ERROR_SUCCESS;
    return text;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool folder_exists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

enum class JsonRead { Missing, Unreadable, Ok };

// A JSON file; comments are tolerated, as browsers tolerate them in their own files.
JsonRead read_json(const std::wstring& path, Json& json, DWORD& error) {
    const auto text = read_text(path, error);
    if (!text) {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? JsonRead::Missing
                                                                              : JsonRead::Unreadable;
    }
    json = Json::parse(*text, nullptr, false, true);
    if (json.is_discarded()) {
        error = ERROR_INVALID_DATA;
        return JsonRead::Unreadable;
    }
    return JsonRead::Ok;
}

const Json* member(const Json& json, const char* name) {
    if (!json.is_object()) {
        return nullptr;
    }
    const auto found = json.find(name);
    return found != json.end() ? &*found : nullptr;
}

std::wstring text_of(const Json& json, const char* name) {
    const Json* value = member(json, name);
    return value && value->is_string() ? platform::to_utf16(value->get_ref<const std::string&>())
                                       : std::wstring();
}

std::optional<std::int64_t> integer_of(const Json& json, const char* name) {
    const Json* value = member(json, name);
    if (!value) {
        return std::nullopt;
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>();
    }
    if (value->is_number_unsigned()) {
        return static_cast<std::int64_t>(value->get<std::uint64_t>());
    }
    if (value->is_string()) {  // Chromium writes 64-bit numbers as strings
        const std::string& text = value->get_ref<const std::string&>();
        char* end = nullptr;
        const long long number = std::strtoll(text.c_str(), &end, 10);
        if (!text.empty() && end && *end == '\0') {
            return number;
        }
    }
    return std::nullopt;
}

bool bool_of(const Json& json, const char* name, bool fallback = false) {
    const Json* value = member(json, name);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

std::vector<std::wstring> strings_of(const Json* json) {
    std::vector<std::wstring> list;
    if (json && json->is_array()) {
        for (const auto& item : *json) {
            if (item.is_string()) {
                list.push_back(platform::to_utf16(item.get_ref<const std::string&>()));
            }
        }
    }
    return list;
}

void append(std::vector<std::wstring>& to, std::vector<std::wstring> from) {
    for (auto& item : from) {
        if (std::none_of(to.begin(), to.end(), [&](const std::wstring& known) { return known == item; })) {
            to.push_back(std::move(item));
        }
    }
}

// "%41" becomes "A", for comparing file URLs; other characters stay.
std::wstring percent_decode(std::wstring_view text) {
    std::string bytes;
    const std::string utf8 = platform::to_utf8(text);
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '%' && i + 2 < utf8.size() && std::isxdigit(static_cast<unsigned char>(utf8[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(utf8[i + 2]))) {
            bytes.push_back(static_cast<char>(std::strtol(utf8.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            bytes.push_back(utf8[i]);
        }
    }
    return platform::to_utf16(bytes);
}

// A Firefox Install entry as Firefox records its sourceURI: a URL as it is, a path as a file URL.
std::wstring uri_key(std::wstring_view text) {
    std::wstring value(platform::trim(text));
    if (platform::is_absolute_path(value)) {
        value = L"file:///" + platform::normalize_path(value);
        std::replace(value.begin(), value.end(), L'\\', L'/');
    }
    return platform::to_lower(percent_decode(value));
}

std::wstring hash_id(std::wstring_view identity) {
    const std::string utf8 = platform::to_utf8(platform::to_lower(identity));
    return std::format(L"{:010x}", platform::fnv1a(utf8.data(), utf8.size()) >> 24);
}

bool contains_browser(const std::vector<BrowserKind>& list, BrowserKind browser) {
    return std::find(list.begin(), list.end(), browser) != list.end();
}

// ---- Programs and pages ----

const ChromiumBrowser* chromium_spec(BrowserKind kind) {
    const auto found = std::find_if(kChromiumBrowsers.begin(), kChromiumBrowsers.end(),
                                    [&](const ChromiumBrowser& spec) { return spec.kind == kind; });
    return found != kChromiumBrowsers.end() ? &*found : nullptr;
}

std::wstring app_path(HKEY root, std::wstring_view software, std::wstring_view name) {
    platform::RegistryKey key;
    if (!root || name.empty() ||
        platform::RegistryKey::open(
            root,
            std::wstring(software) + LR"(\Microsoft\Windows\CurrentVersion\App Paths\)" + std::wstring(name),
            kRead, key) != ERROR_SUCCESS) {
        return {};
    }
    const auto values = key.values();
    const auto* value = platform::find_value(values, L"");
    const auto text = value ? value->as_string() : std::nullopt;
    if (!text) {
        return {};
    }
    std::wstring path = platform::normalize_path(platform::expand_environment(*text));
    return file_exists(path) ? path : std::wstring();
}

// The browser's program for `user` (their own install first), else the machine's; empty if none.
std::wstring find_program(const Collector& collector, BrowserKind browser, const UserScope* user) {
    const auto& folders = collector.environment.scan.machine_folders;
    std::vector<std::wstring> candidates;
    std::wstring_view app_name;
    if (browser == BrowserKind::Firefox) {
        if (collector.environment.firefox_folders) {
            for (const auto& folder : *collector.environment.firefox_folders) {
                candidates.push_back(folder + L"\\firefox.exe");
            }
        } else {
            for (const auto& base : {folders.program_files, folders.program_files_x86}) {
                for (const auto name : kFirefoxPrograms) {
                    if (!base.empty()) {
                        candidates.push_back(base + L"\\" + std::wstring(name) + L"\\firefox.exe");
                    }
                }
            }
        }
        app_name = L"firefox.exe";
    } else if (const auto* spec = chromium_spec(browser)) {
        for (const auto& base : {user ? user->folders.local_app_data : std::wstring(), folders.program_files,
                                 folders.program_files_x86}) {
            for (const auto relative : spec->programs) {
                if (!base.empty() && !relative.empty()) {
                    candidates.push_back(base + L"\\" + std::wstring(relative));
                }
            }
        }
        app_name = spec->app_path;
    }
    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            return platform::normalize_path(candidate);
        }
    }
    if (user) {
        if (auto path = app_path(user->hive, L"Software", app_name); !path.empty()) {
            return path;
        }
    }
    return app_path(collector.environment.scan.machine, L"SOFTWARE", app_name);
}

ExtensionsPage make_page(const Collector& collector, BrowserKind browser, const UserScope* user,
                         const std::wstring& directory, const std::wstring& profile_path) {
    ExtensionsPage page;
    page.executable = find_program(collector, browser, user);
    if (browser == BrowserKind::Firefox) {
        page.url = L"about:addons";
        if (!profile_path.empty()) {
            page.arguments = {L"-profile", profile_path};
        }
    } else {
        page.url = std::wstring(chromium_spec(browser)->url);
        if (!directory.empty()) {
            page.arguments = {L"--profile-directory=" + directory};
        }
    }
    page.arguments.push_back(page.url);
    return page;
}

const UserScope* user_with_sid(const Collector& collector, const std::wstring& sid) {
    for (const auto& user : collector.environment.scan.users) {
        if (!sid.empty() && platform::iequals(user.profile.sid, sid)) {
            return &user;
        }
    }
    return nullptr;
}

// ---- Chromium profiles ----

// Chromium's "__MSG_name__" strings, from the default locale's messages.json.
class Messages {
public:
    Messages(std::wstring folder, std::wstring locale)
        : folder_(std::move(folder)), locale_(std::move(locale)) {}

    std::wstring resolve(const std::wstring& text) {
        if (text.size() <= 8 || !platform::istarts_with(text, L"__MSG_") || !text.ends_with(L"__")) {
            return text;
        }
        load();
        const std::wstring key = platform::to_lower(text.substr(6, text.size() - 8));
        const auto found = messages_.find(key);
        return found != messages_.end() ? found->second : text;
    }

private:
    void load() {
        if (loaded_ || locale_.empty() || folder_.empty()) {
            loaded_ = true;
            return;
        }
        loaded_ = true;
        Json json;
        DWORD error = 0;
        if (read_json(folder_ + LR"(\_locales\)" + locale_ + LR"(\messages.json)", json, error) !=
                JsonRead::Ok ||
            !json.is_object()) {
            return;
        }
        for (const auto& [name, entry] : json.items()) {
            const std::wstring message = text_of(entry, "message");
            messages_.emplace(platform::to_lower(platform::to_utf16(name)), message);
        }
    }

    std::wstring folder_;
    std::wstring locale_;
    bool loaded_ = false;
    std::map<std::wstring, std::wstring> messages_;
};

bool is_host_pattern(std::wstring_view permission) {
    return permission == L"<all_urls>" || permission.find(L"://") != std::wstring_view::npos;
}

void add_entry(Collector& collector, BrowserExtension entry, Pending pending = {}) {
    collector.list.extensions.push_back(std::move(entry));
    collector.pending.push_back(std::move(pending));
}

std::uint32_t disable_reasons_of(const Json& settings) {
    const Json* reasons = member(settings, "disable_reasons");
    if (!reasons) {
        return 0;
    }
    if (reasons->is_number_integer() || reasons->is_number_unsigned()) {
        return static_cast<std::uint32_t>(reasons->get<std::int64_t>());
    }
    std::uint32_t bits = 0;
    if (reasons->is_array()) {  // Newer Chromium: a list of the reasons, each a bit of the old mask
        for (const auto& reason : *reasons) {
            if (reason.is_number_integer()) {
                bits |= static_cast<std::uint32_t>(reason.get<std::int64_t>());
            }
        }
    }
    return bits;
}

ExtensionStore browser_store(BrowserKind browser) {
    switch (browser) {
        case BrowserKind::Edge:
        case BrowserKind::EdgeBeta:
        case BrowserKind::EdgeDev:
        case BrowserKind::EdgeCanary:
            return ExtensionStore::EdgeAddons;
        case BrowserKind::Opera:
        case BrowserKind::OperaGx:
            return ExtensionStore::OperaAddons;
        default:
            return ExtensionStore::ChromeWebStore;
    }
}

void read_chromium_extension(Collector& collector, const BrowserProfile& profile, const std::string& id,
                             const Json& settings) {
    const auto location = integer_of(settings, "location");
    if (!location || *location <= 0 || *location == kComponent || *location == kExternalComponent ||
        integer_of(settings, "state") == 2) {  // 2: an external extension the user uninstalled (old Chromium)
        return;
    }
    const std::wstring relative = text_of(settings, "path");
    std::wstring folder;
    if (!relative.empty()) {
        folder = platform::normalize_path(
            platform::is_absolute_path(relative) ? relative : profile.path + LR"(\Extensions\)" + relative);
    }
    Json manifest;
    DWORD error = 0;
    if (folder.empty() || read_json(folder + L"\\manifest.json", manifest, error) != JsonRead::Ok ||
        !manifest.is_object()) {
        const Json* stored = member(settings, "manifest");  // Older Chromium kept it in the preferences
        manifest = stored && stored->is_object() ? *stored : Json();
    }
    if (folder.empty() && !manifest.is_object()) {
        return;  // Settings left behind for something no longer installed
    }
    if (member(manifest, "theme")) {
        return;
    }

    BrowserExtension entry;
    entry.browser = profile.browser;
    entry.user_sid = profile.user_sid;
    entry.user_name = profile.user_name;
    entry.profile_directory = profile.directory;
    entry.profile_name = profile.name;
    entry.profile_path = profile.path;
    entry.extension_id = platform::to_lower(platform::to_utf16(id));
    entry.folder = folder;
    Messages messages(folder, text_of(manifest, "default_locale"));
    entry.name = messages.resolve(text_of(manifest, "name"));
    entry.description = messages.resolve(text_of(manifest, "description"));
    entry.version = text_of(manifest, "version");
    entry.location = std::to_wstring(*location);
    entry.disable_reasons = disable_reasons_of(settings);
    const auto state = integer_of(settings, "state");
    entry.enabled = state ? *state == 1 : entry.disable_reasons == 0;
    entry.installed_by_default =
        bool_of(settings, "was_installed_by_default") || bool_of(settings, "was_installed_by_oem");
    entry.update_url = text_of(manifest, "update_url");
    entry.store = store_of_update_url(entry.update_url);
    if (entry.store == ExtensionStore::None && bool_of(settings, "from_webstore")) {
        entry.store = browser_store(profile.browser);
    }
    entry.from_store = entry.store != ExtensionStore::None;
    if (const auto time = integer_of(settings, "install_time"); time && *time > 0) {
        entry.install_time = *time / 1000 - kWindowsToUnixMs;
    }
    switch (*location) {
        case kInternal:
            entry.install = entry.from_store ? ExtensionInstall::Store : ExtensionInstall::User;
            break;
        case kExternalPref:
        case kExternalPrefDownload:
            entry.install = ExtensionInstall::ExternalPref;
            break;
        case kExternalRegistry:
            entry.install = ExtensionInstall::ExternalRegistry;
            break;
        case kUnpacked:
        case kCommandLine:
            entry.install = ExtensionInstall::Developer;
            break;
        case kExternalPolicyDownload:
        case kExternalPolicy:
            entry.install = ExtensionInstall::Policy;
            break;
        default:
            entry.install = ExtensionInstall::Unknown;
            break;
    }
    const auto manifest_version = integer_of(manifest, "manifest_version").value_or(2);
    for (auto& permission : strings_of(member(manifest, "permissions"))) {
        if (manifest_version < 3 && is_host_pattern(permission)) {
            append(entry.host_permissions, {std::move(permission)});
        } else {
            append(entry.permissions, {std::move(permission)});
        }
    }
    append(entry.host_permissions, strings_of(member(manifest, "host_permissions")));
    append(entry.optional_permissions, strings_of(member(manifest, "optional_permissions")));
    append(entry.optional_permissions, strings_of(member(manifest, "optional_host_permissions")));
    entry.page = profile.page;
    add_entry(collector, std::move(entry));
}

void read_chromium_profile(Collector& collector, const BrowserProfile& profile) {
    // extensions.settings is split between the two files (Secure Preferences holds it on most machines);
    // Secure Preferences wins where both have a field. Neither is ever written.
    Json merged = Json::object();
    for (const auto name : {L"Preferences", L"Secure Preferences"}) {
        const std::wstring path = profile.path + L"\\" + name;
        Json json;
        DWORD error = 0;
        const JsonRead read = read_json(path, json, error);
        if (read == JsonRead::Missing) {
            continue;
        }
        if (read == JsonRead::Unreadable) {
            collector.list.diagnostics.push_back(
                {ExtensionDiagnosticCode::ProfileUnreadable, path, static_cast<std::int32_t>(error)});
            continue;
        }
        const Json* extensions = member(json, "extensions");
        const Json* settings = extensions ? member(*extensions, "settings") : nullptr;
        if (!settings || !settings->is_object()) {
            continue;
        }
        for (const auto& [id, value] : settings->items()) {
            if (!value.is_object()) {
                continue;
            }
            Json& target = merged[id];
            if (!target.is_object()) {
                target = Json::object();
            }
            for (const auto& [field, data] : value.items()) {
                target[field] = data;
            }
        }
    }
    for (const auto& [id, settings] : merged.items()) {
        read_chromium_extension(collector, profile, id, settings);
    }
}

// The profiles of a User Data folder: Local State's info cache, else the folders with Preferences.
std::vector<std::pair<std::wstring, std::wstring>> chromium_profiles(Collector& collector,
                                                                     const std::wstring& root) {
    std::vector<std::pair<std::wstring, std::wstring>> profiles;  // Folder name, display name
    Json state;
    DWORD error = 0;
    const std::wstring local_state = root + L"\\Local State";
    const JsonRead read = read_json(local_state, state, error);
    if (read == JsonRead::Unreadable) {
        collector.list.diagnostics.push_back(
            {ExtensionDiagnosticCode::LocalStateUnreadable, local_state, static_cast<std::int32_t>(error)});
    }
    const Json* profile = read == JsonRead::Ok ? member(state, "profile") : nullptr;
    const Json* cache = profile ? member(*profile, "info_cache") : nullptr;
    if (cache && cache->is_object()) {
        for (const auto& [directory, info] : cache->items()) {
            const std::wstring name = platform::to_utf16(directory);
            if (folder_exists(root + L"\\" + name)) {
                profiles.emplace_back(name, text_of(info, "name"));
            }
        }
    }
    if (!profiles.empty()) {
        return profiles;
    }
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(::FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &data,
                                              FindExSearchLimitToDirectories, nullptr, 0));
    if (!find) {
        return profiles;
    }
    do {
        const std::wstring name = data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L".." &&
            !platform::iequals(name, L"System Profile") && !platform::iequals(name, L"Guest Profile") &&
            file_exists(root + L"\\" + name + L"\\Preferences")) {
            profiles.emplace_back(name, L"");
        }
    } while (::FindNextFileW(find.get(), &data));
    std::sort(profiles.begin(), profiles.end());
    return profiles;
}

void collect_chromium(Collector& collector, const UserScope& user) {
    for (const auto& spec : kChromiumBrowsers) {
        const std::wstring& base = spec.roaming ? user.folders.roaming_app_data : user.folders.local_app_data;
        if (base.empty()) {
            continue;
        }
        const std::wstring root = platform::normalize_path(base + L"\\" + std::wstring(spec.data));
        if (!folder_exists(root)) {
            continue;
        }
        std::vector<std::pair<std::wstring, std::wstring>> profiles;
        if (spec.single_profile) {
            profiles.emplace_back(L"", L"");
        } else {
            profiles = chromium_profiles(collector, root);
        }
        for (const auto& [directory, name] : profiles) {
            BrowserProfile profile;
            profile.browser = spec.kind;
            profile.user_sid = user.profile.sid;
            profile.user_name = user.profile.account_name;
            profile.directory = directory;
            profile.name = name.empty() ? directory : name;
            profile.path = directory.empty() ? root : root + L"\\" + directory;
            profile.page = make_page(collector, spec.kind, &user, directory, profile.path);
            read_chromium_profile(collector, profile);
            collector.list.profiles.push_back(std::move(profile));
        }
    }
}

// ---- Firefox profiles ----

struct IniProfile {
    std::wstring name;
    std::wstring path;
    bool relative = true;
};

std::vector<IniProfile> parse_profiles_ini(const std::string& text) {
    std::vector<IniProfile> profiles;
    IniProfile* current = nullptr;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::wstring line(
            platform::trim(platform::to_utf16(std::string_view(text).substr(start, end - start))));
        start = end + 1;
        if (line.empty() || line.front() == L';' || line.front() == L'#') {
            continue;
        }
        if (line.front() == L'[') {
            current = nullptr;
            if (platform::istarts_with(line, L"[Profile")) {
                current = &profiles.emplace_back();
            }
            continue;
        }
        const std::size_t equals = line.find(L'=');
        if (!current || equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key(platform::trim(std::wstring_view(line).substr(0, equals)));
        const std::wstring value(platform::trim(std::wstring_view(line).substr(equals + 1)));
        if (platform::iequals(key, L"Name")) {
            current->name = value;
        } else if (platform::iequals(key, L"Path")) {
            current->path = value;
        } else if (platform::iequals(key, L"IsRelative")) {
            current->relative = value != L"0";
        }
    }
    std::erase_if(profiles, [](const IniProfile& profile) { return profile.path.empty(); });
    return profiles;
}

void read_firefox_profile(Collector& collector, const BrowserProfile& profile) {
    const std::wstring path = profile.path + L"\\extensions.json";
    Json json;
    DWORD error = 0;
    const JsonRead read = read_json(path, json, error);
    if (read == JsonRead::Missing) {
        return;
    }
    const Json* addons = read == JsonRead::Ok ? member(json, "addons") : nullptr;
    if (!addons || !addons->is_array()) {
        collector.list.diagnostics.push_back(
            {ExtensionDiagnosticCode::AddonsUnreadable, path, static_cast<std::int32_t>(error)});
        return;
    }
    for (const auto& addon : *addons) {
        const std::wstring location = text_of(addon, "location");
        if (text_of(addon, "type") != L"extension" || text_of(addon, "id").empty() ||
            std::any_of(kFirefoxBuiltIn.begin(), kFirefoxBuiltIn.end(),
                        [&](std::wstring_view builtin) { return location == builtin; }) ||
            bool_of(addon, "isBuiltin") || bool_of(addon, "hidden")) {
            continue;
        }
        BrowserExtension entry;
        entry.browser = BrowserKind::Firefox;
        entry.user_sid = profile.user_sid;
        entry.user_name = profile.user_name;
        entry.profile_directory = profile.directory;
        entry.profile_name = profile.name;
        entry.profile_path = profile.path;
        entry.extension_id = text_of(addon, "id");
        if (const Json* locale = member(addon, "defaultLocale")) {
            entry.name = text_of(*locale, "name");
            entry.description = text_of(*locale, "description");
        }
        entry.version = text_of(addon, "version");
        if (const std::wstring file = text_of(addon, "path"); !file.empty()) {
            entry.folder = platform::normalize_path(file);
        }
        entry.enabled = bool_of(addon, "active", !bool_of(addon, "userDisabled"));
        entry.location = location;
        entry.update_url = text_of(addon, "updateURL");
        entry.store = integer_of(addon, "signedState") == 2 ? ExtensionStore::Mozilla : ExtensionStore::None;
        entry.from_store = entry.store != ExtensionStore::None;
        if (const auto time = integer_of(addon, "installDate"); time && *time > 0) {
            entry.install_time = *time;
        }
        const std::wstring source_uri = text_of(addon, "sourceURI");
        const Json* telemetry = member(addon, "installTelemetryInfo");
        const std::wstring origin = telemetry ? text_of(*telemetry, "source") : std::wstring();
        if (location == L"app-temporary") {
            entry.install = ExtensionInstall::Developer;
        } else if (location == L"winreg-app-global" || location == L"winreg-app-user") {
            entry.install = ExtensionInstall::ExternalRegistry;
        } else if (location == L"app-global" || location == L"app-system-share" ||
                   location == L"app-system-local" || location == L"app-system-user") {
            entry.install = ExtensionInstall::Sideload;
        } else if (location == L"app-profile") {
            if (origin == L"enterprise-policy") {
                entry.install = ExtensionInstall::Policy;
            } else if (bool_of(addon, "foreignInstall")) {
                entry.install = ExtensionInstall::Sideload;
            } else if (origin == L"amo" ||
                       platform::istarts_with(source_uri, L"https://addons.mozilla.org/")) {
                entry.install = ExtensionInstall::Store;
            } else {
                entry.install = ExtensionInstall::User;
            }
        }
        if (const Json* granted = member(addon, "userPermissions")) {
            entry.permissions = strings_of(member(*granted, "permissions"));
            entry.host_permissions = strings_of(member(*granted, "origins"));
        }
        if (const Json* optional = member(addon, "optionalPermissions")) {
            append(entry.optional_permissions, strings_of(member(*optional, "permissions")));
            append(entry.optional_permissions, strings_of(member(*optional, "origins")));
        }
        entry.page = profile.page;
        add_entry(collector, std::move(entry), {source_uri.empty() ? std::wstring() : uri_key(source_uri)});
    }
}

void collect_firefox(Collector& collector, const UserScope& user) {
    if (user.folders.roaming_app_data.empty()) {
        return;
    }
    const std::wstring base = user.folders.roaming_app_data + LR"(\Mozilla\Firefox)";
    const std::wstring ini = base + L"\\profiles.ini";
    DWORD error = 0;
    const auto text = read_text(ini, error);
    if (!text) {
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            collector.list.diagnostics.push_back(
                {ExtensionDiagnosticCode::ProfilesIniUnreadable, ini, static_cast<std::int32_t>(error)});
        }
        return;
    }
    for (const auto& ini_profile : parse_profiles_ini(*text)) {
        BrowserProfile profile;
        profile.browser = BrowserKind::Firefox;
        profile.user_sid = user.profile.sid;
        profile.user_name = user.profile.account_name;
        profile.path = platform::normalize_path(ini_profile.relative ? base + L"\\" + ini_profile.path
                                                                     : ini_profile.path);
        profile.directory = std::wstring(platform::last_path_component(profile.path));
        profile.name = ini_profile.name.empty() ? profile.directory : ini_profile.name;
        if (!folder_exists(profile.path)) {
            continue;
        }
        profile.page = make_page(collector, BrowserKind::Firefox, &user, profile.directory, profile.path);
        read_firefox_profile(collector, profile);
        collector.list.profiles.push_back(std::move(profile));
    }
}

// ---- Stubborn sources ----

// Where a source lives: HKLM in one of its views, or a user's hive.
struct Scope {
    HKEY root = nullptr;
    const UserScope* user = nullptr;
    bool wow64 = false;

    std::wstring software() const {
        return user    ? std::wstring(L"Software")
               : wow64 ? std::wstring(LR"(SOFTWARE\WOW6432Node)")
                       : L"SOFTWARE";
    }
    RegistryAddress address(std::wstring path) const {
        return user ? RegistryAddress{Hive::User, user->profile.sid, std::move(path)}
                    : RegistryAddress{Hive::Machine, L"", std::move(path)};
    }
};

std::vector<Scope> scopes(const Collector& collector) {
    std::vector<Scope> list{{collector.environment.scan.machine, nullptr, false},
                            {collector.environment.scan.machine, nullptr, true}};
    for (const auto& user : collector.environment.scan.users) {
        if (user.hive) {
            list.push_back({user.hive, &user, false});
        }
    }
    return list;
}

// Each key is read once: a key the 32- and 64-bit views share has two paths.
bool open_once(Collector& collector, const Scope& scope, const std::wstring& path,
               platform::RegistryKey& key) {
    if (!scope.root || platform::RegistryKey::open(scope.root, path, kRead, key) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring name = key.kernel_name();
    return name.empty() || collector.seen_keys.insert(platform::to_lower(name)).second;
}

ExtensionSource make_source(ExtensionSourceKind kind, const Scope& scope, std::vector<BrowserKind> browsers) {
    ExtensionSource source;
    source.kind = kind;
    source.browsers = std::move(browsers);
    if (scope.user) {
        source.user_sid = scope.user->profile.sid;
        source.user_name = scope.user->profile.account_name;
    }
    source.wow64 = scope.wow64;
    return source;
}

std::optional<std::wstring> string_of(const RawValue& value) {
    if (value.type != REG_SZ && value.type != REG_EXPAND_SZ && value.type != REG_MULTI_SZ) {
        return std::nullopt;
    }
    if (value.type != REG_MULTI_SZ) {
        return platform::RegistryValue{value.name, value.type, value.data}.as_string();
    }
    // A JSON policy written as REG_MULTI_SZ (Firefox's templates do): its lines, joined.
    std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    std::replace(text.begin(), text.end(), L'\0', L'\n');
    return text;
}

// "1", "2", ...: the names list policies use (Chromium reads them up to the first gap).
std::optional<std::uint32_t> list_index(std::wstring_view name) {
    if (name.empty() || name.size() > 9 || name.front() == L'0' ||
        !std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::wcstoul(std::wstring(name).c_str(), nullptr, 10));
}

// The numbered values of a list policy key, with their text.
template <typename Add>
void read_list(Collector& collector, const Scope& scope, const std::wstring& path, bool stops_at_gap,
               Add add) {
    platform::RegistryKey key;
    if (!open_once(collector, scope, path, key)) {
        return;
    }
    std::vector<RawValue> values;
    platform::read_raw_values(key.get(), values);
    std::erase_if(values, [](const RawValue& value) { return !list_index(value.name); });
    std::sort(values.begin(), values.end(),
              [](const RawValue& a, const RawValue& b) { return *list_index(a.name) < *list_index(b.name); });
    std::uint32_t run = 0;
    while (run < values.size() && *list_index(values[run].name) == run + 1) {
        ++run;
    }
    const std::size_t first = collector.list.sources.size();
    for (const auto& value : values) {
        const auto text = string_of(value);
        if (text && !platform::trim(*text).empty()) {
            add(value, std::wstring(platform::trim(*text)), scope.address(path));
        }
    }
    for (std::size_t i = first; i < collector.list.sources.size(); ++i) {
        auto& source = collector.list.sources[i];
        source.list = values;
        // Chromium reads "1", "2", ... up to the first missing number: values past it are inert.
        source.ignored_by_browser = stops_at_gap && *list_index(source.value_name) > run;
    }
}

bool installs(const std::wstring& mode) { return mode == L"force_installed" || mode == L"normal_installed"; }

// The ids an ExtensionSettings key names: Chromium accepts "id1,id2"; "*" is the default for all.
std::vector<std::wstring> settings_ids(const std::wstring& key, bool chromium) {
    std::vector<std::wstring> ids;
    std::size_t start = 0;
    while (start <= key.size()) {
        std::size_t end = chromium ? key.find(L',', start) : std::wstring::npos;
        if (end == std::wstring::npos) {
            end = key.size();
        }
        const std::wstring id(platform::trim(std::wstring_view(key).substr(start, end - start)));
        if (!id.empty() && id != L"*") {
            ids.push_back(chromium ? platform::to_lower(id) : id);
        }
        start = end + 1;
    }
    return ids;
}

void read_settings(Collector& collector, const Scope& scope, const std::wstring& policy_path,
                   const std::vector<BrowserKind>& browsers, bool chromium) {
    // The JSON value.
    platform::RegistryKey policy;
    if (open_once(collector, scope, policy_path, policy)) {
        RawValue raw;
        if (platform::query_raw_value(policy.get(), L"ExtensionSettings", raw) == ERROR_SUCCESS) {
            const auto text = string_of(raw);
            const Json json = text ? Json::parse(platform::to_utf8(*text), nullptr, false, true) : Json();
            if (!json.is_object()) {
                collector.list.diagnostics.push_back(
                    {ExtensionDiagnosticCode::PolicyUnreadable,
                     platform::display_path(scope.address(policy_path)) + L"\\ExtensionSettings", 0});
            } else {
                for (const auto& [name, settings] : json.items()) {
                    const std::wstring mode = text_of(settings, "installation_mode");
                    if (!installs(mode)) {
                        continue;
                    }
                    const std::wstring key = platform::to_utf16(name);
                    for (const auto& id : settings_ids(key, chromium)) {
                        ExtensionSource source = make_source(ExtensionSourceKind::Settings, scope, browsers);
                        source.key = scope.address(policy_path);
                        source.value_name = L"ExtensionSettings";
                        source.value = raw;
                        source.extension_id = id;
                        source.settings_key = key;
                        source.installation_mode = mode;
                        source.update_url = text_of(settings, chromium ? "update_url" : "install_url");
                        source.data = source.update_url;
                        collector.list.sources.push_back(std::move(source));
                    }
                }
            }
        }
    }
    // The subkey form: ExtensionSettings\<id> with installation_mode and the URL as values.
    platform::RegistryKey settings;
    const std::wstring settings_path = policy_path + L"\\ExtensionSettings";
    if (!open_once(collector, scope, settings_path, settings)) {
        return;
    }
    for (const auto& name : settings.subkey_names()) {
        platform::RegistryKey subkey;
        if (platform::RegistryKey::open(settings.get(), name, kRead, subkey) != ERROR_SUCCESS) {
            continue;
        }
        const auto values = subkey.values();
        const auto* mode_value = platform::find_value(values, L"installation_mode");
        const auto mode = mode_value ? mode_value->as_string() : std::nullopt;
        if (!mode || !installs(*mode)) {
            continue;
        }
        const auto* url_value = platform::find_value(values, chromium ? L"update_url" : L"install_url");
        const auto url = url_value ? url_value->as_string() : std::nullopt;
        for (const auto& id : settings_ids(name, chromium)) {
            ExtensionSource source = make_source(ExtensionSourceKind::Settings, scope, browsers);
            source.key = scope.address(settings_path + L"\\" + name);
            source.extension_id = id;
            source.settings_key = name;
            source.settings_subkey = true;
            source.installation_mode = *mode;
            source.update_url = url.value_or(L"");
            source.data = source.update_url;
            collector.list.sources.push_back(std::move(source));
        }
    }
}

void collect_chromium_policies(Collector& collector, const Scope& scope) {
    for (const auto& vendor : policy_vendors()) {
        const std::wstring policy = scope.software() + LR"(\Policies\)" + std::wstring(vendor.path);
        read_list(collector, scope, policy + L"\\ExtensionInstallForcelist", true,
                  [&](const RawValue& value, std::wstring text, RegistryAddress key) {
                      ExtensionSource source =
                          make_source(ExtensionSourceKind::ForceList, scope, vendor.browsers);
                      const std::size_t semicolon = text.find(L';');
                      source.extension_id = platform::to_lower(platform::trim(text.substr(0, semicolon)));
                      if (semicolon != std::wstring::npos) {
                          source.update_url = std::wstring(platform::trim(text.substr(semicolon + 1)));
                      }
                      source.key = std::move(key);
                      source.value_name = value.name;
                      source.value = value;
                      source.data = std::move(text);
                      collector.list.sources.push_back(std::move(source));
                  });
        read_settings(collector, scope, policy, vendor.browsers, true);
    }
}

void collect_chromium_registrations(Collector& collector, const Scope& scope) {
    for (const auto& vendor : registration_vendors()) {
        const std::wstring path = scope.software() + L"\\" + std::wstring(vendor.path);
        platform::RegistryKey key;
        if (!open_once(collector, scope, path, key)) {
            continue;
        }
        for (const auto& name : key.subkey_names()) {
            platform::RegistryKey subkey;
            if (platform::RegistryKey::open(key.get(), name, kRead, subkey) != ERROR_SUCCESS) {
                continue;
            }
            const auto values = subkey.values();
            const auto text = [&](std::wstring_view value_name) {
                const auto* value = platform::find_value(values, value_name);
                return value ? value->as_string().value_or(L"") : std::wstring();
            };
            ExtensionSource source =
                make_source(ExtensionSourceKind::ExternalRegistry, scope, vendor.browsers);
            source.key = scope.address(path + L"\\" + name);
            // Chromium reads HKLM only in its 32-bit view (KEY_WOW64_32KEY), and HKCU.
            source.ignored_by_browser = !scope.user && !scope.wow64;
            source.extension_id = platform::to_lower(name);
            source.update_url = text(L"update_url");
            if (source.update_url.empty()) {
                source.path = text(L"path");
                source.version = text(L"version");
            }
            source.data = source.update_url.empty() ? source.path : source.update_url;
            collector.list.sources.push_back(std::move(source));
        }
    }
}

void collect_firefox_registry(Collector& collector, const Scope& scope) {
    const std::vector<BrowserKind> firefox = {BrowserKind::Firefox};
    // Add-ons registered by programs: values named after the add-on, holding its folder or .xpi.
    const std::wstring path = scope.software() + LR"(\Mozilla\Firefox\Extensions)";
    platform::RegistryKey key;
    if (open_once(collector, scope, path, key)) {
        std::vector<RawValue> values;
        platform::read_raw_values(key.get(), values);
        for (const auto& value : values) {
            const auto text = string_of(value);
            if (value.name.empty() || !text) {
                continue;
            }
            ExtensionSource source = make_source(ExtensionSourceKind::FirefoxRegistry, scope, firefox);
            source.key = scope.address(path);
            source.value_name = value.name;
            source.value = value;
            source.extension_id = value.name;
            source.path = std::wstring(platform::trim(*text));
            source.data = source.path;
            collector.list.sources.push_back(std::move(source));
        }
    }
    // Policies.
    const std::wstring policy = scope.software() + LR"(\Policies\Mozilla\Firefox)";
    read_settings(collector, scope, policy, firefox, false);
    for (const auto locked : {false, true}) {
        // Whether Firefox stops at a gap too isn't known: its values aren't flagged.
        read_list(collector, scope, policy + (locked ? LR"(\Extensions\Locked)" : LR"(\Extensions\Install)"),
                  false, [&](const RawValue& value, std::wstring text, RegistryAddress list_key) {
                      ExtensionSource source = make_source(
                          locked ? ExtensionSourceKind::FirefoxLocked : ExtensionSourceKind::FirefoxInstall,
                          scope, firefox);
                      source.key = std::move(list_key);
                      source.value_name = value.name;
                      source.value = value;
                      if (locked) {
                          source.extension_id = text;
                      } else if (platform::is_absolute_path(text)) {
                          source.path = text;
                      }
                      source.data = std::move(text);
                      collector.list.sources.push_back(std::move(source));
                  });
    }
}

std::vector<std::wstring> firefox_folders(const Collector& collector) {
    if (collector.environment.firefox_folders) {
        return *collector.environment.firefox_folders;
    }
    std::vector<std::wstring> folders;
    const auto add = [&](const std::wstring& folder) {
        if (!folder.empty() && folder_exists(folder) &&
            std::none_of(folders.begin(), folders.end(),
                         [&](const std::wstring& known) { return platform::paths_equal(known, folder); })) {
            folders.push_back(platform::normalize_path(folder));
        }
    };
    const auto& machine = collector.environment.scan.machine_folders;
    for (const auto& base : {machine.program_files, machine.program_files_x86}) {
        for (const auto name : kFirefoxPrograms) {
            if (!base.empty()) {
                add(base + L"\\" + std::wstring(name));
            }
        }
    }
    add(platform::parent_path(app_path(collector.environment.scan.machine, L"SOFTWARE", L"firefox.exe")));
    return folders;
}

void collect_policies_json(Collector& collector) {
    for (const auto& folder : firefox_folders(collector)) {
        const std::wstring file = folder + LR"(\distribution\policies.json)";
        Json json;
        DWORD error = 0;
        const JsonRead read = read_json(file, json, error);
        if (read == JsonRead::Missing) {
            continue;
        }
        const Json* policies = read == JsonRead::Ok ? member(json, "policies") : nullptr;
        if (!policies || !policies->is_object()) {
            collector.list.diagnostics.push_back(
                {ExtensionDiagnosticCode::PoliciesJsonUnreadable, file, static_cast<std::int32_t>(error)});
            continue;
        }
        const auto add = [&](std::wstring id, std::wstring data, std::wstring mode) {
            ExtensionSource source;
            source.kind = ExtensionSourceKind::PoliciesJson;
            source.browsers = {BrowserKind::Firefox};
            source.file = file;
            source.extension_id = std::move(id);
            if (source.extension_id.empty() && platform::is_absolute_path(data)) {
                source.path = data;
            }
            source.data = std::move(data);
            source.installation_mode = std::move(mode);
            collector.list.sources.push_back(std::move(source));
        };
        if (const Json* settings = member(*policies, "ExtensionSettings");
            settings && settings->is_object()) {
            for (const auto& [name, value] : settings->items()) {
                const std::wstring mode = text_of(value, "installation_mode");
                if (installs(mode) && name != "*") {
                    add(platform::to_utf16(name), text_of(value, "install_url"), mode);
                }
            }
        }
        if (const Json* extensions = member(*policies, "Extensions")) {
            for (auto& url : strings_of(member(*extensions, "Install"))) {
                add(L"", std::move(url), L"");
            }
            for (auto& id : strings_of(member(*extensions, "Locked"))) {
                add(id, id, L"");
            }
        }
    }
}

// ---- Matching ----

bool scope_fits(const ExtensionSource& source, const BrowserExtension& entry) {
    return source.user_sid.empty() || platform::iequals(source.user_sid, entry.user_sid);
}

bool is_registration(ExtensionSourceKind kind) {
    return kind == ExtensionSourceKind::ExternalRegistry || kind == ExtensionSourceKind::FirefoxRegistry;
}

// A Firefox Install entry names a URL or a path: the add-on installed from it, by its sourceURI.
void map_install_urls(Collector& collector) {
    for (auto& source : collector.list.sources) {
        if (!source.extension_id.empty() || source.data.empty() ||
            (source.kind != ExtensionSourceKind::FirefoxInstall &&
             source.kind != ExtensionSourceKind::PoliciesJson)) {
            continue;
        }
        const std::wstring key = uri_key(source.data);
        for (std::size_t i = 0; i < collector.list.extensions.size(); ++i) {
            const auto& entry = collector.list.extensions[i];
            if (entry.browser == BrowserKind::Firefox && scope_fits(source, entry) &&
                collector.pending[i].source_uri == key) {
                source.extension_id = entry.extension_id;
                break;
            }
        }
    }
}

void match_sources(Collector& collector) {
    auto& list = collector.list;
    std::vector<bool> used(list.sources.size(), false);
    for (auto& entry : list.extensions) {
        for (std::size_t s = 0; s < list.sources.size(); ++s) {
            const auto& source = list.sources[s];
            if (contains_browser(source.browsers, entry.browser) && scope_fits(source, entry) &&
                !source.extension_id.empty() && platform::iequals(source.extension_id, entry.extension_id)) {
                entry.sources.push_back(s);
                used[s] = true;
            }
        }
    }
    // An extension its profile says came from the registry, with no registration of its own browser: the
    // registration of another vendor's key it may read (a fork that kept Google\Chrome's, say).
    for (auto& entry : list.extensions) {
        if (entry.install != ExtensionInstall::ExternalRegistry ||
            std::any_of(entry.sources.begin(), entry.sources.end(),
                        [&](std::size_t s) { return is_registration(list.sources[s].kind); })) {
            continue;
        }
        for (std::size_t s = 0; s < list.sources.size(); ++s) {
            const auto& source = list.sources[s];
            if (source.kind == ExtensionSourceKind::ExternalRegistry && scope_fits(source, entry) &&
                platform::iequals(source.extension_id, entry.extension_id)) {
                entry.sources.push_back(s);
                used[s] = true;
            }
        }
    }
    // Sources whose extension is in no profile: one entry per browser, user and extension.
    std::map<std::wstring, std::size_t> registrations;  // By identity, the entry's index
    for (std::size_t s = 0; s < list.sources.size(); ++s) {
        if (used[s]) {
            continue;
        }
        const auto& source = list.sources[s];
        const BrowserKind browser = source.browsers.empty() ? BrowserKind::Chrome : source.browsers.front();
        const std::wstring subject = source.extension_id.empty() ? source.data : source.extension_id;
        const std::wstring identity = std::format(
            L"registration|{}|{}|{}", platform::to_utf16(to_string(browser)), source.user_sid, subject);
        auto [found, inserted] =
            registrations.try_emplace(platform::to_lower(identity), list.extensions.size());
        if (inserted) {
            BrowserExtension entry;
            entry.id = hash_id(identity);
            entry.browser = browser;
            entry.user_sid = source.user_sid;
            entry.user_name = source.user_name;
            entry.in_profile = false;
            entry.extension_id = source.extension_id;
            if (source.extension_id.empty()) {
                entry.name = source.data;  // A Firefox Install URL no installed add-on came from
            }
            entry.update_url = source.update_url;
            entry.store = store_of_update_url(source.update_url);
            entry.from_store = entry.store != ExtensionStore::None;
            entry.install =
                is_registration(source.kind) ? ExtensionInstall::ExternalRegistry : ExtensionInstall::Policy;
            const UserScope* user =
                user_with_sid(collector, source.user_sid.empty() ? collector.environment.scan.current_user_sid
                                                                 : source.user_sid);
            entry.page = make_page(collector, browser, user, L"", L"");
            add_entry(collector, std::move(entry));
        }
        list.extensions[found->second].sources.push_back(s);
    }
}

void finish(Collector& collector) {
    auto& list = collector.list;
    std::unique_ptr<force::OwnerIndex> owners;
    for (auto& source : list.sources) {
        if (is_registration(source.kind) && platform::is_absolute_path(source.path)) {
            if (!owners) {
                owners = std::make_unique<force::OwnerIndex>(collector.environment.scan,
                                                             collector.environment.programs);
            }
            if (const auto* program = owners->program_of(platform::normalize_path(source.path))) {
                source.program_id = program->id;
            }
        }
    }
    for (auto& entry : list.extensions) {
        if (entry.in_profile) {
            entry.id =
                browser_extension_id(entry.browser, entry.user_sid, entry.profile_path, entry.extension_id);
        }
        const auto has = [&](auto predicate) {
            return std::any_of(entry.sources.begin(), entry.sources.end(),
                               [&](std::size_t s) { return predicate(list.sources[s]); });
        };
        if (entry.sources.empty()) {
            entry.removal = ExtensionRemoval::NotRemovable;
        } else if (has([](const ExtensionSource& s) {
                       return s.kind == ExtensionSourceKind::PoliciesJson;
                   })) {
            entry.removal = ExtensionRemoval::ManagedByPoliciesJson;
        } else if (has([](const ExtensionSource& s) {
                       return s.settings_subkey && s.settings_key.find(L',') != std::wstring::npos;
                   })) {
            entry.removal = ExtensionRemoval::NotSupported;
        } else {
            entry.removal = ExtensionRemoval::Removable;
        }
        // The profile says the user installed it (from the store, a file or unpacked), not a source:
        // taking the sources out leaves that copy.
        entry.keeps_user_copy =
            entry.in_profile && !entry.sources.empty() &&
            (entry.install == ExtensionInstall::Store || entry.install == ExtensionInstall::User ||
             entry.install == ExtensionInstall::Developer);
    }
    const auto key = [](const BrowserExtension& entry) {
        return std::make_tuple(entry.browser, platform::to_lower(entry.user_name), !entry.in_profile,
                               platform::to_lower(entry.profile_name), platform::to_lower(entry.profile_path),
                               platform::to_lower(entry.name), entry.extension_id);
    };
    std::stable_sort(list.extensions.begin(), list.extensions.end(),
                     [&](const BrowserExtension& a, const BrowserExtension& b) { return key(a) < key(b); });
}

// Joined to a domain, or enrolled in MDM: Group Policy or the MDM server may write the policies again.
bool machine_managed(HKEY machine) {
    LPWSTR name = nullptr;
    NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
    if (::NetGetJoinInformation(nullptr, &name, &status) == NERR_Success) {
        ::NetApiBufferFree(name);
        if (status == NetSetupDomainName) {
            return true;
        }
    }
    platform::RegistryKey enrollments;
    if (platform::RegistryKey::open(machine, LR"(SOFTWARE\Microsoft\Enrollments)", kRead, enrollments) !=
        ERROR_SUCCESS) {
        return false;
    }
    for (const auto& subkey_name : enrollments.subkey_names()) {
        platform::RegistryKey enrollment;
        if (platform::RegistryKey::open(enrollments.get(), subkey_name, kRead, enrollment) != ERROR_SUCCESS) {
            continue;
        }
        const auto values = enrollment.values();
        // An enrolled MDM server: EnrollmentType 6 (full MDM) or 13 (MDM with an Entra ID join), or
        // Intune's provider. Windows' own authorities (Local, Deploy, Cloud Authority: types 28, 30, 31)
        // have enrollments too, and don't count.
        const auto* provider = platform::find_value(values, L"ProviderID");
        const auto* state = platform::find_value(values, L"EnrollmentState");
        const auto* type = platform::find_value(values, L"EnrollmentType");
        const auto provider_text = provider ? provider->as_string() : std::nullopt;
        const auto type_number = type ? type->as_integer() : std::nullopt;
        if (state && state->as_integer() == 1u &&
            (type_number == 6u || type_number == 13u ||
             (provider_text && platform::iequals(*provider_text, L"MS DM Server")))) {
            return true;
        }
    }
    return false;
}

// "SOFTWARE\Policies\<rest>" of a key, in either view of HKLM or in a user's hive; empty otherwise.
std::wstring_view below_policies(const RegistryAddress& key) {
    std::wstring_view path = key.path;
    if (key.hive == Hive::User && (key.sid.empty() || platform::iends_with(key.sid, L"_Classes"))) {
        return {};
    }
    for (const std::wstring_view prefix : {std::wstring_view(LR"(SOFTWARE\WOW6432Node\Policies\)"),
                                           std::wstring_view(LR"(SOFTWARE\Policies\)")}) {
        if (key.hive == Hive::User && prefix.find(L"WOW6432Node") != std::wstring_view::npos) {
            continue;
        }
        if (path.size() > prefix.size() && platform::istarts_with(path, prefix)) {
            return path.substr(prefix.size());
        }
    }
    return {};
}

}  // namespace

bool is_chromium_family(BrowserKind browser) { return browser != BrowserKind::Firefox; }

BrowserExtensionsList list_browser_extensions(const BrowserExtensionsEnvironment& environment) {
    Collector collector(environment);
    for (const auto& user : environment.scan.users) {
        collect_chromium(collector, user);
        collect_firefox(collector, user);
    }
    for (const auto& scope : scopes(collector)) {
        collect_chromium_policies(collector, scope);
        collect_chromium_registrations(collector, scope);
        collect_firefox_registry(collector, scope);
    }
    collect_policies_json(collector);
    map_install_urls(collector);
    match_sources(collector);
    finish(collector);
    collector.list.policies_managed =
        environment.managed ? *environment.managed : machine_managed(environment.scan.machine);
    return std::move(collector.list);
}

std::wstring browser_extension_id(BrowserKind browser, std::wstring_view user_sid,
                                  std::wstring_view profile_path, std::wstring_view extension_id) {
    return hash_id(std::format(L"{}|{}|{}|{}", platform::to_utf16(to_string(browser)), user_sid,
                               platform::normalize_path(profile_path), extension_id));
}

bool is_extension_policy_value(const platform::RegistryAddress& key, std::wstring_view value_name) {
    const std::wstring_view rest = below_policies(key);
    if (rest.empty()) {
        return false;
    }
    std::vector<std::wstring_view> vendors;
    for (const auto& vendor : policy_vendors()) {
        vendors.push_back(vendor.path);
    }
    vendors.push_back(LR"(Mozilla\Firefox)");
    for (const auto vendor : vendors) {
        if (platform::iequals(rest, vendor)) {
            return platform::iequals(value_name, L"ExtensionSettings");
        }
        const bool firefox = vendor == LR"(Mozilla\Firefox)";
        for (const std::wstring_view list :
             firefox ? std::vector<std::wstring_view>{LR"(\Extensions\Install)", LR"(\Extensions\Locked)"}
                     : std::vector<std::wstring_view>{LR"(\ExtensionInstallForcelist)"}) {
            if (rest.size() == vendor.size() + list.size() && platform::istarts_with(rest, vendor) &&
                platform::iequals(rest.substr(vendor.size()), list)) {
                return list_index(value_name).has_value();
            }
        }
    }
    return false;
}

ExtensionStore store_of_update_url(std::wstring_view update_url) {
    std::wstring url = platform::to_lower(platform::trim(update_url));
    if (const std::size_t query = url.find(L'?'); query != std::wstring::npos) {
        url.resize(query);
    }
    while (!url.empty() && url.back() == L'/') {
        url.pop_back();
    }
    if (url == L"https://clients2.google.com/service/update2/crx" ||
        url == L"http://clients2.google.com/service/update2/crx") {
        return ExtensionStore::ChromeWebStore;
    }
    if (url == L"https://edge.microsoft.com/extensionwebstorebase/v1/crx") {
        return ExtensionStore::EdgeAddons;
    }
    if (url == L"https://extension-updates.opera.com/api/omaha/update") {
        return ExtensionStore::OperaAddons;
    }
    return ExtensionStore::None;
}

std::string_view to_string(BrowserKind browser) {
    switch (browser) {
        case BrowserKind::Chrome:
            return "chrome";
        case BrowserKind::ChromeBeta:
            return "chrome-beta";
        case BrowserKind::ChromeDev:
            return "chrome-dev";
        case BrowserKind::ChromeCanary:
            return "chrome-canary";
        case BrowserKind::Edge:
            return "edge";
        case BrowserKind::EdgeBeta:
            return "edge-beta";
        case BrowserKind::EdgeDev:
            return "edge-dev";
        case BrowserKind::EdgeCanary:
            return "edge-canary";
        case BrowserKind::Brave:
            return "brave";
        case BrowserKind::Vivaldi:
            return "vivaldi";
        case BrowserKind::Opera:
            return "opera";
        case BrowserKind::OperaGx:
            return "opera-gx";
        case BrowserKind::Chromium:
            return "chromium";
        case BrowserKind::Firefox:
            return "firefox";
    }
    return "unknown";
}

bool parse(std::string_view text, BrowserKind& browser) {
    for (int i = 0; i <= static_cast<int>(BrowserKind::Firefox); ++i) {
        if (to_string(static_cast<BrowserKind>(i)) == text) {
            browser = static_cast<BrowserKind>(i);
            return true;
        }
    }
    return false;
}

std::string_view to_string(ExtensionInstall install) {
    switch (install) {
        case ExtensionInstall::Store:
            return "store";
        case ExtensionInstall::User:
            return "user";
        case ExtensionInstall::Developer:
            return "developer";
        case ExtensionInstall::ExternalPref:
            return "external-pref";
        case ExtensionInstall::ExternalRegistry:
            return "external-registry";
        case ExtensionInstall::Policy:
            return "policy";
        case ExtensionInstall::Sideload:
            return "sideload";
        case ExtensionInstall::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::string_view to_string(ExtensionStore store) {
    switch (store) {
        case ExtensionStore::None:
            return "none";
        case ExtensionStore::ChromeWebStore:
            return "chrome-web-store";
        case ExtensionStore::EdgeAddons:
            return "edge-add-ons";
        case ExtensionStore::OperaAddons:
            return "opera-add-ons";
        case ExtensionStore::Mozilla:
            return "mozilla";
    }
    return "unknown";
}

std::string_view to_string(ExtensionSourceKind kind) {
    switch (kind) {
        case ExtensionSourceKind::ForceList:
            return "force-list";
        case ExtensionSourceKind::Settings:
            return "settings";
        case ExtensionSourceKind::ExternalRegistry:
            return "external-registry";
        case ExtensionSourceKind::FirefoxRegistry:
            return "firefox-registry";
        case ExtensionSourceKind::FirefoxInstall:
            return "firefox-install";
        case ExtensionSourceKind::FirefoxLocked:
            return "firefox-locked";
        case ExtensionSourceKind::PoliciesJson:
            return "policies-json";
    }
    return "unknown";
}

std::string_view to_string(ExtensionRemoval removal) {
    switch (removal) {
        case ExtensionRemoval::Removable:
            return "removable";
        case ExtensionRemoval::NotRemovable:
            return "not-removable";
        case ExtensionRemoval::ManagedByPoliciesJson:
            return "managed-by-policies-json";
        case ExtensionRemoval::NotSupported:
            return "not-supported";
    }
    return "unknown";
}

std::string_view to_string(ExtensionDiagnosticCode code) {
    switch (code) {
        case ExtensionDiagnosticCode::ProfileUnreadable:
            return "profile-unreadable";
        case ExtensionDiagnosticCode::LocalStateUnreadable:
            return "local-state-unreadable";
        case ExtensionDiagnosticCode::AddonsUnreadable:
            return "addons-unreadable";
        case ExtensionDiagnosticCode::ProfilesIniUnreadable:
            return "profiles-ini-unreadable";
        case ExtensionDiagnosticCode::PolicyUnreadable:
            return "policy-unreadable";
        case ExtensionDiagnosticCode::PoliciesJsonUnreadable:
            return "policies-json-unreadable";
    }
    return "unknown";
}

}  // namespace avel::tools
