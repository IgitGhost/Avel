# Unit tests for avel_core. They never modify the system: registry tests work in a volatile
# fixture key under HKCU\Software\Avel.Tests, the inventory smoke test only reads, and the process
# runner tests start a small helper program. Uninstalling real programs happens only in Windows
# Sandbox (see tests/sandbox).

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Imitates an uninstaller that starts a copy of itself and exits at once.
add_executable(avel_test_spawner helpers/spawner.cpp)
target_link_libraries(avel_test_spawner PRIVATE avel_options)

add_executable(avel_core_tests
    unit/command_line_test.cpp
    unit/inventory_test.cpp
    unit/name_matching_test.cpp
    unit/process_runner_test.cpp
    unit/registry_source_test.cpp
    unit/registry_values.h
    unit/related_entries_test.cpp
    unit/text_test.cpp
    unit/uninstall_entry_test.cpp
    unit/uninstall_plan_test.cpp
    unit/uninstaller_test.cpp)

target_link_libraries(avel_core_tests PRIVATE avel_core GTest::gtest_main ole32)
target_compile_definitions(avel_core_tests PRIVATE AVEL_TEST_SPAWNER=L"$<TARGET_FILE:avel_test_spawner>")
add_dependencies(avel_core_tests avel_test_spawner)

gtest_discover_tests(avel_core_tests DISCOVERY_MODE PRE_TEST)
