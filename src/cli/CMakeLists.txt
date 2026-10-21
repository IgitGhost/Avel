# avel-cli.exe: the scripting front end (English-only output, JSON where it matters).

find_package(nlohmann_json CONFIG REQUIRED)

configure_file(avel-cli.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/avel-cli.manifest" @ONLY)

add_executable(avel_cli
    change_output.cpp
    change_output.h
    console.cpp
    console.h
    force_command.cpp
    force_command.h
    leftover_output.cpp
    leftover_output.h
    list_command.cpp
    list_command.h
    main.cpp
    orphans_command.cpp
    orphans_command.h
    program_lookup.cpp
    program_lookup.h
    scan_command.cpp
    scan_command.h
    uninstall_command.cpp
    undo_command.cpp
    undo_command.h
    uninstall_command.h
    "${CMAKE_CURRENT_BINARY_DIR}/avel-cli.manifest")

set_target_properties(avel_cli PROPERTIES OUTPUT_NAME avel-cli)

# The manifest already carries trustInfo; stop the linker from adding a second, conflicting one.
target_link_options(avel_cli PRIVATE /MANIFESTUAC:NO)

target_link_libraries(avel_cli PRIVATE avel_core nlohmann_json::nlohmann_json)
