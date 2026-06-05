add_subdirectory(core)
add_subdirectory(tokenizer)
add_subdirectory(analysis/syntactic)
add_subdirectory(analysis/compilation_unit)
add_subdirectory(analysis/semantic)
add_subdirectory(hir)
add_subdirectory(hir/lowering)
add_subdirectory(mir)
add_subdirectory(mir/lowering)
add_subdirectory(opt)
add_subdirectory(lir)
add_subdirectory(asm)
add_subdirectory(link)
add_subdirectory(mir/merge)
add_subdirectory(ffi)
add_subdirectory(lsp)
add_subdirectory(program)

# ── Shared library (DLL / .so / .dylib) ──
# All compiler functionality in a single shared library for embedding in other projects.
add_library(dss-code-prime-lib SHARED
    $<TARGET_OBJECTS:core>
    $<TARGET_OBJECTS:tokenizer>
    $<TARGET_OBJECTS:analysis_syntactic>
    $<TARGET_OBJECTS:analysis_compilation_unit>
    $<TARGET_OBJECTS:analysis_semantic>
    $<TARGET_OBJECTS:hir>
    $<TARGET_OBJECTS:hir_lowering>
    $<TARGET_OBJECTS:mir>
    $<TARGET_OBJECTS:mir_lowering>
    $<TARGET_OBJECTS:mir_merge>
    $<TARGET_OBJECTS:opt>
    $<TARGET_OBJECTS:lir>
    $<TARGET_OBJECTS:lir_lowering>
    $<TARGET_OBJECTS:asm>
    $<TARGET_OBJECTS:link>
    $<TARGET_OBJECTS:ffi>
    $<TARGET_OBJECTS:ffi_abi>
    $<TARGET_OBJECTS:ffi_mangling>
    $<TARGET_OBJECTS:lsp>
    $<TARGET_OBJECTS:program>)
target_include_directories(dss-code-prime-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# PUBLIC propagates to consumers (the CLI exe, tests) so they see
# __declspec(dllimport) at compile time. The matching DSS_BUILDING_DLL lives
# on each OBJECT lib (core / program / ...) — not here — because this target has
# no sources of its own, only aggregated $<TARGET_OBJECTS:...>.
target_compile_definitions(dss-code-prime-lib PUBLIC DSS_SHARED_BUILD)

set_target_properties(dss-code-prime-lib PROPERTIES
    OUTPUT_NAME "dss-code-prime"
)

# ── Executable (CLI) ──
# Thin wrapper that delegates to Program::run().
add_executable(dss-code-prime main.cpp)
target_link_libraries(dss-code-prime PRIVATE dss-code-prime-lib)

# ── MinGW runtime DLLs alongside the binaries (Windows + GCC only) ──
# Every exe (CLI + test binaries) loads libstdc++-6 / libgcc_s_seh-1 /
# libwinpthread-1 at process start. Windows resolves those via the DLL
# search order, where the EXE's own directory is #1 — ahead of PATH. When
# the build toolchain (e.g. Strawberry MinGW, UCRT) is NOT first on PATH —
# such as running ctest from an MSYS2 MINGW64 shell that injects its own
# /mingw64/bin — a *different* libstdc++ loads and the process dies at load
# with STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139). Copying the toolchain's own
# runtime next to the binaries makes the load PATH-independent and the
# binaries self-contained. dirname(CMAKE_CXX_COMPILER) is this toolchain's
# bin dir, so each dev machine ships its own matching runtime. No-op on MSVC
# (system UCRT) and non-Windows (no DLLs).
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_filename_component(_dss_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_dss_runtime_dlls "")
    foreach(_dll libstdc++-6 libgcc_s_seh-1 libwinpthread-1)
        if(EXISTS "${_dss_toolchain_bin}/${_dll}.dll")
            list(APPEND _dss_runtime_dlls "${_dss_toolchain_bin}/${_dll}.dll")
        endif()
    endforeach()
    if(_dss_runtime_dlls)
        add_custom_command(TARGET dss-code-prime-lib POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_dss_runtime_dlls} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMAND_EXPAND_LISTS
            VERBATIM
            COMMENT "Copying MinGW runtime DLLs next to binaries (PATH-independent load)")
    endif()
endif()
