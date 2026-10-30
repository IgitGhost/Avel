# Unit tests for avel_core. They never modify the system: registry tests work in a volatile
# fixture key under HKCU\Software\Avel.Tests, file tests in temporary folders, the inventory and
# leftover smoke tests only read, and the process runner and installer watch tests start a small
# helper program.
# Uninstalling real programs, and removing services and tasks, happens only in Windows Sandbox
# (see tests/sandbox).

find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
include(GoogleTest)

# Imitates uninstallers and installers that start other processes and exit at once.
add_executable(avel_test_spawner helpers/spawner.cpp)
target_link_libraries(avel_test_spawner PRIVATE avel_options)

# Dumps registry keys, files, services and tasks for the Sandbox to compare before a removal and
# after its undo; shares its dumpers with the unit tests.
add_executable(avel_state_dump helpers/state_dump.cpp unit/registry_dump.h unit/tree_dump.h)
target_include_directories(avel_state_dump PRIVATE unit)
target_link_libraries(avel_state_dump PRIVATE avel_core nlohmann_json::nlohmann_json ole32 oleaut32 taskschd)

# The install tracker's oracle in the Sandbox: captures files, keys and values with plain Win32
# calls (none of avel_core's code) and compares two captures.
add_executable(avel_capture helpers/capture.cpp)
target_link_libraries(avel_capture PRIVATE avel_options bcrypt)

add_executable(avel_core_tests
    unit/change_store_test.cpp
    unit/cli_options_test.cpp
    unit/command_line_test.cpp
    unit/deep_search_test.cpp
    unit/executor_test.cpp
    unit/executor_value_edit_test.cpp
    unit/file_changes_test.cpp
    unit/file_quarantine_test.cpp
    unit/file_tree.h
    unit/force_test.cpp
    unit/hash_test.cpp
    unit/install_trace_test.cpp
    unit/installer_watch_test.cpp
    unit/inventory_test.cpp
    unit/journal_trace_test.cpp
    unit/known_folders_test.cpp
    unit/known_values_test.cpp
    unit/leftover_scan_test.cpp
    unit/leftovers_smoke_test.cpp
    unit/linking_test.cpp
    unit/merge_test.cpp
    unit/msi_codes_test.cpp
    unit/msi_scanner_test.cpp
    unit/name_matching_test.cpp
    unit/noise_filter_test.cpp
    unit/nt_registry_test.cpp
    unit/orphans_test.cpp
    unit/paths_test.cpp
    unit/process_runner_test.cpp
    unit/product_name_test.cpp
    unit/protection_test.cpp
    unit/record_folders_test.cpp
    unit/registry_backup_test.cpp
    unit/registry_dump.h
    unit/registry_snapshot_smoke_test.cpp
    unit/registry_snapshot_test.cpp
    unit/registry_source_test.cpp
    unit/registry_values.h
    unit/related_entries_test.cpp
    unit/scratch_registry.h
    unit/serialization_test.cpp
    unit/temp_root.h
    unit/test_spawner.h
    unit/text_test.cpp
    unit/trace_export_test.cpp
    unit/trace_fakes.h
    unit/trace_items_test.cpp
    unit/trace_scanner_test.cpp
    unit/trace_session_test.cpp
    unit/trace_source_test.cpp
    unit/trace_store_test.cpp
    unit/tree_dump.h
    unit/uninstall_entry_test.cpp
    unit/uninstall_plan_test.cpp
    unit/uninstaller_test.cpp
    unit/usn_live_test.cpp
    unit/usn_parse_test.cpp
    unit/value_edits_test.cpp
    # The CLI's option parsing (avel-cli.exe isn't a library).
    ${PROJECT_SOURCE_DIR}/src/cli/command_options.cpp)

target_link_libraries(avel_core_tests PRIVATE avel_core GTest::gtest_main nlohmann_json::nlohmann_json ole32)
target_compile_definitions(avel_core_tests PRIVATE AVEL_TEST_SPAWNER=L"$<TARGET_FILE:avel_test_spawner>")
add_dependencies(avel_core_tests avel_test_spawner)

gtest_discover_tests(avel_core_tests DISCOVERY_MODE PRE_TEST)

# The GUI's leftover tree. avel.exe isn't a library, so the test builds the sources it needs from
# src/app. It runs without a QGuiApplication: nothing it calls needs one.
if(TARGET avel_app)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui)
    add_executable(avel_app_tests
        app/leftover_model_test.cpp
        ${PROJECT_SOURCE_DIR}/src/app/leftover_model.cpp
        ${PROJECT_SOURCE_DIR}/src/app/leftover_model.h
        ${PROJECT_SOURCE_DIR}/src/app/leftover_text.cpp
        ${PROJECT_SOURCE_DIR}/src/app/program_text.cpp)
    set_target_properties(avel_app_tests PROPERTIES AUTOMOC ON)
    target_include_directories(avel_app_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
    target_compile_definitions(avel_app_tests PRIVATE QT_NO_KEYWORDS QT_DISABLE_DEPRECATED_UP_TO=0x060C00)
    target_link_libraries(avel_app_tests PRIVATE avel_core Qt6::Gui GTest::gtest_main)
    # Qt's DLLs next to the test, so test discovery can start it.
    add_custom_command(TARGET avel_app_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:avel_app_tests>
                $<TARGET_FILE_DIR:avel_app_tests>
        COMMAND_EXPAND_LISTS)
    gtest_discover_tests(avel_app_tests DISCOVERY_MODE PRE_TEST)
endif()
