#include "core/leftovers/scan.h"

#include <windows.h>
#include <objbase.h>

#include <wil/resource.h>

#include <algorithm>
#include <future>

#include "core/leftovers/merge.h"
#include "core/leftovers/protection.h"
#include "core/leftovers/scanners.h"
#include "core/platform/text.h"

namespace avel::leftovers {

ScanResult scan_leftovers(const ScanEnvironment& environment, std::span<const ScanTarget> targets,
                          std::span<const inventory::Program> installed, const ScanOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    // Shortcuts are resolved through COM. The caller may already have it, in either apartment.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize = wil::scope_exit([&] {
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });

    ScanResult result;
    result.diagnostics = environment.diagnostics;
    if (targets.empty()) {
        return result;
    }

    std::vector<inventory::Program> others;
    for (const auto& program : installed) {
        const bool is_target = std::any_of(targets.begin(), targets.end(), [&](const ScanTarget& target) {
            return platform::iequals(target.program_id, program.id);
        });
        if (!is_target) {
            others.push_back(program);
        }
    }

    const auto since = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    };
    // Windows Installer's components are read on their own thread while the snapshots are taken;
    // their time overlaps the snapshots'.
    auto installer = std::async(std::launch::async, [&] {
        detail::Snapshots part;
        const auto started = std::chrono::steady_clock::now();
        detail::take_msi_clients(environment, targets, part);
        part.timings.push_back({"installer", since(started)});
        return part;
    });
    // So are the volumes' folders for the deep search.
    std::future<detail::Snapshots> deep;
    if (options.deep) {
        deep = std::async(std::launch::async, [&] {
            detail::Snapshots part;
            const auto started = std::chrono::steady_clock::now();
            detail::take_deep_folders(environment, part, result.diagnostics);
            part.timings.push_back({"deep", since(started)});
            return part;
        });
    }
    detail::Snapshots snapshots = detail::take_snapshots(environment);
    detail::Snapshots part = installer.get();
    snapshots.msi_clients = std::move(part.msi_clients);
    snapshots.timings.push_back(part.timings.front());
    if (deep.valid()) {
        part = deep.get();
        snapshots.deep = std::move(part.deep);
        snapshots.timings.push_back(part.timings.front());
    }
    result.timings = snapshots.timings;
    const HardFloor& floor = HardFloor::live();
    const ProtectedPolicy policy(snapshots.known_folders);
    const auto is_protected = [&](const Location& location) {
        return floor.check(location) != ProtectionReason::None || policy.check(location) != ProtectionReason::None;
    };

    auto stage = std::chrono::steady_clock::now();
    // The traces' value edits first, one per value for all targets: the SharedDLLs counts they
    // leave tell which traced DLLs another program still uses.
    const detail::TraceValuePlan trace_values = detail::plan_trace_values(environment, targets);
    std::vector<Leftover> found;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        ScanTarget target = targets[index];
        if (target.install_location.empty()) {
            target.install_location = detail::msi_install_folder(
                environment, snapshots, target,
                [&](const std::wstring& folder) { return is_protected(FileLocation{folder, true}); });
        }
        // A recorded folder that is protected, or holds protected folders (C:\Program Files),
        // would make everything inside it look like the program's.
        for (auto* folder : {&target.install_location, &target.uninstaller_folder}) {
            if (!folder->empty() && is_protected(FileLocation{*folder, true})) {
                folder->clear();
            }
        }
        if (target.uninstaller_folder.empty()) {
            target.uninstaller.clear();
        }
        result.targets.push_back(target);
        const auto context = detail::make_context(environment, snapshots, target, others);
        detail::scan_install_location(context, found);
        detail::scan_common_drive(context, found);
        detail::scan_shortcuts(context, found);
        detail::scan_software_keys(context, found);
        detail::scan_uninstaller_key(context, found);
        detail::scan_references(context, found);
        detail::scan_related_packages(context, found);
        detail::scan_services(context, found);
        detail::scan_tasks(context, found);
        detail::scan_com_classes(context, found);
        detail::scan_firewall_rules(context, found);
        detail::scan_msi_registration(context, found);
        detail::scan_deep(context, found);
        // Last, so an item other scanners found too keeps their scanner (and its group).
        if (!target.trace_id.empty()) {
            TraceUse use;
            use.program_id = target.program_id;
            use.trace_id = target.trace_id;
            detail::scan_trace(context, trace_values, found, use);
            for (std::size_t i = 0; i < target.traced_values.size(); ++i) {
                const auto& value = target.traced_values[i];
                use.values.push_back({value.key, value.name, trace_values.bases[index][i]});
            }
            result.traces.push_back(std::move(use));
        }
    }
    detail::add_trace_value_edits(trace_values, result.targets, found);
    merge_duplicates(found);
    result.timings.push_back({"matching", since(stage)});

    // The traces' counts, for the items they gave evidence to (by the programs that own them).
    const auto count_for_traces = [&](const Leftover& item, auto&& count) {
        const bool traced = std::any_of(item.records.begin(), item.records.end(), [](const Record& record) {
            return is_trace_evidence(record.evidence);
        });
        if (!traced) {
            return;
        }
        for (auto& use : result.traces) {
            if (std::any_of(item.owner_ids.begin(), item.owner_ids.end(),
                            [&](const std::wstring& id) { return platform::iequals(id, use.program_id); })) {
                count(use);
            }
        }
    };
    stage = std::chrono::steady_clock::now();
    for (auto& item : found) {
        ProtectionReason reason = floor.check(item.location);
        if (reason == ProtectionReason::None) {
            reason = policy.check(item.location);
        }
        if (reason == ProtectionReason::None) {
            result.leftovers.push_back(std::move(item));
        } else {
            ++result.protected_count;
            count_for_traces(item, [](TraceUse& use) { ++use.protected_count; });
        }
    }
    link_containment(result.leftovers);
    for (const auto& item : result.leftovers) {
        const bool edit = std::holds_alternative<ValueEditLocation>(item.location);
        const bool tied = std::any_of(item.records.begin(), item.records.end(), [](const Record& record) {
            return record.evidence == Evidence::TracedInstallRoot;
        });
        count_for_traces(item, [&](TraceUse& use) {
            ++(edit ? use.value_edits : use.items);
            if (!edit && tied) {
                ++use.tied;
            }
        });
    }
    result.timings.push_back({"protection", since(stage)});
    result.elapsed = since(started);
    return result;
}

}  // namespace avel::leftovers
