# Greenfield project — pinning to current latest-stable CMake (4.x, 2026).
# CMake has no formal LTS, but starting from latest-major gives us:
#   - The deprecation-cleanup baseline (pre-3.5 policies are now errors)
#   - First-class C++23 / modules / FetchContent improvements
#   - Cleanest target-export and presets story
# Latest at time of writing is 4.3.2. Anything in the 4.x line is fine.
cmake_minimum_required(VERSION 4.0)

# Single source of truth for the project version is the `VERSION` file at
# repo root (a one-line file containing `Major.Minor.Patch`). The deploy
# pipeline (DSS.DevOps cpp-app-deploy) bumps it; CMake reads it here.
# Keeping the version in one place avoids the classic CMakeLists-vs-VERSION
# drift on every release.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DSS_VERSION_RAW)
string(STRIP "${DSS_VERSION_RAW}" DSS_VERSION)

project(dss-code-prime VERSION ${DSS_VERSION} LANGUAGES CXX)

# ── Language standard ──
# C++23 is required for std::expected, std::format ranges support, etc.
# Verified compilers: MSVC 17.5+, GCC 13+, Clang 16+.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# This project uses ZERO C++20 modules (no .cppm/.ixx, no `export module`/`import std`
# — header-based C++23 via <expected>/<format>). CMake 3.28+ (policy CMP0155) scans
# every C++20+ TU for module dependencies BY DEFAULT, which (a) costs a gratuitous
# scan pass on cold builds and (b) puts module-mapper/dyndep flags on every compile
# command — and ccache flags any module-mapper compile as UNCACHEABLE (~94% of CI
# compiles were uncacheable because of this). Disabling the scan is safe (no modules)
# and a double win: faster cold builds + ccache can finally cache the compiles.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default visibility to hidden on shared builds (only DSS_EXPORT symbols are public)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# OBJECT libraries under src/ (core, program, ...) feed dss-code-prime-lib,
# which is SHARED. CMake doesn't auto-enable -fPIC on OBJECT libs, so without
# this the Linux link step rejects their .o files with "relocation ... cannot
# be used when making a shared object; recompile with -fPIC". Setting it
# globally also future-proofs any STATIC-into-SHARED linking we add later.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Place DSS compiler binaries in bin/dss/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/dss)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/dss)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── Options ──
option(DSS_BUILD_TESTS "Build unit tests" ON)

# ── External dependencies (FetchContent) ──
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# nlohmann/json — JSON parsing for language config files.
# Used only by core/types/grammar_schema_json.cpp; not in the public include surface.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# GoogleTest — only when tests are enabled.
if(DSS_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    # Prevent overriding the parent project's compiler/linker settings on Windows.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ── MSVC: /bigobj (raise the COFF section limit) ──
# Several large translation units (semantic_analyzer.cpp, cst_to_hir.cpp,
# hir_to_mir.cpp …) have grown past the default COFF object-file section limit in
# the DEBUG configuration (no inlining + full debug sections) — MSVC fatal
# `C1128: ... compile with /bigobj`. Release stays under the cap, so it surfaces
# only in Debug, breaking the Debug ctest build. /bigobj raises the limit; it is
# harmless in Release and a NO-OP on every non-MSVC toolchain (ELF / Mach-O have
# no such cap), so it stays target-agnostic. Applied project-wide for MSVC after
# the FetchContent deps (so it scopes to this project's targets), consistent with
# the per-compiler global-flag convention below (the PCH /Brepro block).
if(MSVC)
    add_compile_options(/bigobj)
endif()

# ── Precompiled headers (build-time only — D-CI-WINDOWS-MSVC-BUILD-TIME-CEILING) ──
# The windows-msvc-release CI leg re-parses the same heavy, templated STL
# headers (<format>, <unordered_map>, <vector>, …) in every one of the ~280
# translation units. On the 2-core CI runner that repeated parse dominates the
# cold build and pushed the leg past its 45-minute ceiling. Precompiling this
# stable STL set ONCE and reusing it removes the repeated parse.
#
# This is a PURE build-time optimization — every TU still sees the same headers
# in the same order, so NO emitted code changes (single-CU byte-identity holds).
# The list is deliberately STL-ONLY (no project headers) so that (a) editing our
# own code never invalidates the PCH — incremental dev builds stay fast — and
# (b) the PCH can never mask a missing project-header include. `DSS_PCH_HEADERS`
# is consumed by src/ (one shared producer, reused across the object libs) and
# tests/ (a second shared producer, reused across the test executables).
set(DSS_PCH_HEADERS
    <algorithm>
    <array>
    <cstddef>
    <cstdint>
    <expected>
    <filesystem>
    <format>
    <functional>
    <limits>
    <memory>
    <optional>
    <span>
    <string>
    <string_view>
    <type_traits>
    <unordered_map>
    <unordered_set>
    <utility>
    <variant>
    <vector>)

# Debug-build canary: compile PCH-free in Debug so the one Debug CI leg
# (linux-clang-asan, a single-config Ninja build with CMAKE_BUILD_TYPE=Debug)
# proves every TU's #includes are self-sufficient. A TU that silently leaned on
# a PCH-provided header would compile on the PCH Release legs but FAIL here —
# turning the classic "PCH masks a missing include" trap into a hard CI error
# rather than a latent portability bug. (Multi-config local generators leave
# CMAKE_BUILD_TYPE empty, so PCH stays on there; the seam is the single-config
# Debug CI leg, which is exactly where we want the PCH-free proof to run.)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
endif()

# ── Reproducible PCH binaries for cross-run ccache hits (Clang + MSVC) ──
# ccache hashes the precompiled header as an input to EVERY dependent TU, so a
# PCH that is not byte-reproducible across CI runs makes every compile look new.
# Measured on PR #30 (run 28060528181): GCC's `.gch` is already reproducible
# (linux-gcc / linux-arm64 ≈ 99.7% cross-run hits) but Clang and MSVC bake a
# build TIMESTAMP into the PCH → macOS ≈ 1.2% hits, Windows ≈ 6.9% hits → cold
# rebuilds every run. The fix is per-compiler, applied where PCH is active:
#   * Clang / AppleClang — `-Xclang -fno-pch-timestamp` strips the timestamp
#     Clang embeds in the `.pch` (the documented ccache+Clang+PCH remedy).
#   * MSVC — `/Brepro` zeros the embedded TIMESTAMP, but a LOCAL measurement
#     (two clean PCH builds on VS18/MSVC, this change) shows ~36 residual
#     non-deterministic bytes per 178 MB `.pch` — ASLR-influenced absolute
#     addresses in MSVC's memory-image PCH that `/Brepro` does not touch. ccache
#     hashes the WHOLE `.pch`, so 36 bytes ≠ 0 ⇒ Windows STILL misses. `/Brepro`
#     is kept as the harmless, documented best-effort (and the prerequisite for a
#     future `/experimental:deterministic` attempt); the Windows-leg caching
#     strategy is a pending HUMAN decision (drop PCH on the MSVC leg so ccache
#     caches normally · MinGW-GCC for PCH+cache but no MSVC coverage · status
#     quo). It compiles + links clean, so carrying it is no-harm.
# GCC is deliberately UNTOUCHED (already optimal). The flag goes on every PCH
# producer AND every REUSE_FROM consumer: Clang/MSVC validate the PCH against the
# consumer's compile line, so a producer-only flag would make the consumer
# REJECT the timestamp-free PCH ("file modified since PCH was built") and break
# the build — both sides must carry it. No-op when PCH is disabled (the Debug
# canary leg) and on any compiler that is neither Clang nor MSVC, and each flag
# is per-compiler-guarded so it never leaks to a toolchain that would reject it.
function(dss_pch_reproducible target)
    if(CMAKE_DISABLE_PRECOMPILE_HEADERS)
        return()  # PCH off (Debug canary) — there is no PCH binary to stabilize
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")        # Clang AND AppleClang
        target_compile_options(${target} PRIVATE -Xclang -fno-pch-timestamp)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /Brepro)
    endif()
endfunction()

add_subdirectory(src)

if(DSS_BUILD_TESTS)
    # `integrated_tests` is a test harness (drives the dss-code-prime
    # CLI as a subprocess + asserts OS exit codes from spawned
    # binaries). It uses `enable_testing()` + `add_test()`, so it
    # must live inside the DSS_BUILD_TESTS gate alongside the unit
    # tests — otherwise `add_test()` silently no-ops when the gate
    # is off (no testing context in the calling scope) and CI would
    # build the binary but never invoke it through ctest. This gate
    # IS the fix; no separate anchor is tracked.
    add_subdirectory(integrated_tests)
    add_subdirectory(tests)

    # Anchor-registry CI guard (2026-06-03 — per the cross-plan
    # staleness sweep + the standing discipline that "make the
    # system enforce it" is the right move for recurring leaks).
    # Every `D-*` anchor cited in src/ must resolve to a row /
    # citation in any .plans/*.md file. Same shape as the
    # agnosticism veto + the CallConv static_assert — system-
    # enforced rather than convention-enforced.
    #
    # Dispatches to the platform-appropriate script. The bash
    # variant runs on Linux/macOS CI; the PowerShell variant runs
    # on Windows CI. Both share the same contract + registry file.
    if(WIN32)
        find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)
        add_test(
            NAME anchor_registry_guard
            COMMAND ${POWERSHELL_EXE}
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/tools/check-anchor-registry.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    else()
        add_test(
            NAME anchor_registry_guard
            COMMAND bash "${CMAKE_SOURCE_DIR}/tools/check-anchor-registry.sh"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endif()
