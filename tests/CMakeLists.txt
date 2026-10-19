# Unit tests for avel_core. They never modify the system: registry tests work in a volatile
# fixture key under HKCU\Software\Avel.Tests, file tests in temporary folders, the inventory and
# leftover smoke tests only read, and the process runner tests start a small helper program.
# Uninstalling real programs, and removing services and tasks, happens only in Windows Sandbox
# (see tests/sandbox).

find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
include(GoogleTest)

# Imitates an uninstaller that starts a copy of itself and exits at once.
add_executable(avel_test_spawner helpers/spawner.cpp)
target_link_libraries(avel_test_spawner PRIVATE avel_options)

# Dumps registry keys, files, services and tasks for the Sandbox to compare before a removal and
# after its undo; shares its dumpers with the unit tests.
add_executable(avel_state_dump helpers/state_dump.cpp unit/registry_dump.h unit/tree_dump.h)
target_include_directories(avel_state_dump PRIVATE unit)
target_link_libraries(avel_state_dump PRIVATE avel_core nlohmann_json::nlohmann_json ole32 oleaut32 taskschd)

add_executable(avel_core_tests
    unit/change_store_test.cpp
    unit/command_line_test.cpp
    unit/executor_test.cpp
    unit/file_quarantine_test.cpp
    unit/file_tree.h
    unit/inventory_test.cpp
    unit/known_folders_test.cpp
    unit/leftover_scan_test.cpp
    unit/leftovers_smoke_test.cpp
    unit/merge_test.cpp
    unit/name_matching_test.cpp
    unit/paths_test.cpp
    unit/process_runner_test.cpp
    unit/product_name_test.cpp
    unit/protection_test.cpp
    unit/registry_backup_test.cpp
    unit/registry_dump.h
    unit/registry_source_test.cpp
    unit/registry_values.h
    unit/related_entries_test.cpp
    unit/scratch_registry.h
    unit/text_test.cpp
    unit/tree_dump.h
    unit/uninstall_entry_test.cpp
    unit/uninstall_plan_test.cpp
    unit/uninstaller_test.cpp)

target_link_libraries(avel_core_tests PRIVATE avel_core GTest::gtest_main nlohmann_json::nlohmann_json ole32)
target_compile_definitions(avel_core_tests PRIVATE AVEL_TEST_SPAWNER=L"$<TARGET_FILE:avel_test_spawner>")
add_dependencies(avel_core_tests avel_test_spawner)

gtest_discover_tests(avel_core_tests DISCOVERY_MODE PRE_TEST)
