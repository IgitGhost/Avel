#include "cli/list_command.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string>

#include "cli/console.h"
#include "core/inventory/inventory.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using inventory::Program;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

struct ListOptions {
    bool all = false;
    bool broken = false;
    bool json = false;
    bool timing = false;
};

std::string date_text(const std::optional<std::chrono::sys_days>& date) {
    if (!date) {
        return {};
    }
    const std::chrono::year_month_day ymd{*date};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                       static_cast<unsigned>(ymd.day()));
}

Json to_json(const Program& p) {
    Json json;
    json["id"] = to_utf8(p.id);
    json["name"] = to_utf8(p.display_name);
    json["version"] = to_utf8(p.display_version);
    json["publisher"] = to_utf8(p.publisher);
    json["listed_in_settings"] = inventory::is_listed_in_settings(p);
    json["source"] = inventory::to_string(p.source);
    json["installer_type"] = inventory::to_string(p.installer_type);
    json["scope"] = inventory::to_string(p.scope);
    json["user_sid"] = to_utf8(p.user_sid);
    json["user_name"] = to_utf8(p.user_name);
    json["architecture"] = inventory::to_string(p.architecture);
    json["install_location"] = to_utf8(p.install_location);
    json["install_source"] = to_utf8(p.install_source);
    json["install_date"] = p.install_date ? Json(date_text(p.install_date)) : Json(nullptr);
    json["estimated_size_bytes"] = p.estimated_size_bytes;
    json["uninstall_string"] = to_utf8(p.uninstall_string);
    json["quiet_uninstall_string"] = to_utf8(p.quiet_uninstall_string);
    json["modify_path"] = to_utf8(p.modify_path);
    json["display_icon"] = to_utf8(p.display_icon);
    json["about_url"] = to_utf8(p.about_url);
    json["comments"] = to_utf8(p.comments);
    json["msi_product_code"] = to_utf8(p.msi_product_code);
    json["parent_key_name"] = to_utf8(p.parent_key_name);
    json["package_full_name"] = to_utf8(p.package_full_name);
    json["package_family_name"] = to_utf8(p.package_family_name);
    json["external_location"] = to_utf8(p.external_location);
    json["system_component"] = p.system_component;
    json["is_update"] = p.is_update;
    json["is_framework"] = p.is_framework;
    json["no_remove"] = p.no_remove;
    json["no_modify"] = p.no_modify;
    json["no_repair"] = p.no_repair;
    json["windows_installer"] = p.windows_installer;
    json["msi_product_unregistered"] = p.msi_product_unregistered;
    json["uninstaller_missing"] = p.uninstaller_missing;
    json["msi_package_missing"] = p.msi_package_missing;
    json["broken"] = inventory::is_broken(p);
    if (p.registry) {
        json["registry"] = {{"view", inventory::to_string(p.registry->view)},
                            {"key_name", to_utf8(p.registry->key_name)},
                            {"path", to_utf8(p.registry->path)}};
    } else {
        json["registry"] = nullptr;
    }
    return json;
}

void print_json(const inventory::Inventory& inventory, const std::vector<const Program*>& programs) {
    Json root;
    root["avel_version"] = kVersionString;
    root["elevated"] = platform::is_process_elevated();
    root["elapsed_ms"] = inventory.elapsed.count();
    root["timings"] = Json::array();
    for (const auto& timing : inventory.timings) {
        root["timings"].push_back({{"source", inventory::to_string(timing.source)},
                                   {"elapsed_ms", timing.elapsed.count()},
                                   {"count", timing.count}});
    }
    root["diagnostics"] = Json::array();
    for (const auto& diagnostic : inventory.diagnostics) {
        root["diagnostics"].push_back({{"code", inventory::to_string(diagnostic.code)},
                                       {"subject", to_utf8(diagnostic.subject)},
                                       {"error", diagnostic.error}});
    }
    root["programs"] = Json::array();
    for (const auto* program : programs) {
        root["programs"].push_back(to_json(*program));
    }
    print(root.dump(2) + "\n");
}

// Pads or truncates (with an ellipsis) to `width` UTF-16 code units, which matches console
// columns for Latin, Cyrillic and Armenian text.
std::wstring fit(std::wstring_view text, size_t width) {
    if (text.size() > width) {
        return std::wstring(text.substr(0, width - 1)) + L"…";
    }
    return std::wstring(text) + std::wstring(width - text.size(), L' ');
}

std::wstring type_label(const Program& p) {
    if (p.source == inventory::Source::Appx) {
        return L"store";
    }
    const auto type = inventory::to_string(p.installer_type);
    return std::wstring(type.begin(), type.end());
}

// Why a broken entry can't uninstall itself, and whether its folder is gone too (a dead entry).
std::string broken_reasons(const Program& p) {
    std::string reasons;
    const auto add = [&](std::string_view reason) {
        reasons += reasons.empty() ? "" : ", ";
        reasons += reason;
    };
    if (p.uninstaller_missing) {
        add("uninstaller-missing");
    }
    if (p.msi_package_missing) {
        add("msi-package-missing");
    }
    if (p.msi_product_unregistered) {
        add("msi-product-unregistered");
    }
    if (p.install_location.empty() ||
        ::GetFileAttributesW(p.install_location.c_str()) == INVALID_FILE_ATTRIBUTES) {
        add("no-install-folder");
    }
    return reasons;
}

void print_broken(const std::vector<const Program*>& programs) {
    for (const auto* p : programs) {
        const std::wstring name =
            p->display_name.empty() && p->registry ? L"(" + p->registry->key_name + L")" : p->display_name;
        print(std::format("{}\n    {}\n    {}\n", to_utf8(name), to_utf8(p->id), broken_reasons(*p)));
    }
}

void print_table(const std::vector<const Program*>& programs) {
    const int width = console_width();
    // Name, version, publisher, type; the name column absorbs extra width.
    const size_t version_width = 18;
    const size_t publisher_width = 26;
    const size_t type_width = 13;
    const size_t fixed = version_width + publisher_width + type_width + 3;
    const size_t name_width = width > 0 ? std::max<size_t>(24, static_cast<size_t>(width) - fixed - 1) : 48;

    std::wstring out = fit(L"Name", name_width) + L" " + fit(L"Version", version_width) + L" " +
                       fit(L"Publisher", publisher_width) + L" " + L"Type\n";
    out += std::wstring(name_width + fixed, L'-') + L"\n";
    for (const auto* p : programs) {
        const std::wstring name =
            p->display_name.empty() && p->registry ? L"(" + p->registry->key_name + L")" : p->display_name;
        out += fit(name, name_width) + L" " + fit(p->display_version, version_width) + L" " +
               fit(p->publisher, publisher_width) + L" " + type_label(*p) + L"\n";
    }
    print(to_utf8(out));
}

}  // namespace

int run_list_command(const std::vector<std::wstring_view>& arguments) {
    ListOptions options;
    for (const auto argument : arguments) {
        if (argument == L"--all") {
            options.all = true;
        } else if (argument == L"--broken") {
            options.broken = true;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument == L"--timing") {
            options.timing = true;
        } else {
            print_error("list: unknown option " + to_utf8(argument) + "\n");
            print_error("Usage: avel-cli list [--all | --broken] [--json] [--timing]\n");
            return 2;
        }
    }

    const auto inventory = inventory::scan_inventory();

    std::vector<const Program*> selected;
    for (const auto& program : inventory.programs) {
        // Broken entries count whether or not Settings shows them.
        if (options.broken ? inventory::is_broken(program)
                           : options.all || inventory::is_listed_in_settings(program)) {
            selected.push_back(&program);
        }
    }

    if (options.json) {
        print_json(inventory, selected);
    } else if (options.broken) {
        print_broken(selected);
        print(std::format("\n{} broken entr{}; `avel-cli force <id>` removes one\n", selected.size(),
                          selected.size() == 1 ? "y" : "ies"));
    } else {
        print_table(selected);
        const size_t hidden = inventory.programs.size() - selected.size();
        std::string summary = std::format("\n{} programs", selected.size());
        if (hidden > 0) {
            summary +=
                std::format(" ({} more hidden: updates, system components, frameworks; use --all)", hidden);
        }
        print(summary + "\n");
    }

    if (options.timing) {
        std::string text = std::format("Scan took {} ms:", inventory.elapsed.count());
        for (const auto& timing : inventory.timings) {
            text += std::format(" {} {} ms ({} entries);", inventory::to_string(timing.source),
                                timing.elapsed.count(), timing.count);
        }
        print_error(text + "\n");
    }
    for (const auto& diagnostic : inventory.diagnostics) {
        if (!options.json) {
            print_error(std::format("warning: {} {} (error {})\n", inventory::to_string(diagnostic.code),
                                    to_utf8(diagnostic.subject), diagnostic.error));
        }
    }
    return 0;
}

}  // namespace avel::cli
