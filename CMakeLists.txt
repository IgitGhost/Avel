cmake_minimum_required(VERSION 3.28)

project(Avel
    VERSION 0.1.0
    DESCRIPTION "Professional software removal suite for Windows"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Avel is a Windows-only project.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Avel requires the MSVC toolchain (C++/WinRT and WIL depend on it).")
endif()

option(AVEL_BUILD_APP "Build the Qt GUI (avel.exe)" ON)
option(AVEL_BUILD_TESTS "Build the unit tests" ON)
option(AVEL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every executable in one folder so the CLI, the GUI and the tests can be run side by side
# (and mapped into Windows Sandbox as a single directory).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/AvelCompilerOptions.cmake)

# `deploy` makes the bin folder self-contained (MSVC runtime, plus Qt when the GUI is built) so it
# runs in Windows Sandbox or on a machine without the toolchain:
#   cmake --build --preset release --target deploy
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
add_custom_target(deploy
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMENT "Copying the MSVC runtime next to the executables"
    VERBATIM)

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(AVEL_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(AVEL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
