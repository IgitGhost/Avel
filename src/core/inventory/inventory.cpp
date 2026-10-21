#include "core/inventory/inventory.h"

#include <windows.h>

#include <wil/com.h>

#include <algorithm>
#include <future>
#include <unordered_map>
#include <unordered_set>

#include "core/inventory/appx_source.h"
#include "core/inventory/msi_source.h"
#include "core/inventory/registry_source.h"
#include "core/inventory/related_entries.h"
#include "core/inventory/uninstall_entry.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::inventory {

namespace {

using Clock = std::chrono::steady_clock;

void fill_missing(Program& target, const Program& source) {
    auto fill = [](std::wstring& to, const std::wstring& from) {
        if (to.empty()) {
            to = from;
        }
    };
    fill(target.display_name, source.display_name);
    fill(target.display_version, source.display_version);
    fill(target.publisher, source.publisher);
    fill(target.install_location, source.install_location);
    fill(target.install_source, source.install_source);
    fill(target.display_icon, source.display_icon);
    fill(target.about_url, source.about_url);
    if (!target.install_date) {
        target.install_date = source.install_date;
    }
    // Only an entry that can't be uninstalled at all borrows the product's uninstall command.
    if (target.uninstall_string.empty()) {
        target.uninstall_string = source.uninstall_string;
        fill(target.msi_product_code, source.msi_product_code);
        if (target.installer_type == InstallerType::Unknown) {
            target.installer_type = source.installer_type;
        }
    }
}

// Product codes an Uninstall entry is known by: its own MSI code, or a key named after a GUID
// (some Microsoft installers create bare keys like that).
std::wstring product_code_of(const Program& program) {
    if (!program.msi_product_code.empty()) {
        return program.msi_product_code;
    }
    if (program.registry && program.registry->key_name.size() == 38) {
        return extract_guid(program.registry->key_name).value_or(L"");
    }
    return {};
}

template <typename Function>
auto timed(Source source, Function&& function) {
    const auto start = Clock::now();
    auto result = function();
    SourceTiming timing{source, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start),
                        result.programs.size()};
    return std::make_pair(std::move(result), timing);
}

template <typename Function>
auto run_in_mta(Function function) {
    return std::async(std::launch::async, [function = std::move(function)] {
        const auto com = wil::CoInitializeEx(COINIT_MULTITHREADED);
        return function();
    });
}

bool name_less(const Program& a, const Program& b) {
    const int order =
        ::CompareStringOrdinal(a.display_name.c_str(), static_cast<int>(a.display_name.size()),
                               b.display_name.c_str(), static_cast<int>(b.display_name.size()), TRUE);
    if (order != CSTR_EQUAL) {
        return order == CSTR_LESS_THAN;
    }
    return a.id < b.id;
}

}  // namespace

void merge_msi_products(std::vector<Program>& programs, std::vector<Program> msi_products,
                        std::wstring_view current_user_sid, bool msi_scan_complete) {
    auto visible_to_current_user = [&](const Program& program) {
        return program.scope == Scope::Machine || platform::iequals(program.user_sid, current_user_sid);
    };
    std::unordered_set<std::wstring> installed_codes;
    for (const auto& product : msi_products) {
        if (visible_to_current_user(product)) {
            installed_codes.insert(product.msi_product_code);
        }
    }

    std::unordered_multimap<std::wstring, std::size_t> by_code;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (auto code = product_code_of(programs[i]); !code.empty()) {
            by_code.emplace(std::move(code), i);
        }
    }

    const std::size_t registry_count = programs.size();
    std::vector<Program> unmatched;
    for (auto& product : msi_products) {
        const auto [first, last] = by_code.equal_range(product.msi_product_code);
        if (first != last) {
            for (auto it = first; it != last; ++it) {
                fill_missing(programs[it->second], product);
                // The same product, so the same package.
                programs[it->second].msi_package_missing = product.msi_package_missing;
            }
            continue;
        }

        // Same rule as BCU's MergeResults: the best-scoring entry, if it reaches the threshold.
        int best_score = kRelatedThreshold - 1;
        std::size_t best = registry_count;
        for (std::size_t i = 0; i < registry_count; ++i) {
            const int score = relatedness_score(programs[i], product);
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best < registry_count) {
            fill_missing(programs[best], product);
        } else {
            unmatched.push_back(std::move(product));
        }
    }

    if (msi_scan_complete) {
        for (std::size_t i = 0; i < registry_count; ++i) {
            Program& program = programs[i];
            if (program.windows_installer && visible_to_current_user(program)) {
                const std::wstring code = product_code_of(program);
                program.msi_product_unregistered = !code.empty() && !installed_codes.contains(code);
            }
        }
    }

    programs.insert(programs.end(), std::make_move_iterator(unmatched.begin()),
                    std::make_move_iterator(unmatched.end()));
}

Inventory scan_inventory(const ScanOptions& options) {
    const auto start = Clock::now();
    Inventory inventory;

    auto registry_future = run_in_mta([&] {
        return options.registry ? timed(Source::Registry, scan_registry)
                                : std::make_pair(RegistryScanResult{}, SourceTiming{Source::Registry});
    });
    auto msi_future = run_in_mta([&] {
        return options.msi ? timed(Source::Msi, scan_msi_products)
                           : std::make_pair(MsiScanResult{}, SourceTiming{Source::Msi});
    });
    // The AppX source manages its own apartment, so it runs right here.
    auto appx = options.appx ? timed(Source::Appx, scan_appx_packages)
                             : std::make_pair(AppxScanResult{}, SourceTiming{Source::Appx});
    auto registry = registry_future.get();
    auto msi = msi_future.get();

    const bool msi_complete =
        std::none_of(msi.first.diagnostics.begin(), msi.first.diagnostics.end(),
                     [](const Diagnostic& d) { return d.code == DiagnosticCode::MsiEnumerationFailed; });
    inventory.programs = std::move(registry.first.programs);
    merge_msi_products(inventory.programs, std::move(msi.first.programs), platform::current_user_sid(),
                       options.msi && msi_complete);
    inventory.programs.insert(inventory.programs.end(), std::make_move_iterator(appx.first.programs.begin()),
                              std::make_move_iterator(appx.first.programs.end()));
    std::sort(inventory.programs.begin(), inventory.programs.end(), name_less);

    for (auto* diagnostics : {&registry.first.diagnostics, &msi.first.diagnostics, &appx.first.diagnostics}) {
        inventory.diagnostics.insert(inventory.diagnostics.end(), diagnostics->begin(), diagnostics->end());
    }
    inventory.timings = {registry.second, msi.second, appx.second};
    inventory.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return inventory;
}

}  // namespace avel::inventory
