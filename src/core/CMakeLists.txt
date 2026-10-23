# avel_core: the Qt-free engine shared by the CLI and the GUI.
# It returns codes and data, never user-facing text, so all translation happens in the GUI.

find_package(nlohmann_json CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/core/version.h" @ONLY)

add_library(avel_core STATIC
    changes/change_set.cpp
    changes/change_set.h
    changes/executor.cpp
    changes/executor.h
    changes/file_quarantine.cpp
    changes/file_quarantine.h
    changes/locks.cpp
    changes/locks.h
    changes/registry_backup.cpp
    changes/registry_backup.h
    changes/serialization.cpp
    changes/serialization.h
    changes/store.cpp
    changes/store.h
    changes/system_items.cpp
    changes/system_items.h
    force/force.cpp
    force/force.h
    inventory/appx_source.cpp
    inventory/appx_source.h
    inventory/diagnostic.cpp
    inventory/diagnostic.h
    inventory/inventory.cpp
    inventory/inventory.h
    inventory/msi_codes.cpp
    inventory/msi_codes.h
    inventory/msi_source.cpp
    inventory/msi_source.h
    inventory/name_matching.cpp
    inventory/name_matching.h
    inventory/program.cpp
    inventory/program.h
    inventory/registry_source.cpp
    inventory/registry_source.h
    inventory/related_entries.cpp
    inventory/related_entries.h
    inventory/uninstall_entry.cpp
    inventory/uninstall_entry.h
    leftovers/confidence.cpp
    leftovers/confidence.h
    leftovers/deep_scanner.cpp
    leftovers/drive_scanners.cpp
    leftovers/environment.cpp
    leftovers/environment.h
    leftovers/leftover.cpp
    leftovers/leftover.h
    leftovers/merge.cpp
    leftovers/merge.h
    leftovers/msi_scanner.cpp
    leftovers/orphans.cpp
    leftovers/orphans.h
    leftovers/post_uninstall.cpp
    leftovers/post_uninstall.h
    leftovers/product_name.cpp
    leftovers/product_name.h
    leftovers/protection.cpp
    leftovers/protection.h
    leftovers/registry_scanners.cpp
    leftovers/scan.cpp
    leftovers/scan.h
    leftovers/scan_context.cpp
    leftovers/scan_target.cpp
    leftovers/scan_target.h
    leftovers/scanners.h
    leftovers/snapshots.cpp
    leftovers/system_scanners.cpp
    leftovers/system_snapshots.cpp
    platform/com_security.cpp
    platform/com_security.h
    platform/command_line.cpp
    platform/command_line.h
    platform/elevation.cpp
    platform/elevation.h
    platform/file_ops.cpp
    platform/file_ops.h
    platform/hash.h
    platform/known_folders.cpp
    platform/known_folders.h
    platform/nt_registry.cpp
    platform/nt_registry.h
    platform/paths.cpp
    platform/paths.h
    platform/processes.cpp
    platform/processes.h
    platform/record_folders.cpp
    platform/record_folders.h
    platform/registry.cpp
    platform/registry.h
    platform/registry_address.h
    platform/secure_folder.cpp
    platform/secure_folder.h
    platform/text.cpp
    platform/text.h
    platform/time.cpp
    platform/time.h
    platform/user_hives.cpp
    platform/user_hives.h
    platform/user_profiles.cpp
    platform/user_profiles.h
    platform/version_info.cpp
    platform/volume_folders.cpp
    platform/volume_folders.h
    platform/version_info.h
    tracker/trace.cpp
    tracker/trace.h
    uninstall/batch.cpp
    uninstall/batch.h
    uninstall/installer_detection.cpp
    uninstall/installer_detection.h
    uninstall/process_runner.cpp
    uninstall/process_runner.h
    uninstall/registration.cpp
    uninstall/registration.h
    uninstall/restore_point.cpp
    uninstall/restore_point.h
    uninstall/uninstall_plan.cpp
    uninstall/uninstall_plan.h
    uninstall/uninstaller.cpp
    uninstall/uninstaller.h)

target_include_directories(avel_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/include")

# WIL is public because platform/registry.h exposes wil::unique_hkey.
# C++/WinRT headers come from the Windows SDK; windowsapp.lib provides the WinRT runtime.
target_link_libraries(avel_core
    PUBLIC avel_options WIL::WIL
    PRIVATE nlohmann_json::nlohmann_json advapi32 msi ole32 oleaut32 rstrtmgr shell32 shlwapi taskschd version windowsapp
            xmllite)
