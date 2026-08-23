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

# D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT — strict arm verdicts.
#
# WHAT IT DOES. Both corpus harnesses ledger a verdict for every declared target
# arm. An ENVIRONMENTAL skip — the manifest declared an `emulator` and this
# machine does not have it on PATH — is a visible WARNING by default and a hard
# FAILURE when `DSS_STRICT_ARM_VERDICTS=1` is in the test's environment.
#
# WHY IT IS OFF BY DEFAULT, and this is a requirement rather than timidity: a
# developer without qemu must still get a usable green suite. On a Linux x86_64
# box without `qemu-aarch64`, strict mode reds 449 corpus manifests — correctly,
# but that is a GATE posture, not a working-tree posture.
#
# WHY THE CMAKE OPTION EXISTS AT ALL, given the env var already works. The gate
# is driven by scripts OUTSIDE this repo (the operator's 3-leg driver), which
# this project cannot edit. A cache variable is the one chokepoint that reaches
# them: configure once with `-DDSS_STRICT_ARM_VERDICTS=ON` and EVERY subsequent
# `ctest` from any script, in any shell, inherits it — no recipe edit required.
# Exporting the variable in each gate shell would work too and is the documented
# alternative; the two are equivalent by design, because the runners read only
# the environment and neither knows which one set it.
#
# ⚠ DELIBERATELY NOT TURNED ON IN `.github/workflows/pipeline-pr.yml`. Those legs
# install no qemu at all, so strict mode there would red ~449 arms x 2 legs on
# the first PR. D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT sequences the LEDGER
# before any qemu-in-CI work precisely so that adding an emulator later is a
# measurable delta rather than an unverifiable claim. Flip CI on in the same
# change that installs qemu, not before.
option(DSS_STRICT_ARM_VERDICTS
       "Fail (not warn) when a corpus arm declared an emulator this machine lacks"
       OFF)

# Stamp the strict-mode variable onto ONE ctest entry. A no-op when the option
# is OFF, so the property is absent rather than present-and-false — an
# `ENVIRONMENT` entry spelling `=0` would work, but it would also silently
# OVERRIDE an operator who exported `=1` in their gate shell, which is the exact
# "a stale value quietly disabled the gate" failure the runners' strict parse
# refuses to allow.
function(dss_strict_arm_verdicts test_name)
    if(NOT DSS_STRICT_ARM_VERDICTS)
        return()
    endif()
    get_test_property(${test_name} ENVIRONMENT existing_env)
    if(existing_env STREQUAL "NOTFOUND")
        set(existing_env "")
    endif()
    list(APPEND existing_env "DSS_STRICT_ARM_VERDICTS=1")
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${existing_env}")
endfunction()

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

# ── GNU-on-Windows (MinGW/binutils): -Wa,-mbig-obj (COFF section limit) ──
# D-BUILD-GNU-WINDOWS-BIGOBJ-SCOPE. The SAME COFF object-section-count cap the
# MSVC /bigobj above raises also hits the GNU-on-Windows assembler (binutils
# `as`) on large TUs — and NOW on large TEST TUs too (`tests/lir/test_mir_to_lir.cpp`,
# ~6.7k lines, overflows in Debug: "can't write … 'file too big'"). The MSVC
# /bigobj above is PROJECT-WIDE (it reaches every src/ + tests/ target, added
# below), but the GNU-on-Windows remedy was scoped per-FILE to ONE TU
# (grammar_schema_json.cpp) in src/core/CMakeLists.txt — leaving every OTHER large
# TU (notably test TUs) exposed, an asymmetry that broke the local GNU-on-Windows
# (MinGW/Strawberry-g++) Debug ctest build. Apply it PROJECT-WIDE here (mirroring
# /bigobj), gated GNU AND Windows via the generator expression so it expands to
# NOTHING for MSVC, Clang, and Linux/macOS GCC — CRUCIAL because the Linux/macOS
# GNU `as` REJECTS -mbig-obj ("unrecognized option"), and GNU-on-Linux shares the
# same CXX_COMPILER_ID:GNU as MinGW. A COFF-object-size accommodation, not a
# language/CPU/format branch. This generalizes (and makes redundant) the per-file
# scoping in src/core/CMakeLists.txt, which is retained there as a defensive
# belt for that one critical generated TU.
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>")

# ── Exhaustive-switch enforcement, PROJECT-WIDE ──
# D-BUILD-CLANG-ONLY-WARNINGS-INVISIBLE-TO-THE-MSVC-AND-GCC-LEGS, closing half.
#
# ★ WHAT THE MEASUREMENT ACTUALLY SHOWED, because the anchor row got it wrong.
# The row recorded the macOS-only `-Wswitch` finding as a CLANG-ONLY warning —
# a toolchain gap, to be closed by adding a clang leg to the routine gate.
# ✔MEASURED: it is nothing of the kind. GCC 13 reports the identical
# `enumeration value 'X' not handled in switch [-Wswitch]` at the identical
# line the moment `-Wall` (or `-Wswitch` alone) is on the command line, and a
# sweep of EVERY CMakeLists.txt in this tree found NO `-Wall`, NO `-Wextra`
# and NO `/W4` anywhere. Clang saw it only because AppleClang's DEFAULTS
# include `-Wswitch`; MSVC's equivalent (C4062) is off by default at every
# `/W` level. So the class was invisible for a reason that costs nothing to
# fix and needs no new CI leg: NOBODY ASKED FOR IT. A gate gap, not a
# compiler quirk — and a whole warning class hid behind the mis-diagnosis.
#
# ★ WHY IT IS PROJECT-WIDE AND NOT A SIXTH HAND-LISTED RATCHET. This gate
# already existed FIVE TIMES over, per-file, in five different CMakeLists
# (src/asm, src/opt, src/lir, src/core, src/mir/lowering — see the anchor
# notes retained at each site). The file that shipped the defect,
# `src/asm/asm_text_to_lir.cpp`, sits in the SAME TARGET as four files the
# src/asm ratchet covers and was simply not on its hand-written list; its own
# comment even describes the drift as surfacing "only ... on the macOS /
# linux-clang CI legs". Duplication is what bred the missed site, so the
# remedy is the section-A.5 move: ONE chokepoint, coverage by construction,
# no list to forget a file from.
#
# ⚠ THIS IS THE NO-`default` FLAVOR, AND THAT IS THE WHOLE REASON IT IS SAFE
# TO GENERALIZE. `-Wswitch` / C4062 fire ONLY on a switch over an enum that
# has NO `default:` arm and is missing an enumerator. Every subset-switch in
# this tree that deliberately carries a `default:` is untouched — the five
# per-file blocks each said so in their own comments, which is precisely why
# their per-file scoping was never load-bearing for correctness. The noisy
# with-default flavor (`-Wswitch-enum` / C4061) is deliberately NOT enabled.
#
# ✔BLAST RADIUS MEASURED BEFORE ENABLING, on the whole tree (702 targets,
# src + tests + integrated_tests), GCC 13.3 `-Wall -Wextra`: **ZERO**
# `-Wswitch` occurrences. So this flag costs no source changes today and its
# entire job is to keep it that way. (The same probe found 1039 other
# warnings in classes NOT enabled here; they are reported to the operator
# rather than dumped into this change.)
#
# ⚠ FAILURE DIRECTION IF THIS IS WRONG: it is an ERROR, not a warning, so the
# way it fails is LOUD AND IMMEDIATE — a future toolchain that reports
# `-Wswitch` somewhere we cannot currently compile (an `__APPLE__`-only arm,
# a newer libstdc++ enum) reds the build instead of shipping. That is the
# direction we want: the alternative, a warning nobody reads, is the exact
# mechanism that let an unhandled control-flow enumerator sit unnoticed
# across three legs. If it ever needs relaxing, relax it at THIS line so the
# relaxation is visible in one place — never with a blanket `default:` arm,
# which would hide the unhandled enumerator instead of reporting it.
#
# Placed AFTER the FetchContent block on purpose (same reason as /bigobj
# above): directory-scoped options do not reach googletest / nlohmann_json,
# which were already added by FetchContent_MakeAvailable and must not be
# gated on our warning policy.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Werror=switch)
elseif(MSVC)
    add_compile_options(/we4062)
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

# ── THE COMPILER'S BUILD STAMP (the runtime object cache's compiler identity) ──
#
# `cmake/DssBuildStamp.cmake` computes the value and documents WHY it exists and
# why the three obvious alternatives (hash the compiler image / (path,size,mtime)
# / `DSS_PROJECT_VERSION` alone) are each refused. This block is only the wiring,
# and the wiring carries two decisions worth stating here.
#
# ★ IT REGENERATES AT BUILD TIME, NOT AT CONFIGURE TIME. A configure-time
# `configure_file` would bake the stamp of whenever CMake last ran, so every
# commit made without re-configuring would build a binary claiming an older
# identity — and the cache would then serve the OLD compiler's artifacts to the
# NEW one, silently. `add_custom_target` (always out of date) is what makes the
# value track the tree. The script writes the header ONLY when the value
# changed, so an unchanged stamp costs a `git status` and nothing else.
# ✔MEASURED 2026-08-17 on this workstation, dirty tree (133 entries, a
# 1,440,179-byte `git diff HEAD`): 63 ms + 231 ms ≈ 0.3 s, ONCE per build.
#
# ★★ THE INCLUSION IS SCOPED ON PURPOSE, and this is the part to defend in
# review. The generated header changes on every dirty edit, so every TU that
# `#include`s it recompiles on every dirty edit. Reaching it requires opting in
# through `dss_use_build_stamp()` — a target that does not ask cannot see the
# header at all. The intended includer is the runtime object cache TU and
# nothing else; a widely-included header pulling this in would rebuild the world
# on every keystroke. That is a review-stop, not a preference.
find_package(Git QUIET)   # → GIT_EXECUTABLE, or undefined on a machine with none

set(DSS_BUILD_STAMP_DIR "${CMAKE_BINARY_DIR}/generated")
set(DSS_BUILD_STAMP_HEADER
    "${DSS_BUILD_STAMP_DIR}/program/dss_build_stamp_generated.hpp")

add_custom_target(dss_build_stamp_generate
    COMMAND ${CMAKE_COMMAND}
            "-DDSS_STAMP_VERSION=${DSS_VERSION}"
            "-DDSS_STAMP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DDSS_STAMP_GIT=${GIT_EXECUTABLE}"
            "-DDSS_STAMP_OUTPUT=${DSS_BUILD_STAMP_HEADER}"
            -P "${CMAKE_SOURCE_DIR}/cmake/DssBuildStamp.cmake"
    BYPRODUCTS "${DSS_BUILD_STAMP_HEADER}"
    COMMENT "dss: computing DSS_BUILD_STAMP"
    VERBATIM)

# Carries the include paths only. `${CMAKE_SOURCE_DIR}/src` is here as well as
# the generated dir so the pair is self-sufficient: `program/dss_build_stamp.hpp`
# is the checked-in entry point and it must resolve for anyone who opted in.
add_library(dss_build_stamp INTERFACE)
target_include_directories(dss_build_stamp INTERFACE
    "${DSS_BUILD_STAMP_DIR}"
    "${CMAKE_SOURCE_DIR}/src")

# THE opt-in. One call wires BOTH halves — the include paths and the ordering
# edge — because they are useless apart: paths without the edge is a race
# (the header may not exist yet on a clean build), and the edge without the
# paths does not compile. `add_dependencies` is named EXPLICITLY on the consumer
# rather than left to ride the INTERFACE library, so the ordering holds on every
# generator without depending on how interface-target dependencies propagate.
# The dependency is ORDER-ONLY in Ninja, so an always-out-of-date generator does
# NOT drag its consumers into a rebuild — only a changed header does that, via
# the compiler's own depfile.
function(dss_use_build_stamp target)
    target_link_libraries(${target} PRIVATE dss_build_stamp)
    add_dependencies(${target} dss_build_stamp_generate)
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
    # ★★★ ONE MANIFEST GLOB, READ BY BOTH CORPUS HARNESSES.
    # D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD gave
    # `integrated_tests` per-example ctest entries, which means BOTH harnesses now
    # derive their entry set from the corpus. Hoisted here rather than globbed
    # twice so the two cannot disagree about WHICH examples exist —
    # [[D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE]] is about capability parity, and
    # a harness that silently enumerates a different corpus than its sibling is
    # that divergence in its most basic form.
    # ⚠ `CONFIGURE_DEPENDS` so a NEW example re-runs cmake instead of being
    # silently absent until someone reconfigures by hand.
    file(GLOB_RECURSE DSS_EXAMPLE_MANIFESTS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/examples/*/*/expected.json")
    list(LENGTH DSS_EXAMPLE_MANIFESTS DSS_EXAMPLE_MANIFEST_COUNT)
    # A glob that collapses to nothing would silently retire every corpus entry in
    # BOTH harnesses and leave a green suite asserting nothing about the corpus.
    # Refuse at configure time, where it is one line, instead of at review time.
    if(DSS_EXAMPLE_MANIFEST_COUNT LESS 100)
        message(FATAL_ERROR
            "examples/ yielded only ${DSS_EXAMPLE_MANIFEST_COUNT} manifest(s). "
            "Both corpus harnesses derive their ctest entries from this glob, so "
            "a collapsed result silently empties the corpus suite. Refusing.")
    endif()

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
    # Tracked-blob line-ending guard (2026-08-06 —
    # D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES).
    # No tracked TEXT blob may carry a line-terminating CR unless it
    # DECLARES `eol=crlf`. Wired here, beside the anchor guard,
    # deliberately: `.gitattributes` now pins `eol=lf` for the source
    # tree, but a pin is only as good as its GLOB COVERAGE — the pin
    # that existed before covered configs/goldens/drivers and still
    # let six CRLF blobs land in src/ and tests/ because no `.cpp` was
    # ever named. The pin is the mechanism; THIS is the invariant, and
    # an invariant nothing executes is a convention.
    #
    # Orphan-test-source guard (2026-08-10 —
    # D-TEST-UNIT-TIER-HAS-NO-ORPHAN-SOURCE-GUARD).
    # Every `tests/**/*.cpp` must be registered by a STRUCTURAL
    # `dss_add_test(... SOURCES ...)` call or be on the guard's
    # machine-checked allowlist. An unregistered test source compiles
    # NOWHERE and no ctest entry runs it — absent coverage that looks
    # exactly like coverage from every direction (the file is there, it
    # reads like a test, the diff adding it looked like added coverage,
    # and the suite is green because it never had anything to say about
    # it). ✔MEASURED 2026-08-10: nothing in this repository checked for
    # it, and no CI job asserts an expected ctest entry count.
    #
    # ★ RIDES CTEST rather than being a CI step, which is why it is here
    # and not in a workflow: every leg then runs it for free, on the
    # host it is about, and the invariant is EXECUTED rather than
    # documented — the same reasoning the line-endings comment above
    # states in its last sentence.
    #
    # ★★ IT SELF-TESTS ON EVERY RUN. Given no argument (the form used
    # here) each twin mirrors tests/ into a per-run temp tree, mutates
    # it 12 ways, and asserts it reds with the right exit code and the
    # right message each time — so this entry cannot pass without also
    # proving the guard can fail (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-
    # VACUOUS-PASS, D-CENSUS-INSTRUMENT-UNGUARDED-BY-CTEST). Cost,
    # MEASURED on this Windows workstation: ~18 s (.sh) / ~11 s (.ps1),
    # which ctest -j overlaps with the other ~810 entries and is in
    # family with the anchor guard's own 9.6–14.7 s.
    #
    # Same WIN32 dispatch, same contract, same exit codes as the
    # anchor guard above; both scripts drive identical `git`
    # subcommands so the two shells only marshal arguments.
    if(WIN32)
        find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)
        add_test(
            NAME anchor_registry_guard
            COMMAND ${POWERSHELL_EXE}
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/scripts/check-anchor-registry/check-anchor-registry.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_test(
            NAME line_endings_guard
            COMMAND ${POWERSHELL_EXE}
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/scripts/check-line-endings/check-line-endings.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_test(
            NAME orphan_tests_guard
            COMMAND ${POWERSHELL_EXE}
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/scripts/check-orphan-tests/check-orphan-tests.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    else()
        add_test(
            NAME anchor_registry_guard
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check-anchor-registry/check-anchor-registry.sh"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_test(
            NAME line_endings_guard
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check-line-endings/check-line-endings.sh"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_test(
            NAME orphan_tests_guard
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check-orphan-tests/check-orphan-tests.sh"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()

    # Script-index guard (2026-08-19 — D-SCRIPTS-NO-INDEX-OF-THE-REPOSITORYS-OWN-TOOLING).
    # Every directory under `scripts/` must be documented in BOTH indexes
    # (`scripts/README.md` and the `/dss-cycle` skill's `references/scripts.md`),
    # with a purpose byte-identical to the script's own `PURPOSE:` declaration.
    # ✔MEASURED 2026-08-19: this repository had eighteen scripts and no index of
    # any kind, so a cycle could not be told to prefer an existing tool over
    # writing a new one — nothing listed the existing tools. The operator's rule
    # the guard makes reachable: "if a tool has a problem, fix before using
    # again, not workaround an own tool."
    #
    # ★ NOT DISPATCHED BY PLATFORM, unlike the three guards above: this one is a
    # single Python implementation rather than a `.sh`/`.ps1` twin pair. The
    # twins exist so a shell-only host can run those guards; the trade they make
    # is one contract expressed twice, and this guard's whole subject is a fact
    # expressed in two places drifting apart. Paying that cost inside the guard
    # against it would be the joke it sounds like. Python is already a
    # development dependency here — five of the eight `check-*` scripts are
    # Python, including `check-anchor-balance`, which every cycle must run — so
    # the interpreter is REQUIRED rather than optional: a guard that silently
    # skips when a tool is missing is the vacuous pass this entry exists to
    # refuse (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).
    #
    # ★★ AND IT SELF-TESTS ON EVERY RUN. The no-argument form used here verifies
    # the real tree and then runs 28 arms against a MIRROR — every refusal the
    # guard has, each asserting the MESSAGE and not merely a non-zero exit — so
    # this entry cannot pass without also proving the guard can fail.
    # ⚠ The message assertions are load-bearing: an independent audit found the
    # first draft's floor arm passing with the floor DELETED, because it
    # collapsed on a different refusal that happened to share the exit code.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME scripts_index_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-scripts-index/check-scripts-index.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Closed-vocabulary well-formedness gate (2026-08-21 -
    # D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE).
    # An `EnumNameTable<E, N>` written with N-1 initializers COMPILES: the missing
    # row value-initializes to `{E(0), ""}`, so `fromName("")` starts resolving --
    # and for a vocabulary whose enumerator 0 is a sentinel, an empty string then
    # silently selects "this knob does nothing". The predicate that refuses that
    # shape lives beside the template; this entry refuses a DECLARATION that never
    # invokes it, which is the hazard the predicate alone cannot cover.
    # * It reads the declarations, never a hand-maintained list -- the correction
    #   `check-diagnostic-codes.py` already records: a hand-kept pin can only check
    #   rows somebody remembered to add to it.
    # * It self-tests on every run (7 arms, each asserting the MESSAGE of the
    #   refusal it names), so this entry cannot pass without proving it can fail.
    #   ⚠ THIS SENTENCE WAS FALSE FOR A DAY, AND THE FIX IS IN THE SCRIPT, NOT HERE.
    #   ✔MEASURED 2026-08-22: `main()` ran the self-test only under `--self-test`,
    #   and this entry passes no flag — so the ctest form verified the tree and
    #   proved nothing. `check-shell-portability` found it while being written
    #   against the same convention (D-GATE-ENUM-NAME-TABLE-CTEST-FORM-NEVER-SELF-TESTED).
    add_test(
        NAME enum_name_table_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-enum-name-table-guards/check-enum-name-table-guards.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Shell-portability guard (2026-08-22 —
    # D-SCRIPT-CASE-IN-COMMAND-SUBSTITUTION-BREAKS-BASH-3-2).
    # macOS is a supported host and its `/bin/bash` is 3.2.57 — there is no newer
    # one, and this repository drives its guards through `bash <script>` on every
    # non-Windows leg. Two shapes are fatal there and invisible everywhere else:
    #   · a `case` inside `$( … )` whose patterns are not `(`-prefixed. bash 3.2
    #     scans for the matching `)` instead of parsing, so the pattern's own `)`
    #     ends the substitution and the arm expands to LITERAL TEXT.
    #   · a bash-4+ construct (`mapfile`, `declare -A`, `${v^^}`, …) in a file that
    #     never says it needs bash 4 — six of the twelve leave the exit status at
    #     ZERO, so the script runs on with a wrong answer.
    #
    # ★★ THE COST OF NOT HAVING THIS WAS MEASURED, NOT IMAGINED. On 2026-08-22 the
    # `macos-clang-release` leg failed with `anchor_registry_guard` dying inside its
    # OWN self-test — so the anchor registry had never actually been checked on that
    # host. ⚠ And `bash -n` cannot see the first shape at all (✔the probe file
    # parses clean under 3.2 and fails only when the substitution is EXPANDED), which
    # is why this is a static guard and not a syntax pre-check.
    #
    # ★ RIDES CTEST rather than being a CI step, for the reason the guards above
    # give: every leg then runs it, on the host it is about.
    # ★★ AND IT SELF-TESTS ON EVERY RUN — the no-argument form used here verifies the
    # tree and then drives 34 arms against synthetic fixtures, each red arm asserting
    # the MESSAGE of the refusal it names, so this entry cannot pass without proving
    # the guard can fail (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).
    add_test(
        NAME shell_portability_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-shell-portability/check-shell-portability.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Citation-stability guard (2026-08-19 — D-PLANS-CITATION-LINE-NUMBERS-SILENTLY-INVALIDATED).
    # A `path:line` citation is a claim about a file that nothing rechecks: insert
    # one line above it and it silently points at unrelated prose while still
    # reading as evidence. Operator rule, 2026-08-19: cite a method name, a
    # comment id, or a defined anchor — never a line number.
    #
    # ★ A RATCHET, NOT A BAN, and the numbers are why: 2376 positional citations
    # already exist across 20 documents, overwhelmingly inside CLOSED registry
    # rows describing what was true at some long-past commit. Rewriting those
    # wholesale would be a vast edit that also destroys the record's precision.
    # So the inventory holds per-document ceilings that may only come DOWN — a
    # new citation reds at once, and a converted one reds until its ceiling is
    # lowered in the same commit, because unclaimed headroom is where the next
    # one hides. Same shape as check-no-abort-in-tests' abort() inventory.
    # ⚠ Green here means NO NEW positional citation landed. It never means the
    # plans cite stably; the inventory is DEBT.
    add_test(
        NAME plan_citations_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-plan-citations/check-plan-citations.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Wall-clock-literal guard (2026-08-23 —
    # D-TEST-A-NEW-WALL-CLOCK-LITERAL-IN-A-TEST-IS-UNGUARDED).
    # A duration number written into a test is sized on the machine that wrote it:
    # it passes there and reds on the slowest leg, NAMING THE WRONG EVENT, because
    # a timeout usually surfaces as a sentinel return value the assertion then
    # reports as a wrong result. ✔MEASURED on CI run 32585879580: a two-second
    # deadline that takes 622 ms idle and 1912 ms under 3x contention crossed at
    # 2204 ms on `linux-clang-asan` and read `Which is: -1`.
    #
    # ★ A RATCHET, NOT A BAN, and the numbers are why: ✔MEASURED 2026-08-23, 48
    # literals across 11 files already exist — 26 of them the single idiom
    # `runBinary(exe, std::chrono::milliseconds{5000})`. Per-file ceilings that may
    # only come DOWN, so a NEW literal reds at once and a fixed one reds until its
    # ceiling is lowered in the same commit. Same shape as check-no-abort-in-tests.
    # ⚠ Green here means NO NEW literal landed. It never means the tests are free of
    # them; the inventory is DEBT.
    # ★★ `sleep_for` / `sleep_until` are DELIBERATELY not refused: a sleep is a
    # delay, not a verdict, and a guard that reds on every one-millisecond sleep
    # gets turned off within a week.
    # ★★ AND IT SELF-TESTS ON EVERY RUN — the no-argument form used here verifies
    # the tree and then drives 23 matcher arms against synthetic fixtures, so this
    # entry cannot pass without also proving the guard can fail
    # (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).
    add_test(
        NAME wall_clock_in_tests_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-wall-clock-in-tests/check-wall-clock-in-tests.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Diagnostic-code allocation gate (2026-08-23 —
    # D-GATE-DIAGNOSTIC-CODE-ALLOCATOR-HAS-NO-CTEST-ENTRY).
    # ★ THE ASYMMETRY IS THE POINT: this is the gate written BECAUSE "a lane
    # allocates an ordinal and nothing mechanical notices" — and ✔MEASURED
    # 2026-08-23 it was itself noticed only when a human remembered to run it: no
    # `add_test` entry here and no `.github/workflows/**` step invoking it.
    # ⚠ Registering it was only half the fix. Its `main()` ran `self_test()` ONLY
    # under `--self-test`, and this entry passes no flag — the exact shape that left
    # `enum_name_table_guard` proving nothing. The other half is in the script: the
    # no-argument form now verifies the header AND runs the self-test, honouring
    # both statuses.
    add_test(
        NAME diagnostic_codes_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-diagnostic-codes/check-diagnostic-codes.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ── THE REST OF THE GUARDS NOTHING MECHANICAL RAN ───────────────────────
    # D-GATE-FOUR-GUARD-SCRIPTS-STILL-HAVE-NO-CTEST-ENTRY.
    #
    # ★★★ REGISTERING `diagnostic_codes_guard` above closed ONE instance of
    # "a guard written because nothing mechanical notices, which nothing
    # mechanical runs" and left four. ✔MEASURED 2026-08-23: 14 `scripts/check-*`
    # directories against 9 registered entries, and ✔no `.github/workflows/**`
    # step invoked any of the missing ones either.
    #
    # ⚠⚠ AND THE COST WAS NOT HYPOTHETICAL. Running `check-no-abort-in-tests`
    # by hand at the end of cycle P28 found it **exit 1**: three lanes had each
    # added a `std::abort()` to a test fixture, breaching a ratchet, in a cycle
    # whose whole-tree gate was 1557/1557 green. `abort()` kills the test
    # BINARY, so every sibling in that executable loses its verdict — the
    # failure mode the ratchet exists to prevent, shipped past a green suite
    # because the only thing that could see it was a script nobody ran.
    #
    # ⓘ `check-ninja-deps` stays unregistered and that is deliberate, not the
    # same omission: it takes a build directory and asks whether a specific
    # object's dependency set contains a file, so it has no argument-free form
    # to register. It is a probe, not a tree invariant.
    add_test(
        NAME no_abort_in_tests_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-no-abort-in-tests/check-no-abort-in-tests.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME path_identity_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-path-identity/check-path-identity.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME retyped_closed_sets_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-retyped-closed-sets/check-retyped-closed-sets.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ★ `check-anchor-balance` needs a BASE REF to compare against, so its
    # ordinary form cannot be a ctest entry — a cycle supplies that ref. Its
    # `--self-test` has no such obstacle: 58 cases over the real arithmetic,
    # including the red-on-disable proving the `+bookkept` term is load-bearing.
    # Registering the self-test is what stops the instrument that judges every
    # cycle from being the one thing no cycle checks.
    add_test(
        NAME anchor_balance_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/check-anchor-balance/check-anchor-balance.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# ── The install set (D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE) ──
# Kept whole in its own module rather than interleaved here: it owns what a
# package ships (the CLI, its shared library, and `src/dss-config/`), it forwards
# the installed layout to the resolver that has to find it again at runtime, and
# it registers the scratch-prefix smoke test that proves an installed compiler
# actually compiles. Included LAST because it modifies targets created by
# `add_subdirectory(src)` above.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DssInstall.cmake")
