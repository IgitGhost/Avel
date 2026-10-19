#include "core/inventory/uninstall_entry.h"

#include <algorithm>
#include <cwctype>
#include <vector>

#include "core/platform/command_line.h"
#include "core/platform/text.h"

namespace avel::inventory {

namespace {

using platform::RegistryValue;

class ValueReader {
public:
    ValueReader(const std::vector<RegistryValue>& values, const UninstallKeyContext& context)
        : values_(values), context_(context) {}

    // String value with surrounding whitespace removed and REG_EXPAND_SZ expanded.
    std::wstring string(std::wstring_view name) const { return read(platform::find_value(values_, name)); }

    // Like string(), but falls back to values whose name starts with `name`, such as the
    // "UninstallString_hidden" some installers use to hide themselves from Settings.
    std::wstring fuzzy_string(std::wstring_view name) const {
        if (auto exact = string(name); !exact.empty()) {
            return exact;
        }
        for (const auto& value : values_) {
            if (platform::istarts_with(value.name, name)) {
                if (auto text = read(&value); !text.empty()) {
                    return text;
                }
            }
        }
        return {};
    }

    std::uint64_t integer(std::wstring_view name) const {
        const auto* value = platform::find_value(values_, name);
        return value ? value->as_integer().value_or(0) : 0;
    }

    bool flag(std::wstring_view name) const { return integer(name) != 0; }

    bool any_name_contains(std::wstring_view part) const {
        for (const auto& value : values_) {
            if (platform::icontains(value.name, part)) {
                return true;
            }
        }
        return false;
    }

private:
    std::wstring read(const RegistryValue* value) const {
        if (value == nullptr) {
            return {};
        }
        const auto text = value->as_string();
        if (!text) {
            return {};
        }
        std::wstring result(platform::trim(*text));
        if (value->type == REG_EXPAND_SZ && context_.expand) {
            result = context_.expand(result);
        }
        return result;
    }

    const std::vector<RegistryValue>& values_;
    const UninstallKeyContext& context_;
};

// BCU ApplicationEntryTools.CleanupDisplayVersion: "1, 2, 3" -> "1.2.3".
std::wstring cleanup_version(std::wstring version) {
    for (const std::wstring_view from : {L", ", L". ", L","}) {
        for (size_t at = version.find(from); at != std::wstring::npos; at = version.find(from, at)) {
            version.replace(at, from.size(), L".");
            ++at;
        }
    }
    return std::wstring(platform::trim(version));
}

// Install locations are compared and displayed a lot; store them without quotes or trailing
// separators (but keep "C:\").
std::wstring normalize_location(std::wstring_view path) {
    path = platform::trim(path);
    while (!path.empty() && path.front() == L'"') {
        path.remove_prefix(1);
    }
    while (!path.empty() && path.back() == L'"') {
        path.remove_suffix(1);
    }
    path = platform::trim(path);
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.remove_suffix(1);
    }
    return std::wstring(path);
}

bool is_digits(std::wstring_view text) {
    if (text.empty()) {
        return false;
    }
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return false;
        }
    }
    return true;
}

int to_int(std::wstring_view digits) {
    int value = 0;
    for (const wchar_t c : digits) {
        value = value * 10 + (c - L'0');
    }
    return value;
}

std::optional<std::chrono::sys_days> make_date(int year, int month, int day) {
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || year < 1980 || year > 2200) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date};
}

// C's asctime() format, "Sat Oct 17 22:38:01 2026", which some installers (Qt's among them) write.
std::optional<std::chrono::sys_days> parse_ctime_date(std::wstring_view text) {
    std::vector<std::wstring_view> fields;
    while (!text.empty()) {
        const size_t start = text.find_first_not_of(L' ');
        if (start == std::wstring_view::npos) {
            break;
        }
        text.remove_prefix(start);
        const size_t end = std::min(text.find(L' '), text.size());
        fields.push_back(text.substr(0, end));
        text.remove_prefix(end);
    }
    if (fields.size() != 5 || fields[0].size() != 3 || fields[3].size() != 8 || !is_digits(fields[2]) ||
        fields[2].size() > 2 || fields[4].size() != 4 || !is_digits(fields[4])) {
        return std::nullopt;
    }
    constexpr std::wstring_view kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                             L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    for (int month = 0; month < 12; ++month) {
        if (platform::iequals(fields[1], kMonths[month])) {
            return make_date(to_int(fields[4]), month + 1, to_int(fields[2]));
        }
    }
    return std::nullopt;
}

// "KB1234567"-style names mark Windows and Office updates.
bool looks_like_kb_number(std::wstring_view text) {
    return text.size() > 6 && text.starts_with(L"KB") && std::iswdigit(text[2]) && std::iswdigit(text.back());
}

std::wstring first_url(const ValueReader& reader) {
    for (const std::wstring_view name : {L"URLInfoAbout", L"URLUpdateInfo", L"HelpLink"}) {
        if (auto url = reader.string(name); url.find(L'.') != std::wstring::npos) {
            return url;
        }
    }
    return {};
}

// Settings prefers DisplayName_Localized (a resource reference such as "@C:\...\Resource.dll,-20000")
// and falls back to DisplayName, which may itself be a resource reference.
std::wstring display_name(const ValueReader& reader, const UninstallKeyContext& context) {
    auto resolve = [&](const std::wstring& name) -> std::wstring {
        if (!name.starts_with(L'@')) {
            return name;
        }
        return context.resolve_indirect ? context.resolve_indirect(name) : std::wstring();
    };
    if (auto localized = resolve(reader.string(L"DisplayName_Localized")); !localized.empty()) {
        return localized;
    }
    const std::wstring name = reader.string(L"DisplayName");
    const std::wstring resolved = resolve(name);
    return resolved.empty() ? name : resolved;
}

bool is_inno_uninstaller_name(std::wstring_view executable) {
    // unins000.exe, unins001.exe, ...
    const auto slash = executable.find_last_of(L"\\/");
    const std::wstring_view file =
        slash == std::wstring_view::npos ? executable : executable.substr(slash + 1);
    return file.size() == 12 && platform::istarts_with(file, L"unins") && is_digits(file.substr(5, 3)) &&
           platform::iends_with(file, L".exe");
}

}  // namespace

std::optional<std::chrono::sys_days> parse_install_date(std::wstring_view text) {
    text = platform::trim(text);
    if (text.size() == 8 && is_digits(text)) {
        const int year = to_int(text.substr(0, 4));
        const int a = to_int(text.substr(4, 2));
        const int b = to_int(text.substr(6, 2));
        if (auto date = make_date(year, a, b)) {
            return date;
        }
        return make_date(year, b, a);  // YYYYDDMM
    }
    if (text.size() == 10 && text[4] == L'-' && text[7] == L'-' && is_digits(text.substr(0, 4)) &&
        is_digits(text.substr(5, 2)) && is_digits(text.substr(8, 2))) {
        return make_date(to_int(text.substr(0, 4)), to_int(text.substr(5, 2)), to_int(text.substr(8, 2)));
    }
    return parse_ctime_date(text);
}

std::optional<std::wstring> extract_guid(std::wstring_view text) {
    constexpr size_t kLength = 38;  // {8-4-4-4-12}
    for (size_t start = text.find(L'{'); start != std::wstring_view::npos;
         start = text.find(L'{', start + 1)) {
        if (start + kLength > text.size()) {
            break;
        }
        const std::wstring_view candidate = text.substr(start, kLength);
        bool valid = candidate.back() == L'}';
        for (size_t i = 1; valid && i < kLength - 1; ++i) {
            const bool dash_position = i == 9 || i == 14 || i == 19 || i == 24;
            valid = dash_position ? candidate[i] == L'-' : std::iswxdigit(candidate[i]) != 0;
        }
        if (valid) {
            std::wstring guid(candidate);
            for (auto& c : guid) {
                c = static_cast<wchar_t>(std::towupper(c));
            }
            return guid;
        }
    }
    return std::nullopt;
}

InstallerType installer_type_from_command(std::wstring_view command) {
    using platform::icontains;
    if (command.empty()) {
        return InstallerType::Unknown;
    }
    // WiX Burn bundles cache themselves: "C:\ProgramData\Package Cache\{GUID}\setup.exe" /uninstall
    if (icontains(command, LR"(\Package Cache\{)") && icontains(command, L"}\\") &&
        icontains(command, L".exe")) {
        return InstallerType::WixBurn;
    }
    if (icontains(command, L"msiexec ") || icontains(command, L"msiexec.exe") ||
        platform::iends_with(platform::trim(command), L".msi")) {
        return InstallerType::Msi;
    }
    if (icontains(command, L"sdbinst") && icontains(command, L".sdb")) {
        return InstallerType::SdbInst;
    }
    if (icontains(command, LR"(InstallShield Installation Information\{)")) {
        return InstallerType::InstallShield;
    }
    if (icontains(command, L"powershell.exe") || icontains(command, L".ps1")) {
        return InstallerType::PowerShell;
    }
    // Squirrel apps (Discord, Teams classic, ...): "%LocalAppData%\App\Update.exe" --uninstall
    if (icontains(command, L"\\Update.exe") && icontains(command, L"--uninstall")) {
        return InstallerType::Squirrel;
    }
    // "_?=<folder>" (run in place) is an NSIS uninstaller parameter.
    if (icontains(command, L" _?=")) {
        return InstallerType::Nsis;
    }
    const auto parsed = platform::split_command_line(command, [](const std::wstring&) { return false; });
    if (is_inno_uninstaller_name(parsed.executable)) {
        return InstallerType::InnoSetup;
    }
    return InstallerType::Unknown;
}

std::optional<Program> parse_uninstall_entry(const UninstallKeyContext& context,
                                             const std::vector<RegistryValue>& values) {
    const ValueReader reader(values, context);

    Program program;
    program.display_name = display_name(reader, context);
    program.publisher = reader.string(L"Publisher");
    program.quiet_uninstall_string = reader.fuzzy_string(L"QuietUninstallString");
    program.uninstall_string = reader.fuzzy_string(L"UninstallString");
    if (program.uninstall_string.empty()) {
        program.uninstall_string = program.quiet_uninstall_string;
    }
    if (program.display_name.empty() && program.publisher.empty() && program.uninstall_string.empty()) {
        return std::nullopt;
    }

    program.id = context.id_prefix + context.location.key_name;
    program.source = Source::Registry;
    program.scope = context.scope;
    program.user_sid = context.user_sid;
    program.user_name = context.user_name;
    program.architecture = context.architecture;
    program.registry = context.location;

    program.display_version = cleanup_version(reader.string(L"DisplayVersion"));
    program.install_location = normalize_location(reader.string(L"InstallLocation"));
    program.install_source = normalize_location(reader.string(L"InstallSource"));
    program.install_date = parse_install_date(reader.string(L"InstallDate"));
    program.modify_path = reader.string(L"ModifyPath");
    program.display_icon = reader.string(L"DisplayIcon");
    program.comments = reader.string(L"Comments");
    program.about_url = first_url(reader);

    // EstimatedSize is in KiB; some installers write it as a string.
    program.estimated_size_bytes = reader.integer(L"EstimatedSize") * 1024;

    program.system_component = reader.flag(L"SystemComponent");
    program.no_remove = reader.flag(L"NoRemove");
    program.no_modify = reader.flag(L"NoModify");
    program.no_repair = reader.flag(L"NoRepair");

    program.parent_key_name = reader.string(L"ParentKeyName");
    const std::wstring release_type = reader.string(L"ReleaseType");
    program.is_update = !program.parent_key_name.empty() || platform::icontains(release_type, L"Update") ||
                        platform::icontains(release_type, L"Hotfix") ||
                        looks_like_kb_number(context.location.key_name) ||
                        looks_like_kb_number(reader.string(L""));

    program.windows_installer = reader.flag(L"WindowsInstaller");
    if (program.windows_installer) {
        program.installer_type = InstallerType::Msi;
    } else if (reader.any_name_contains(L"Inno Setup:")) {
        program.installer_type = InstallerType::InnoSetup;
    } else if (context.location.key_name.starts_with(L"Steam App ")) {
        program.installer_type = InstallerType::Steam;
    } else if (!reader.string(L"BundleProviderKey").empty() || !reader.string(L"BundleCachePath").empty()) {
        program.installer_type = InstallerType::WixBurn;
    } else {
        program.installer_type = installer_type_from_command(program.uninstall_string);
    }

    if (program.installer_type == InstallerType::Msi) {
        // MSI products use the product code as the key name; "MsiExec.exe /X{GUID}" also carries it.
        auto code = extract_guid(context.location.key_name);
        if (!code) {
            code = extract_guid(program.uninstall_string);
        }
        program.msi_product_code = code.value_or(L"");
    }

    return program;
}

}  // namespace avel::inventory
