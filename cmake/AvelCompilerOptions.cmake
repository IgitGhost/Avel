# Shared compiler settings for every Avel target. Link against `avel_options` to inherit them.

add_library(avel_options INTERFACE)

target_compile_definitions(avel_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00   # Windows 10 and later
    WINVER=0x0A00)

target_compile_options(avel_options INTERFACE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /Zc:preprocessor
    /EHsc
    /bigobj   # C++/WinRT projections produce very large object files
    $<$<BOOL:${AVEL_WARNINGS_AS_ERRORS}>:/WX>)

# Whole-program optimization for release builds.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Release builds also get PDBs, so crashes (WER fault offsets, dumps) can be symbolized.
# /Z7 keeps debug info in the object files, which avoids PDB write contention under Ninja.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
target_link_options(avel_options INTERFACE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
