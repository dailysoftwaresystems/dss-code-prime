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
# ★ A VERSION BUMP MUST RE-RUN CONFIGURE, AND WITHOUT THIS LINE IT DOES NOT.
# The version is read HERE, at configure time, and baked into the binary as
# DSS_STAMP_VERSION; the shipped config tree is stamped from the same file but
# staged per RUN. So after a bump, an INCREMENTAL build keeps a compiler stamped
# with the old version beside a config tree stamped with the new one, and the
# version-skew check refuses every compile.
# ✔MEASURED 2026-09-16 on the merge that bumped 0.0.2 -> 0.5.0: 1868 of 2207 tests
# failed, every one of them reporting `C_InvalidLanguageName` + `D_SchemaLoadFailed`
# — a diagnostic that reads like a defect in `c.lang.json` and is nothing of the
# kind. The build that produced it reported success.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

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

# OBJECT libraries under src/ (core, program, ...) feed dsscp-lib,
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

# Strict arm verdicts: a cross-arch arm that is SKIPPED must still yield a verdict.
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
# the first PR. The LEDGER was deliberately sequenced
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
# The SAME COFF object-section-count cap the
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
# The closing half of the finding "clang-only warnings are invisible to the MSVC and GCC legs".
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

# ── Precompiled headers (build-time only — the windows-msvc build-time ceiling) ──
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

# ── The host compiler every PCH is built by — a DECLARED dependency since 2026-09-23 ──
# A precompiled header is valid only for the compiler that produced it, and nothing told the build
# so: ✔MEASURED 2026-09-23, Visual Studio updated MSVC IN PLACE (same `14.51.36231` directory,
# `cl` 19.51.36257 → 19.51.36260), so the compiler PATH — all CMake and ninja compare — did not
# move, and every MSVC tree configured before the update failed C1853 on EVERY PCH consumer until
# it was configured afresh. `cmake/DssHostCompilerStamp.cmake` says why and how in full.
# ⇒ A generated header naming the compiler binary's identity, regenerated whenever that binary is
#   newer than its last run (`DEPENDS "${CMAKE_CXX_COMPILER}"`), and APPENDED TO `DSS_PCH_HEADERS`
#   so every PCH includes it: an in-place update re-runs the stamp, its content changes, every PCH
#   and so every consumer rebuilds. Rewritten only when the identity changed, and the rule restats,
#   so an unchanged compiler costs one digest and rebuilds nothing.
# ⓘ Still one STL-only list for the purpose that list states: this header is the build tree's, not
#   the project's, so editing project code never touches it.
# ★ Every target whose PCH lists it depends on `dss_host_compiler_stamp` (wired once, at the end of
#   this file, for every such target in every directory), so it exists before the first PCH
#   compiles. The pin is the ctest entry `build/pch_host_compiler_stamp`.
# ⚠ It is listed as a QUOTED INCLUDE LITERAL, never as a bare path. CMake gives a cmake_pch.hxx it
#   regenerates the modification time of the first BARE-PATH header of the list that exists, and
#   after the first build the stamp exists. ✔MEASURED 2026-09-23 (CMake 4.3.2, Ninja, MinGW g++ and
#   MSVC alike): with the stamp as a bare path, a list that gained `<map>` left cmake_pch.hxx at the
#   stamp's exact time and the PCH was NOT rebuilt; as a literal, it was. Every later change of this
#   list would have left every PCH stale. The walk at the end of this file refuses a bare path in
#   any list that carries the stamp.
set(DSS_HOST_COMPILER_STAMP_HEADER "${CMAKE_BINARY_DIR}/generated/dss_host_compiler_stamp.h")
add_custom_command(
    OUTPUT "${DSS_HOST_COMPILER_STAMP_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            "-DDSS_HCS_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DDSS_HCS_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DDSS_HCS_OUTPUT=${DSS_HOST_COMPILER_STAMP_HEADER}"
            -P "${CMAKE_SOURCE_DIR}/cmake/DssHostCompilerStamp.cmake"
    DEPENDS "${CMAKE_CXX_COMPILER}"
            "${CMAKE_SOURCE_DIR}/cmake/DssHostCompilerStamp.cmake"
    COMMENT "dss: stamping the host compiler every precompiled header is built by"
    VERBATIM)
add_custom_target(dss_host_compiler_stamp DEPENDS "${DSS_HOST_COMPILER_STAMP_HEADER}")
list(APPEND DSS_PCH_HEADERS "\"${DSS_HOST_COMPILER_STAMP_HEADER}\"")

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
    # `integrated_tests` is a test harness (drives the dsscp
    # CLI as a subprocess + asserts OS exit codes from spawned
    # binaries). It uses `enable_testing()` + `add_test()`, so it
    # must live inside the DSS_BUILD_TESTS gate alongside the unit
    # tests — otherwise `add_test()` silently no-ops when the gate
    # is off (no testing context in the calling scope) and CI would
    # build the binary but never invoke it through ctest. This gate
    # IS the fix; no separate anchor is tracked.
    # ★★★ ONE MANIFEST GLOB, READ BY BOTH CORPUS HARNESSES.
    # The fix for the integrated runner walking every example in ONE thread gave
    # `integrated_tests` per-example ctest entries, which means BOTH harnesses now
    # derive their entry set from the corpus. Hoisted here rather than globbed
    # twice so the two cannot disagree about WHICH examples exist —
    # the rule that the two runners must agree is about capability parity, and
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

    # ── DssHarness: the tool that now owns the repository verbs these guards ──
    #    used to spell as hand-written shell scripts
    #
    # ⛔⛔ RESOLVED TO AN ABSOLUTE PATH HERE, AND NEVER SPELLED AS A BARE NAME IN A
    # `COMMAND`. ✔MEASURED on the WSL x86_64 leg, driven in the exact shape this
    # repository drives it (`wsl.exe -e bash <file>`):
    #   * the tool file's CASE follows the RELEASE, and a Linux filesystem is
    #     case-sensitive. The shim is named after the package's `ToolCommandName`,
    #     `DssHarness` through 0.5.5 and `dssharness` from 0.5.6: 2026-09-17 found
    #     `<home>/.dotnet/tools/DssHarness` with no lowercase sibling, and 2026-09-19,
    #     under 0.5.7, `<home>/.dotnet/tools/dssharness` with no capitalised one.
    #     `NAMES` lists both spellings, so either release resolves;
    #   * that directory is on the LOGIN PATH only, so a non-login shell resolves
    #     NEITHER spelling (re-measured 2026-09-19: non-login `which dssharness`
    #     exits 1, and only the login PATH carries `<home>/.dotnet/tools`).
    # ⇒ a bare `COMMAND <name> <verb>` runs on a Windows host — where PATH carries the
    # directory and the filesystem folds case — and fails on the WSL leg twice over:
    # the non-login shell that leg runs misses the directory, and a spelling right for
    # one release is wrong for the other. Either failure alone would have hidden the
    # other. `find_program` answers ONCE, at configure time, with a path ctest invokes
    # directly and that no shell's PATH can steer. The HINTS are the tool installer's
    # own location, so the answer does not depend on which shell class started the
    # configure.
    # ★ A HOST WITHOUT THE TOOL REFUSES, IT DOES NOT VANISH — the rule every guard
    # registered here keeps: a guard that silently does not run publishes a gate
    # figure about the host and not about the tree.
    # ★★ THE SET OF VERDICTS IS OPEN, AND EVERYTHING BUT 0 IS A FAILURE HERE.
    # `rc == 0` is the whole pass condition, which is the one shape a new code cannot
    # fall through as "unknown". Two that already exist or are landing:
    #   21 (Incomplete, shipped 0.5.5) — the work ran, nothing failed, but not every
    #      leg reached a verdict. NOT a pass.
    #   4  (contended) — a build's process-table sample saw work that voids its result.
    #      `build` could previously only pass or fail; a consumer enumerating those two
    #      would read this as neither. NOT a pass.
    # ⛔ DELIBERATELY NOT AN ENUMERATION. Listing acceptable codes is how the NEXT code
    # becomes a silent pass the first time it is emitted — the same shape as the defect
    # 21 was added to fix, one level out.
    # ⚠ Anything added later that READS the JSON ledger instead must read `complete` as
    # well as `passed`; `passed` alone repeats that defect, and an interrupted run (130)
    # also reports `complete: false`.
    # ⓘ ✔MEASURED 2026-09-17 that nothing can build two legs into one directory from
    # here: every entry below is a repository READ, none names a build directory, and
    # `dssharness build` keys its own directory by leg variant.
    find_program(DSS_HARNESS_EXE
        NAMES DssHarness dssharness
        HINTS "$ENV{HOME}/.dotnet/tools" "$ENV{USERPROFILE}/.dotnet/tools"
        DOC "the DssHarness tool that owns this repository's harness verbs")
    if(DSS_HARNESS_EXE)
        message(STATUS "DssHarness resolved for the harness-verb guards: ${DSS_HARNESS_EXE}")
    else()
        message(STATUS "DssHarness NOT FOUND — every harness-verb guard is registered as a REFUSAL")
    endif()

    # `dss_add_harness_guard(<name> <verb> [args...])` — register <name> driving the
    # resolved DssHarness, or, where it was not found, register <name> as a REFUSAL
    # that names the verb nobody ran. One owner for both arms, so the arm that
    # REGISTERS a guard and the arm that REFUSES one cannot come to disagree about
    # which subject was left unchecked.
    function(dss_add_harness_guard _dss_hg_name)
        if(DSS_HARNESS_EXE)
            add_test(
                NAME ${_dss_hg_name}
                COMMAND "${DSS_HARNESS_EXE}" ${ARGN}
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        else()
            string(JOIN " " _dss_hg_cmd ${ARGN})
            set(_dss_hg_script
                "${CMAKE_CURRENT_BINARY_DIR}/${_dss_hg_name}_refusal.cmake")
            file(WRITE "${_dss_hg_script}"
                 "message(FATAL_ERROR \"${_dss_hg_name} CANNOT RUN: no DssHarness was found on this host, so `DssHarness ${_dss_hg_cmd}` never ran and this tree was never checked for what it refuses. Searched for the names DssHarness and dssharness on PATH and under <home>/.dotnet/tools. ⚠ On Linux the installed file's case follows the release (DssHarness through 0.5.5, dssharness from 0.5.6) and its directory is on the LOGIN PATH only, so a working tool can still be invisible to a non-login configure — install it with `dotnet tool install --global DssHarness`, or point -DDSS_HARNESS_EXE at the file. This test FAILS rather than disappearing, because a guard that silently does not run publishes a gate figure that is about the host and not about the tree.\")\n")
            add_test(
                NAME ${_dss_hg_name}
                COMMAND ${CMAKE_COMMAND} -P "${_dss_hg_script}")
            message(STATUS "${_dss_hg_name} registered as a REFUSAL: no DssHarness on this host")
        endif()
    endfunction()

    # ── THE RUN'S SHIPPED-CONFIG SNAPSHOT ────────────────────────────────
    #
    # Registers the `config/snapshot` ctest entry — the `FIXTURES_SETUP` that
    # copies `src/dss-config` ONCE per ctest run — and defines the
    # `dss_use_config_snapshot()` that every config-reading entry in
    # `tests/`, `tests/examples/` and `integrated_tests/` calls. The module's
    # own header carries the whole rationale, including the one constraint
    # that shapes it: the copy is taken at ctest RUN time and never at build
    # time, because a build-time copy would silently green every config-level
    # red-on-disable in this repository.
    #
    # ⚠ MUST PRECEDE the two add_subdirectory() calls below.
    # `dss_use_config_snapshot()` names the fixture entry, and CMake can only
    # set a property on a test that already exists.
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DssConfigSnapshot.cmake")
    dss_config_snapshot_init()

    add_subdirectory(integrated_tests)
    add_subdirectory(tests)

    # ── Three repository guards, ONE Python program each (2026-09-21, lane mig, part 4) ──
    # `anchor_registry_guard`: every `D-*` anchor cited in a scanned root resolves to a registry
    # row, and no markdown table row's unescaped pipes silently drop cells.
    # `line_endings_guard`: no tracked text blob carries a line-terminating CR (HEAD, the index,
    # the working tree), and no CR instrument that cannot see a CR is left for someone to run.
    # `orphan_tests_guard`: every `tests/**/*.cpp` is registered by a STRUCTURAL
    # `dss_add_test(... SOURCES ...)` call or sits on the guard's machine-checked allowlist; an
    # unregistered test source compiles nowhere and reads exactly like coverage.
    # ★ EACH SELF-TESTS ON EVERY RUN: the form used here verifies the tree and drives its arms,
    # every red arm synthesizing its own negative, so the entry cannot pass without proving the
    # guard can fail.
    # ★★ THEY WERE `.sh`/`.ps1` TWINS UNTIL 2026-09-21, dispatched here by `CMAKE_HOST_WIN32`,
    # and the operator's order ended that: no `.sh`/`.ps1` under `.harness-config/runner/actions`.
    # Each pair collapsed into one `.py` carrying the UNION of both twins' checks and arms;
    # `cmake/DssHarnessDeletionInventory.md` records what retired and what covers it. One
    # interpreter on every host means one entry on every host: the host dispatch -- and the
    # `find_program(POWERSHELL_EXE ... REQUIRED)` that once refused to configure a Linux host
    # cross-building for Windows -- left with the twins.
    # ⓘ `line_endings_guard` is the entry that timed out under ctest 3 runs of 3 on 2026-09-21 in
    # its PowerShell form while passing by hand in 3.9 s. The Python guard bounds every wait
    # (90 s, naming what it waited on) and starts every child with no inherited handle.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME anchor_registry_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-anchor-registry/check-anchor-registry.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME line_endings_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-line-endings/check-line-endings.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME orphan_tests_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-orphan-tests/check-orphan-tests.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ── The sqlite harness driver checks its own logic (2026-09-21, lane mig, part 4) ──
    # `harness/sqlite_driver_selftest` runs `build_and_test.py --self-test`: the driver's Step 0
    # ALONE -- its two Python suites, every module's `--self-test`, the leg plan's self-test and
    # lint -- the SAME list and the SAME judgement a real run applies before it starts, so this
    # entry and the driver cannot come to disagree about what is checked.
    # ★ NEW. The shell drivers ran their suites only at their own Step 0, hours away from any
    # gate, and the helpers' self-tests ran nowhere; when those twins collapsed into one Python
    # driver, its checks became cheap enough for every gate run.
    # ★ NOT `repo-guard`, ON PURPOSE -- hence no `guard` suffix for the label scan below to
    # match. What these programs do depends on the HOST: how its processes are enumerated,
    # whether the POSIX half runs in this process or inside WSL. So every leg runs them, rather
    # than only the host the tree happened to be checked on.
    add_test(
        NAME harness/sqlite_driver_selftest
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/real-examples/c/sqlite/build_and_test.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # The speedtest1 benchmark's driver: every check of the shell and PowerShell drivers it
    # replaced, its measurement core's own self-test, and the new arms -- none of which any gate
    # ran before.
    add_test(
        NAME harness/sqlite_benchmark_selftest
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/real-examples/c/sqlite/benchmark_speedtest1.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ── `cmake-import`: its own arms, and the path base proved with a real compiler ──
    # `harness/cmake_import_selftest` runs the program's synthesized arms (the refusals, the
    # scratch rules, the generator ladder through a stand-in cmake, the path base).
    # `harness/cmake_import_any_cwd` is the PIN on that path base (2026-09-21): a manifest the
    # program writes OUTSIDE the project root must build the same files, with THIS build's
    # dsscp, from the root, from a directory of `#error` look-alikes and from an empty one --
    # and the root-relative manifest it wrote until then must fail from the last two. Every
    # path a manifest holds resolves against the manifest's own directory, so a path written
    # relative to the project root named nothing wherever the manifest was not at the root.
    # ⓘ Both NEW: until 2026-09-21 no gate ran this program at all. Not `repo-guard` names,
    # for the same reason as the entry above: the second compiles and links with dsscp.
    add_test(
        NAME harness/cmake_import_selftest
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/cmake-import/cmake-import.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME harness/cmake_import_any_cwd
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/cmake-import/cmake-import.py"
                --prove-any-cwd $<TARGET_FILE:dsscp>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # dsscp resolves its shipped config through `$DSS_CONFIG_ROOT`, pointed at the run's
    # snapshot; the proof's builds start in temporary directories no cwd walk can climb out of.
    dss_use_config_snapshot(harness/cmake_import_any_cwd)

    # ── The anchor CITATION question, asked by the tool ───────────────────────
    # ⛔⛔ THIS IS AN ADDITION, NOT A REPLACEMENT, AND THAT IS A MEASURED DECISION
    # RATHER THAN CAUTION. `DssHarness check-anchor-citations` answers *"does every
    # cited anchor id resolve to a ROW?"* — which is ONE of the things the guard above
    # does, and the one it does WORSE. ✔MEASURED 2026-09-17 on this tree, both sides:
    #   the guard : `OK (1352 src anchors all resolve to plans, 2 retired id(s) uncited)`
    #   the tool  : `OK - every cited anchor resolves: 12835 citation(s) over 2378
    #               file(s) …, against 2041 registry row(s)`
    # The tool is strictly better AT THIS ONE SUBJECT, twice over: it reads the two root
    # FILES the guard's directory-only `_ROOT_SPECS` structurally cannot reach
    # (`README.md`, `.gitattributes`), and it resolves against a registry ROW instead of
    # a substring anywhere under `.plans/` — which closes the hole where an id resolved
    # via its own bug report.
    # ⛔ BUT THE GUARD ALSO OWNS TWO SUBJECTS THE TOOL HAS NO VERB FOR, so retiring it
    # here would be a silent loss, not a retarget: the markdown CELL-WIDTH / unescaped
    # pipe property over all of `.plans/` (✔MEASURED 340 tables, 4264 rows, 42 files —
    # `read-anchors --lint` sees the two registry files only), and the RETIRED-ID
    # matcher. Its per-root collapse floors and its 21-arm self-test have no counterpart
    # either. ⇒ both entries run, the overlap is deliberate, and the guard retires when
    # those subjects have an owner. Recorded in `.temp/lane-m2-findings.md`.
    # ⓘ `--current-tree`, not the default `--current-commit`: a ctest entry judges the
    # tree it was configured from, including work not committed yet. The default reads
    # HEAD, which would pass a working tree citing an id nobody has added.
    dss_add_harness_guard(anchor_citations_guard check-anchor-citations --current-tree)

    # ── Root litter, now asked of the tool ────────────────────────────────────
    # A RETARGET of the entry, not a rename: same name, same subject, same verdict.
    # ✔MEASURED 2026-09-17 with the negative SYNTHESIZED — one loose untracked file
    # planted at this tree's root and removed again — the retired
    # `scripts/check-root-litter/check-root-litter.sh` and `DssHarness
    # check-root-litter` agree on both arms, and the tool also covers the two the
    # script paid for by measurement: ignored litter at depth 1, and an entry whose
    # NAME carries a path from another machine.
    # ★ UNSTEERABLE, AND THE NEGATIVE WAS SYNTHESIZED RATHER THAN ASSUMED.
    # ✔MEASURED with `GIT_DIR` + `GIT_WORK_TREE` + `GIT_INDEX_FILE` exported at a decoy
    # repository holding a loose file: a bare `git status` in that environment listed
    # the decoy's litter, and the tool ignored it and named THIS root. That is the
    # property `leg_tree_git_unsteered` gave the script, and a git hook exporting
    # `GIT_INDEX_FILE` is why it is not hypothetical.
    # ★★ IT LEAVES THE BASH BLOCK ENTIRELY. This entry used to be registered beside
    # `lane_worktree_guard` because it needed a bash proven able to open a `C:/` path —
    # and it was the entry that block once lost SILENTLY when no such bash was found.
    # It now needs no shell at all, so the one host question left is whether the tool is
    # present, which `dss_add_harness_guard` refuses loudly about.
    # ⚠ ONE PROPERTY IS NOT YET PROVEN FOR THE TOOL: the script refused, rather than
    # reporting an empty list, when `git status` itself failed (the measured
    # `index file smaller than expected` false green). The probe for it was
    # inconclusive — a scratch repository with a deliberately corrupt index was
    # refused earlier, for having no harness configuration (exit 11) — so this
    # paragraph records an OPEN question rather than a covered one.
    dss_add_harness_guard(root_litter_guard check-root-litter)

    # Action-index guard (born 2026-08-19 as the script index, when this repository had
    # eighteen scripts and no index of any kind).
    # Every ACTION under `.harness-config/runner/actions/` -- a directory holding its own
    # `<name>.yml`, grouped at any depth -- must be documented in BOTH indexes
    # (`.harness-config/runner/actions/README.md` and the `/dss-cycle` skill's
    # `references/actions.md`), with a purpose byte-identical to the action's own
    # `PURPOSE:` declaration, and must be REACHABLE: a `predefinedRunners` entry in
    # `.harness-config/config.json` names it, and every runner names an action that exists.
    # ⓘ Retargeted 2026-09-18, when `scripts/` and `real-examples/` stopped existing and
    # every program moved into the actions directory; the entry keeps its name because
    # the program keeps its name.
    # ✔MEASURED 2026-08-19: with no index, a cycle could not be told to prefer an
    # existing tool over writing a new one — nothing listed the existing tools. The
    # operator's rule the guard makes reachable: "if a tool has a problem, fix before
    # using again, not workaround an own tool."
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
    # refuse.
    #
    # ★★ AND IT SELF-TESTS ON EVERY RUN. The no-argument form used here verifies
    # the real tree and then runs its arms against a MIRROR — every refusal the
    # guard has, each asserting the MESSAGE and not merely a non-zero exit, counted
    # against the program's own `EXPECTED_ARMS` — so this entry cannot pass without
    # also proving the guard can fail.
    # ⚠ The message assertions are load-bearing: an independent audit found the
    # first draft's floor arm passing with the floor DELETED, because it
    # collapsed on a different refusal that happened to share the exit code.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME scripts_index_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-scripts-index/check-scripts-index.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES — the
    # ANTI-REGRESSION DEVICE, and the reason that row can be closed rather than
    # merely fixed.
    # ★ The defect was never one wrong line: the LSP layer named a BUFFER
    # nowhere while three coordinate spaces were live, so every handler could
    # reach for `tree.source()` and interpret a document position in
    # SYNTHESIZED coordinates. Repairing the five red call sites would have left
    # the next channel open — the registry records this class three times
    # already. `dss::lsp::DocumentCoordinates` is the type that makes the wrong
    # spelling unavailable; THIS is what makes it unreachable.
    # ⚠ A rule only a reader enforces is the hole this class keeps coming back
    # through, which is why it is a ctest entry and not a convention.
    add_test(
        NAME lsp_coordinate_ownership_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-lsp-coordinates/check-lsp-coordinates.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ⓘ `carriage_paths_guard` STOOD HERE AND IS GONE WITH ITS SUBJECT.
    # It refused a carriage script whose hard-coded repository directory name
    # disagreed with `project()` — a real defect (a global rename rewrote
    # thirteen of them at once while ctest reported 1603/1603 PASS). The
    # DssHarness migration deleted every carriage script: the repository path on
    # each host is now ONE value in `.harness-config/config.json`, read by one
    # tool, so there is no second spelling left for a guard to hold to the first.
    # ⚠ The durable property it stood for survives without it, and is worth
    # keeping in words: a path that exists on another host is not checkable from
    # here, so it must be DECLARED ONCE rather than agreed on by inspection.

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
    #   against the same convention.
    add_test(
        NAME enum_name_table_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-enum-name-table-guards/check-enum-name-table-guards.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ⓘ `shell_portability_guard` STOOD HERE AND IS GONE WITH ITS SUBJECT (2026-09-21, lane mig,
    # part 4). It refused bash-3.2-fatal shapes in this repository's `.sh` programs. No `.sh`
    # remains under the actions directory -- `scripts_index_guard` now refuses one by name -- and
    # the one `.sh` left in the tree is an example's product test data, exercised by that
    # example's own run on the POSIX legs.

    # Lane-worktree gate (2026-09-01, P50 -- `remove` discarded an unpreserved scratchpad).
    # `lane-worktree remove` used to delete a lane's ignored evidence without asking, and the
    # preserve step was a convention living in the orchestrator's head. ✔MEASURED the day the gate
    # landed: a `cp … && lane-worktree remove` one-liner lost a lane's whole evidence tree because
    # the destination's parent did not exist and the silence read as success. The gate moved the
    # rule into the tool; this entry is what keeps it there.
    # ★ INTEGRATION ARMS BY NECESSITY: the defect was in what the verb DOES to a real tree, so each
    # arm drives the real program against a real worktree in a fenced temp fixture and asserts the
    # exit code, the MESSAGE, and -- wherever the verb claims a removal -- that the directory is
    # independently gone.
    # ★★ ONE PYTHON PROGRAM SINCE 2026-09-21. It was a `.sh`/`.ps1` pair plus a bash self-test
    # launched through a bash PROBE (a bare `bash` on Windows resolves to the WSL shim, which
    # cannot open a `C:/` path), with a refusal arm for a host no bash passed. The probe, its
    # refusal, `leg_tree_guard` (the shell tree helper's self-test, whose properties now live in
    # `owning_tree_selftest_guard`) and every other consumer of that bash left with the twins; the
    # subject is now driven in-process, with no interpreter to find.
    add_test(
        NAME lane_worktree_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/lane-worktree/lane-worktree.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Citation-stability guard (2026-08-19 — line-number citations were silently invalidated).
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
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-plan-citations/check-plan-citations.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Wall-clock-literal guard (2026-08-23 — a new wall-clock literal in a test was
    # unguarded).
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
    # entry cannot pass without also proving the guard can fail.
    add_test(
        NAME wall_clock_in_tests_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-wall-clock-in-tests/check-wall-clock-in-tests.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Diagnostic-code allocation gate (2026-08-23 — the allocator had no ctest
    # entry).
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
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-diagnostic-codes/check-diagnostic-codes.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ── THE REST OF THE GUARDS NOTHING MECHANICAL RAN ───────────────────────
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
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-no-abort-in-tests/check-no-abort-in-tests.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # ⚠ THESE TWO RAN ZERO SELF-TEST ARMS UNTIL 2026-08-24 (cycle P29,
    # independent step-10 audit), AND THE ENTRIES BELOW ARE UNCHANGED — the
    # repair was in the SCRIPTS, which is the whole lesson. Both carried a
    # self-test behind a flag (`--selftest` / `--self-test`) that nothing passed,
    # so each entry exercised its tree check and proved nothing about its own
    # ability to fail — the identical shape measured live in
    # `no_abort_in_tests_guard`.
    # ⛔ THE OBVIOUS FIX — ADDING THE FLAG HERE — IS WRONG AND MUST NOT BE MADE:
    # ✔MEASURED, in BOTH scripts that flag RETURNS from the self-test without
    # scanning the tree, so it would have TRADED the check for the proof instead
    # of adding the proof to the check. The no-argument form now does both, and
    # a red-on-disable drove the mutant through each registered form to prove the
    # failure actually propagates.
    add_test(
        NAME path_identity_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-path-identity/check-path-identity.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME retyped_closed_sets_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-retyped-closed-sets/check-retyped-closed-sets.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ── `check-doc-census` — registered 2026-09-04 (P59) to close unchecked drift in
    # the docs' status claims; that closing work asked for "the
    # cheapest durable check" for prose that goes false while nobody re-reads it.
    #
    # ⚠⚠ THE INSTRUMENT IT COMPLETES ALREADY EXISTED, WHICH IS THE WHOLE POINT.
    # `.harness-config/runner/actions/examples-census/examples-census.py` was written on 2026-08-24 to
    # stop the corpus figures being hand-counted, and its PURPOSE line says it
    # re-derives "every corpus-manifest figure examples/README.md states". It
    # worked; the numbers rotted anyway. ✔MEASURED 2026-09-04 the first time this
    # guard was pointed at that file: **25 of 28** documented figures were wrong
    # (634 manifests stated against 788), and one had INVERTED rather than
    # drifted ("0 declare it false" against six that do).
    # ⇒ The missing half was never the DERIVATION, it was the COMPARISON. A
    # census somebody must remember to run and then hand-transcribe is a document
    # with extra steps.
    #
    # ★ The claims are declared BY THE DOCUMENT (`<!--census:examples:<key>-->`
    # markers), never listed in the guard — a guard holding its own table of "the
    # figures in the README" would be a third copy of the same fact and would rot
    # exactly like the first two. Adding a checked figure needs no change here.
    add_test(
        NAME doc_census_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-doc-census/check-doc-census.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # ★ And its self-test, for the reason the three entries below carry: a
    # self-test behind a flag nobody types is not a test. Every arm is a
    # REFUSAL plus a CONTROL that must stay green, and each was proven able to
    # fail by deleting the refusal it aims at — including both directions of the
    # `.worktrees/` exclusion, which would otherwise be free to widen until it
    # swallowed the live tree.
    add_test(
        NAME doc_census_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-doc-census/check-doc-census.py"
                --selftest
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ★ `check-anchor-balance` needs a BASE REF to compare against, so its
    # ordinary form cannot be a ctest entry — a cycle supplies that ref. Its
    # `--self-test` has no such obstacle: it runs its whole case set over the
    # real arithmetic — the guard prints the count itself, and no figure is
    # quoted here on purpose — including the red-on-disable proving the
    # `+bookkept` term is load-bearing. ⚠ This comment said "58 cases", which was
    # true at HEAD and made FALSE BY THIS CYCLE'S OWN EDITS to the same guard
    # (✔MEASURED at the fold: 78). A count in a comment is a measurement with no
    # instrument attached — the reader cannot re-derive it and no guard can check
    # it — so it decays silently in the direction of looking authoritative.
    # Registering the self-test is what stops the instrument that judges every
    # cycle from being the one thing no cycle checks.
    add_test(
        NAME anchor_balance_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-anchor-balance/check-anchor-balance.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # `lane-fold.py --self-test` — registered 2026-09-02 (P53) for exactly the
    # reason the entry above exists, and it is the SAME defect class: a self-test
    # behind a flag nobody passes.
    #
    # ⚠ ✔MEASURED at the P53 fold by lane `lw`, which could not fix it because
    # this file is outside a lane's set: `lane_worktree_guard` was registered and
    # its Python TWIN was not, so `lane-fold`'s arms ran only when somebody typed
    # them. That asymmetry is the dangerous half — the two verbs are one
    # workflow, `lane-fold` is the one that WRITES over the main tree, and every
    # arm it carries is a way a fold can destroy work while reporting success
    # (an INHERITED path silently reverting a sibling lane; a DRIFTED destination
    # that must refuse the WHOLE fold; a deletion whose destination moved).
    #
    # ★ The arms are all REFUSALS by design — a fold that copies the right files
    # is right by construction and proves nothing — so this entry cannot pass
    # without the instrument demonstrating it can fail. P53 added arms (i) and
    # (j): driven from ANOTHER repository's cwd the root still names the tree the
    # FILE lives in, and `--repo` still reaches elsewhere deliberately.
    add_test(
        NAME lane_fold_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/lane-fold/lane-fold.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # `anchors.py --self-test` — registered 2026-09-02 (P54), and it is the THIRD
    # self-test behind a flag nobody passes found by
    # the same question: which self-test does nothing unless a human types it?
    #
    # ★★ THIS ONE IS THE HIGHEST-VALUE OMISSION OF THE THREE. `anchors.py` is the
    # ONLY sanctioned door to all three registries — it is what makes a wrapped
    # anchor id, an unescaped pipe and a wrong cell count inexpressible, and it
    # performs the archive MOVE that a closed row depends on. Its 26 arms were the
    # only thing standing behind every one of those invariants, and they ran on
    # request.
    #
    # ⚠ ✔MEASURED 2026-09-02, and it is why this entry is here rather than on a
    # list: 35 pipes across 14 rows had been stored with a DOUBLED backslash, so
    # they rendered a stray `\` where their author wrote `|` — a C `||`, a shell
    # `||`, a regex alternation, `awk -F"|"`. `make_cell`'s own docstring had
    # considered and dismissed that input as one "that never arrives". It arrives
    # from two directions, neither detectable downstream: an author pre-escaping a
    # pipe, and a lane reading the RAW TABLE LINE instead of `read-anchor --json`
    # and handing the escaped form back — which COMPOUNDS, one backslash per
    # re-close. The writer now refuses it (arms 6b–6d), and this entry is what
    # makes that refusal a property of the tree rather than of a habit.
    add_test(
        NAME anchors_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/anchors/anchors.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Which tree does a script's own file live in? (2026-09-15, P66 lane rr.)
    # Eleven Python scripts took their root from the CALLER'S working directory -- a
    # bare `git rev-parse --show-toplevel`, or a cwd-relative path -- and no entry in
    # this file could see it, because every entry pins WORKING_DIRECTORY to the source
    # tree. ✔MEASURED before the fix: run by path from inside another repository they
    # reported on THAT repository, some of them OK. `.harness-config/runner/actions/owning-tree/owning-tree.py`
    # now owns the answer; every consumer's own self-test carries the cwd arms, and this
    # first entry proves those arms can FAIL.
    add_test(
        NAME owning_tree_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/owning-tree/owning-tree.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # Two more tools that read their subject THROUGH git, and handed git the caller's git
    # environment (2026-09-15, P66 lane ge). ✔MEASURED before the fix, under another repository's
    # GIT_DIR or GIT_DIR + GIT_WORK_TREE: corpus-census stamped its report with THAT repository's
    # HEAD and branch (and "clean" for a dirty tree), and refresh_landing_log read THAT
    # repository's history -- 1 commit instead of 111. Both now ask through
    # `owning-tree.run_git`; each entry drives a COPY of the tool in a fixture history, so a
    # reverted call reddens by arm name. Neither needs a network or a built binary.
    add_test(
        NAME corpus_census_git_environment_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/corpus-census/test-corpus-census.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(
        NAME landing_log_git_environment_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/refresh_landing_log/refresh_landing_log.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # ⚠ Two of those consumers had a self-test and NO entry: a self-test behind a flag
    # nobody passes, the shape of the entries above.
    # One of them WRITES registry rows under the root it resolves.
    add_test(
        NAME apply_registry_row_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/apply-registry-row/apply-registry-row.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # ⓘ `carriage_excludes_selftest_guard` stood here. `carriage-excludes.py`
    # derived a transport exclude list from what git ignores, so no carriage
    # re-typed one; DssHarness's `sync` owns that question now, from the same
    # source of truth, for every host at once.

    # Wrapped-anchor-id guard (2026-08-23 — an id wrapped across a line break is
    # invisible to every grep).
    # An anchor id split across a line break — the line ends `…D-SOME-`
    # and the next opens `ANCHOR-PART-OF-THE-ID` — still READS as a citation, but no
    # grep for the whole id will ever return it. ★★★ IT DOES NOT FAIL, IT
    # DISAPPEARS, which is the one failure mode a fail-loud project cannot catch
    # by watching for a failure: the row simply loses an inbound link and starts
    # looking unreferenced.
    #
    # ⚠ NEITHER EXISTING ANCHOR GUARD CAN COUNT ONE, AND THAT IS BY DESIGN.
    # `anchor_registry_guard` RECOVERS a wrapped citation (its `WRAP_JOIN_AWK`
    # joins the fragment to the next line's head token) and then resolves the
    # joined name — recovery, not refusal, because refusing would red hundreds of
    # lines of honest text. `check-anchor-balance` reads WHOLE-LINE ids, so a
    # wrapped one is not there at all. ⇒ the gap is that nothing COUNTS them, and
    # ✔MEASURED 2026-08-23 the population is 290 sites across 145 files.
    #
    # ★ A RATCHET, NOT A BAN, for the reason the two guards above give: per-file
    # ceilings in `inventory.json` that may only come DOWN. ✔AND THE BURN-DOWN
    # COMPLETED IN THIS SAME COMMIT, so the ceiling set is EMPTY and the ratchet
    # now holds a floor of ZERO: any wrapped id landing hereafter is the first,
    # and reds. ⚠ An earlier draft of this block said the un-wrap was "a separate,
    # quiet-tree step … while that happens" — true when typed, false when the
    # commit landed, and left uncorrected it would have told the next reader that
    # a finished burn-down was still in flight. Re-derive the population with the
    # guard itself rather than trusting any number written here.
    # ★★ AND IT SELF-TESTS ON EVERY RUN — the no-argument form used here verifies
    # the tree and then drives EVERY arm its own `EXPECTED_ARMS` pin declares, each
    # red arm asserting the MESSAGE of the refusal it names, so this entry cannot
    # pass without also proving the guard can fail.
    # ⚠ THE ARM COUNT USED TO BE WRITTEN HERE AS A NUMBER AND WENT STALE (it said
    # 35; the guard drives 40 as of 2026-08-24). A count in a comment is a
    # measurement with no instrument attached, so it decays silently in the
    # direction of looking authoritative. The authority is the script's own
    # `EXPECTED_ARMS`, which the guard checks against itself on every run. Its
    # fixtures are
    # SYNTHESIZED into a temp tree at run time and deleted: an on-disk wrapped
    # fixture would be a deliberate wrap that the burn-down step would un-wrap,
    # silently breaking the self-test.
    add_test(
        NAME wrapped_anchor_ids_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-wrapped-anchor-ids/check-wrapped-anchor-ids.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Ninja dependency-record probe, SELF-TEST arm (2026-08-23 — it exited ZERO on
    # a directory that does not exist).
    # ⚠ THE `ⓘ` NOTE IN THE BLOCK ABOVE — `check-ninja-deps` "has no argument-free
    # form to register" — IS TRUE ONLY OF ITS PROBE FORM, AND IS STALE AS WRITTEN;
    # correcting it in place is left to the owner of that block. Its `--self-test`
    # takes no build directory at all — exactly the shape `anchor_balance_selftest_guard`
    # exists for — so the instrument that decides whether a gate may be trusted was
    # itself the one thing no gate checked. ✔MEASURED 2026-08-23: pointed at a
    # directory that does not exist it printed `SKIP` and exited 0, while its own
    # docstring claimed a missing tree is FATAL.
    # ⓘ The PROBE form still cannot be registered: it needs a build directory, and
    # a source checkout has none. Registering the self-test is what stops the
    # verdict rule from drifting unwatched.
    # ~~ ⚠ THAT LAST SENTENCE IS REFUTED BY MEASUREMENT AND IS CORRECTED IN PLACE
    # RATHER THAN LEFT TO BE RE-READ AS TRUE (P62 lane `ax`). "A source checkout
    # has none" is a claim about a SHELL invoked in a checkout; it is not a claim
    # about CMake, which is generating INTO a build directory and therefore knows
    # `${CMAKE_BINARY_DIR}` at configure time. The probe form IS registrable, it is
    # registered immediately below, and the eleven-day gap between writing that
    # sentence and testing it cost cycle P62 a phantom regression — see there. ~~
    add_test(
        NAME ninja_deps_selftest_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-ninja-deps/check-ninja-deps.py"
                --self-test
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ── THE PROBE FORM, ENFORCED: IS THIS BUILD DIRECTORY FRESH ENOUGH TO BE
    #    BELIEVED? (P62 lane `ax` — ninja recorded ZERO header deps under
    #    concurrent builds) ──
    #
    # ★★★ WHAT IT COST TO LEAVE THIS UNREGISTERED, ✔MEASURED IN THE CYCLE THAT
    # ADDED IT. Cycle P62's wave-2 gate came back with two `SemanticAnalyzerC`
    # inline-asm cases red and every other entry green, and the reds were read as a
    # grammar regression a lane had just landed. They were nothing of the kind:
    # `check-ninja-deps.py build/dbg` reported `1 of 625 objects have ZERO recorded
    # header deps`, and the one object was
    # `…/dss_analysis_semantic_test_semantic_analyzer_c.dir/test_semantic_analyzer_c.cpp.obj`
    # — two days stale, never recompiled after a lane inserted a member INTO THE
    # MIDDLE of `SemanticConfig`, then RELINKED against a freshly built
    # `libdsscp.dll` whose layout had moved. The stale object read
    # `semantics().inlineAsm` at an offset one `RuleId` + one `std::string` too low,
    # so `inlineAsm.rule` resolved to `expression` instead of `asmStmt` and the
    # facts gatherer walked the wrong nodes and found nothing. ✔REPRODUCED ON
    # DEMAND by compiling one probe against the pre-change header and linking the
    # post-change DLL: `ia.rule` 25/`asmStmt` became 2/`expression`, and the
    # per-fixture match counts matched the gate's transcript exactly.
    # ⇒ The cost of a build-directory invariant that only runs when someone
    # remembers is not a slow build. It is a gate that reports a defect the source
    # does not contain, in a file the guilty lane never touched.
    #
    # ★★ NOT NAMED `*_guard`, AND THAT IS THE LOAD-BEARING PART OF THE NAME. The
    # label sweep near the end of this file gives every test whose name ends in
    # `guard` the `repo-guard` label, and an indirect leg (WSL, macOS, the arm64
    # VPS) SKIPS that label by operator ruling — correctly, because a repo guard
    # reads the SOURCE TREE and the tree is byte-identical everywhere. This entry
    # reads a BUILD DIRECTORY, and every leg has its own, with its own
    # `.ninja_deps` and its own concurrency history. Inheriting the label would
    # have switched it off on precisely the three legs whose build trees this
    # project has least visibility into.
    #
    # ★ THE `--allow-non-ninja` ESCAPE IS DIRECTIONAL AND CANNOT BE REACHED FROM A
    # NINJA TREE — an escape every caller triggers is an escape that disarms the
    # guard. It is decided HERE, at configure time, from `CMAKE_GENERATOR`, which
    # the build cannot fake; on Ninja (the generator `.harness-config/config.json`
    # declares for every leg) the flag is not passed AT ALL, so the SKIP arm is unreachable and
    # a missing `build.ninja` would be FATAL rather than a pass. On a genuinely
    # non-Ninja tree the entry passes while printing `NOTHING WAS VERIFIED here`,
    # which is a visible skip rather than a silent one.
    #
    # ⓘ `ninja` is put on PATH from `CMAKE_MAKE_PROGRAM` rather than assumed: the
    # tool spawns `ninja -t deps`, and a `ctest` run from a shell that built
    # nothing would otherwise get `FATAL -- ninja is not on PATH` — a refusal that
    # is correct in kind and useless in practice.
    set(_dss_ninja_deps_args "${CMAKE_BINARY_DIR}")
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        list(APPEND _dss_ninja_deps_args --allow-non-ninja)
    endif()
    add_test(
        NAME build/ninja-deps-freshness
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-ninja-deps/check-ninja-deps.py"
                ${_dss_ninja_deps_args}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    get_filename_component(_dss_ninja_dir "${CMAKE_MAKE_PROGRAM}" DIRECTORY)
    if(_dss_ninja_dir)
        set_tests_properties(build/ninja-deps-freshness PROPERTIES
            ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${_dss_ninja_dir}")
    endif()

    # Export-macro placement guard (2026-08-28 —
    # D-BUILD-EXPORT-MACRO-ON-AN-EXPORTED-CLASS-MEMBER-BREAKS-MSVC).
    # `DSS_EXPORT` on a MEMBER of a class that is already `DSS_EXPORT` is MSVC
    # error C2487 and is accepted silently by GCC and Clang.
    #
    # ★★★ THE COMPILER THAT REFUSES IT IS THE ONE NO LOCAL LEG RUNS. The four-leg
    # gate is Windows/MinGW-GCC, WSL x86_64 GCC, qemu arm64 GCC and macOS Clang;
    # MSVC exists only in CI. ✔MEASURED: the declaration on
    # `ObjectFormatData::addSectionRow` landed in cycle P34 and sat green through
    # eight cycles and a 1708/1708 local Windows gate before `windows-msvc-release`
    # failed to BUILD on it. ⇒ A defect class whose only detector is a
    # fourteen-minute remote job is a defect class that LANDS, so the rule is
    # re-expressed as a static check every leg can run.
    #
    # ★★ WHAT IS REFUSED IS THE MEMBER, NOT THE MACRO INSIDE THE BODY, and the
    # boundary is MEASURED against `cl` rather than reasoned: member function,
    # static member function and static member data are C2487; a nested
    # class/struct and a `friend` declaration are ACCEPTED. A guard keyed on "any
    # `DSS_EXPORT` inside an exported class" would refuse nine live sites the
    # compiler is happy with — `PhaseTimers::Scope`, `TreeCursor::Bookmark`,
    # `TokenStream::Bookmark` and six more — and would have been switched off the
    # same day.
    #
    # ★ A BAN, NOT A RATCHET, and the population is why: after the one site above
    # is repaired the live count is ZERO across src/, tests/, integrated_tests/
    # and libs/, and there is no legitimate instance to grandfather. A ratchet
    # with an empty inventory is a ban with extra moving parts.
    # ★★ AND IT SELF-TESTS ON EVERY RUN — the no-argument form used here verifies
    # the tree and then drives every refuse-arm AND every accept-arm, so this
    # entry cannot pass without also proving the guard can fail, nor without
    # proving it does not over-reach onto the nine legitimate nested types.
    add_test(
        NAME export_macro_placement_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-export-macro-placement/check-export-macro-placement.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Guard-output-encoding ratchet (2026-08-23 — nothing ratcheted a new guard into
    # reconfiguring its streams).
    # On Windows a Python child whose stdout is a PIPE — which is how ctest runs
    # every guard above — comes up cp1252/surrogateescape, and `surrogateescape`
    # does nothing for an ordinary unencodable character. So a guard printing tree
    # text with a box-drawing rule or a status glyph dies INSIDE ITS OWN REPORT:
    # the run reds, but the finding is lost and the traceback names a `print`.
    # The scripts that carry the five-line block were given it ONE AT A TIME in
    # cycle P29; nothing made the NEXT one carry it, and "the class is closed by
    # inspection" is the state that let the population accumulate the first time.
    # ⓘ No population count is quoted here ON PURPOSE — the guard prints its own
    # census (`N primary script(s) probed; … SELF, … TRANSITIVE, … UNPROTECTED`)
    # and that is the only figure that cannot rot. ✔An earlier draft of this block
    # said "Twelve … the thirteenth" when the guard reported TEN SELF, which is
    # the same defect this comment now exists to avoid.
    #
    # ★★★ IT TESTS THE PROPERTY, NEVER THE SOURCE TEXT. Each Python primary is
    # IMPORTED in a child whose two streams are forced to cp1252/strict and are
    # pipes; the child then writes U+1F7E0 to both and the parent asserts the
    # UTF-8 bytes arrive. Forcing the encoding is what keeps the arm from being
    # vacuous on the Linux and macOS legs, where a pipe is already UTF-8. Two
    # self-test arms pin the rule from both sides: an EQUIVALENT block written a
    # different way passes, and the literal block present only as a string fails.
    #
    # ★ A RATCHET, NOT A BAN, like the two above: the primaries that reach the
    # property late (inside main()) or never are listed in `inventory.json` as
    # DEBT that may only get shorter — the guard names them on every run. A NEW
    # primary without the block is refused.
    # ★★ AND IT SELF-TESTS ON EVERY RUN — the no-argument form used here verifies
    # the tree and then drives EVERY arm its own `EXPECTED_ARMS` pin declares, each
    # red arm asserting the MESSAGE of the refusal it names, so this entry cannot
    # pass without also proving the guard can fail.
    # ⚠ THE ARM COUNT USED TO BE WRITTEN HERE AS A NUMBER AND WENT STALE (it said
    # 17; the guard drives 21 as of 2026-08-24), and this guard had NO
    # `EXPECTED_ARMS` pin at all until P30 -- it reported its own arm count and
    # compared it to nothing, so arms lost by subtraction left the suite green.
    # The pin is the authority now; do not write the number here again.
    add_test(
        NAME guard_output_encoding_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-guard-output-encoding/check-guard-output-encoding.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Stale-refusal-citation ratchet (2026-08-24 — a claim true when typed and false
    # when the commit landed).
    # An author writes "X therefore stays refused fail-loud (<row id>)" while
    # that row is OPEN; a sibling lane closes the row IN THE SAME COMMIT. The
    # sentence was TRUE WHEN IT WAS TYPED and FALSE WHEN THE COMMIT LANDED, and
    # it still READS as evidence — so the next author is sent to fix an engine
    # gap that no longer exists. The window is MINUTES, not cycles, which is
    # exactly the window a human diff review cannot see.
    #
    # ★★★ AND IT IS REGISTERED BECAUSE THE SWEEP THAT FOUND IT WAS
    # INSTANCE-DRIVEN. The cycle that named the species closed it claiming TWO
    # instances, read out of its own diff. Two independent auditors then found at
    # least EIGHT MORE in the same tree, in files the diff never touched — one of
    # them a config `$comment` declaring a 16-bit sibling absent FOURTEEN LINES
    # ABOVE the 16-bit sibling the same commit added. A sweep finds the instances
    # somebody happened to read; a predicate finds the class.
    #
    # ★★ HOW A STATE PREDICATE CATCHES A TRANSITION WITH NO BASE REF, which is
    # the whole design rather than a side effect: the day a row closes, every
    # sentence anywhere in the tree that still calls that row a live blocker
    # BECOMES a finding, its file rises above its inventory ceiling, and this
    # entry reds — naming the file and quoting the sentence. The file need not
    # have been touched. A self-test arm drives exactly that transition.
    #
    # ★★ THE UNIT IS A SENTENCE, NEVER A LINE WINDOW, and that is measured rather
    # than tidy: a JSON `$comment` in this tree is ONE PHYSICAL LINE holding
    # kilobytes of paragraphs, so a ±N-line window swallows the lot and a single
    # past-tense word anywhere inside it suppresses a real hit. ✔MEASURED
    # 2026-08-24 against a hand-written line-window predicate over the same tree:
    # it missed a live site in `c_inline_asm_memory_arithmetic/expected.json`
    # that this guard reported.
    #
    # ★ A RATCHET, NOT A BAN: sites already exist and the guard prints its own
    # census on every run -- no population count is quoted here, because this
    # number falls as they are repaired and a stale one reads as authority. Per-file ceilings that may only come DOWN, so a NEW site
    # reds at once and a corrected one reds until its ceiling is lowered in the
    # same commit. `--write` burns down and is structurally unable to raise —
    # ✔PROVEN live, it refuses with rc=1 and leaves the inventory byte-identical.
    # ⚠ Green here means NO NEW stale refusal citation landed. It never means the
    # tree's comments are true; the inventory is DEBT, and every entry in it is
    # an unrepaired candidate rather than a sanctioned exception.
    #
    # ★★★ FALSE POSITIVES WERE THE DESIGN CONSTRAINT, because a guard weakened
    # every time it fires asserts nothing. Six narrowings, each measured against
    # the whole tree with the others held fixed (relax all six: 147 candidates;
    # shipped: 9):
    #   * the trigger is a PERSISTENCE word, never a copula — a definitional
    #     "X IS A REFUSAL" is a closure DELIVERING a refusal, not a gap  (9→101);
    #   * a RESIDUAL BOUNDARY exempts the claim: "a frame > 16 MiB stays
    #     fail-loud" is TRUE while the row it cites closed frames UP TO 16 MiB —
    #     the row closed a SUBSET of what the sentence refuses            (9→17);
    #   * `open` counts only predicatively ("stays open"), never attributively
    #     ("still the open block", which is IR jargon)                    (9→10);
    #   * a sentence ends at a closer or a JSON value→key boundary        (9→12);
    #   * a sentence citing an OPEN row too is attributed to that row     (9→12);
    #   * row ids are MASKED before the prose is read — worth 0 today, decisive
    #     in any draft that widens the trigger, and pinned by its own arm.
    # ⓘ `.plans/` is deliberately NOT governed — 40 of the 49 tree-wide
    # candidates live there, 27 in the registry alone, in documents rewritten
    # every cycle; and a registry row contradicting its own verdict is already
    # `check-anchor-balance`'s mismarked-closure arm.
    #
    # ★★ AND IT SELF-TESTS ON EVERY RUN — the no-argument form used here verifies
    # the tree and then drives its arms against fixtures SYNTHESIZED into a temp
    # tree at run time, every red arm asserting the MESSAGE of the refusal it
    # names, so this entry cannot pass without also proving the guard can fail.
    # An on-disk fixture is
    # impossible by construction: it would BE an instance of the defect, would
    # sit in this guard's own inventory forever, and the first person to repair
    # it would silently break the self-test.
    add_test(
        NAME stale_refusal_citations_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-stale-refusal-citations/check-stale-refusal-citations.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # ⓘ `repo_tree_guard` STOOD HERE AND IS GONE WITH ITS SUBJECT (2026-09-21, lane mig, part 4).
    # `repo-tree.ps1`, the PowerShell owner of "which tree am I standing in?", and its bash sibling
    # `leg-tree.sh` became ONE Python owner, `owning-tree.py`, whose self-test
    # (`owning_tree_selftest_guard`) carries every arm both proved -- the identity of a worktree git
    # cannot follow, git asked through that identity, the one-root rule, the owning root of a
    # nested copy -- on every host, with no interpreter probe and no not-applicable arm.
endif()

# ── `repo-guard`: the label that lets an indirect leg skip what it cannot learn ──
# OPERATOR RULING 2026-08-25: *"the root is whichever the host is using you (claude). every
# indirect leg (wsl, vps, macos, whatever), do not need guards!"* A repo guard checks the
# SOURCE TREE, and the tree is byte-identical on every leg, so a second host re-checking it
# learns nothing while paying the most expensive entries in the suite -- ✔MEASURED (P29-era,
# at 18 entries) 159.1 s on Windows, and on macOS the three biggest had not finished after
# 367 s. ⚠ THE COUNT IS NOT 18 ANY MORE AND IS NOT QUOTED HERE ON PURPOSE: the block below
# PRINTS it at configure time (`repo-guard label applied to N test(s)`), which is the figure
# to read, and `ctest -N -L repo-guard` is how to check it. A count typed into a comment is a
# measurement with no instrument attached, and this one had already gone stale by eight
# entries before anyone noticed -- the same rot `.harness-config/runner/actions/check-doc-census/` now refuses for
# every corpus figure `examples/README.md` states.
#
# ⚠ THE SCAN IS OVER TESTS THAT ACTUALLY EXIST, not a hand-kept list. `set_tests_properties`
# on a name some host-conditional branch did not define is a hard CMake error, so a hand-kept list
# would have to track every branch -- the drift shape this repo has been bitten by
# repeatedly. The count is printed so `ctest -N -L repo-guard` can be CHECKED against it
# rather than trusted.
#
# ★ An unlabelled new guard is NOT skipped anywhere -- it simply runs on every leg. The
# failure direction of this mechanism is "too much testing", the only direction a gate may
# fail in.
get_property(_dss_all_tests DIRECTORY PROPERTY TESTS)
set(_dss_guard_tests "")
foreach(_t IN LISTS _dss_all_tests)
    if(_t MATCHES "guard$")
        list(APPEND _dss_guard_tests "${_t}")
    endif()
endforeach()
if(_dss_guard_tests)
    set_tests_properties(${_dss_guard_tests} PROPERTIES LABELS "repo-guard")
    list(LENGTH _dss_guard_tests _dss_guard_count)
    message(STATUS "repo-guard label applied to ${_dss_guard_count} test(s)")
endif()

# ── The install set ──
# Kept whole in its own module rather than interleaved here: it owns what a
# package ships (the CLI, its shared library, and `src/dss-config/`), it forwards
# the installed layout to the resolver that has to find it again at runtime, and
# it registers the scratch-prefix smoke test that proves an installed compiler
# actually compiles. Included LAST because it modifies targets created by
# `add_subdirectory(src)` above.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DssInstall.cmake")

# ── P63: the two proofs this cycle added, registered here rather than beside ──
#         their siblings, and labelled EXPLICITLY.
#
# ⚠ APPENDED AT THE END on purpose: this file is shared by concurrent lanes, and
#   an append is the one edit that cannot collide with a sibling's.
# ⚠ THE LABEL IS SET BY HAND, and that is not a style choice: the `repo-guard`
#   scan above runs BEFORE this point, so a test registered here is invisible to
#   it. An unlabelled guard is not skipped anywhere — it simply runs on every
#   leg — so the failure direction is "too much testing", but the indirect legs
#   would be paying for a tree they cannot learn anything new about.
if(DSS_BUILD_TESTS)
    # ── line_endings_watchdog_guard ───────────────────────────────────────
    # Proves the bound on `check-line-endings`: 90 s, the budget P64 set. A probe that genuinely
    # hangs must be CUT SHORT inside its budget, the refusal must name BOTH the timeout and what it
    # was waiting on, a named CONTROL run must still pass, and a probe blocked on a GRANDCHILD must
    # die with nothing left holding its pipe.
    # ⓘ SEPARATE FROM `line_endings_guard` because the proof necessarily costs a real timeout.
    # ⓘ ONE entry on every host since 2026-09-21: the `.sh`/`.ps1` twins it proved -- and the
    # Windows-only `line_endings_watchdog_sh_guard`, which existed only so the bash twin's bound
    # was proved on SOME leg, with the bash derivation it rode -- collapsed into one program.
    add_test(
        NAME line_endings_watchdog_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-line-endings/check-line-endings.py"
                --selftest-watchdog
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # The label the scan above could no longer apply, applied by name.
    set_tests_properties(line_endings_watchdog_guard PROPERTIES LABELS "repo-guard")
    message(STATUS "repo-guard label applied to 1 additional test (line_endings_watchdog_guard) registered after the scan")
endif()

# ── P66: the package pipeline's release path, and the only thing that can see it ──
#
# ⚠ APPENDED AT THE END, for the same reason the P63 block above says: this file
#   is shared by concurrent lanes, and an append is the one edit that cannot
#   collide with a sibling's. The `repo-guard` scan ran long before this point,
#   so the label is applied BY NAME below.
if(DSS_BUILD_TESTS)
    # `.github/workflows/pipeline-pkg.yml` gained an artifacts-only mode
    # (`workflow_dispatch`) that builds and packages the same four legs as the
    # release path and uploads each as a GitHub Actions artifact, creating and
    # publishing nothing. It was added by THREE `if:` conditions and one
    # side-effect-free step — no new job, no moved step, no changed permission —
    # because the operator ruled on 2026-09-09: *"Ensure the release process will
    # not be changed. The goal is to ensure it will work"*.
    #
    # ★★★ SO THE PROPERTY THE GUARD PINS IS THE RELEASE PATH'S STEP SEQUENCE, as
    # a literal ordered list of every (job, step) a release push runs, plus which
    # of them may carry a mode gate. An added job, a deleted step, a reordering,
    # a step that quietly stops running on a release push — each is a refusal
    # with its own message. Everything else in the guard hangs off that.
    #
    # ★★★ NOTHING ELSE IN THIS REPOSITORY CAN LOOK AT ANY OF IT. The YAML never
    # runs on any host this suite reaches, and CI cannot test its own trigger
    # matrix — a run only ever demonstrates the mode it happened to be in.
    # ✔MEASURED 2026-09-09 with `gh run list --workflow=pipeline-pkg.yml`: that
    # workflow has TWO runs in its whole history, both `startup_failure` at 0 s
    # on 2026-07-07 at `b196c336`, where the file still called a reusable
    # workflow in a PRIVATE repository — so the vendored body has never been
    # executed at all. For this file, reading is the verification, and this entry
    # is what makes a reading stick.
    #
    # ⛔ AND IT REFUSES THE FORK. The cheapest way to add a mode to a workflow is
    # to copy the packaging steps into a second job; the sibling pipeline was already
    # filed as that exact class (a vendored copy whose sync nothing checks).
    # `cmake --install`
    # and `tar -czf` are refused here by COUNT, not by review.
    #
    # ★ Python, not a `.sh`/`.ps1` pair, for the reason `scripts_index_guard`
    # gives: the subject is one contract that must not exist twice, so expressing
    # the guard twice would be the joke it sounds like. STDLIB ONLY — ✔no script
    # here imports PyYAML and no CI step installs it, so a guard that did would
    # red on an interpreter's contents rather than on the tree.
    #
    # ★★ IT SELF-TESTS ON EVERY RUN: one arm per clause against a MIRROR of the
    # real file, each asserting ITS OWN message id rather than a non-zero exit, so
    # this entry cannot pass without also proving every clause can fail. ⚠ THE ARM
    # COUNT IS NOT WRITTEN HERE ON PURPOSE — the guard PRINTS it every run, and a
    # count typed into a comment is a measurement with no instrument attached.
    #
    # ★★ AND A CLAUSE WITH NO ARM IS ITSELF A REFUSAL. The guard reads its own
    # source, collects every message id it can emit, and reds if any of them has
    # no arm asserting it — because a clause whose realistic mutation also trips a
    # neighbouring byte-pin will otherwise sit unproved forever behind the
    # neighbour's green. One clause was already in that state when the ratchet was
    # added. Two further arms exist because an earlier draft refused the CORRECT
    # file over PROSE (a comment naming `gh release upload`, and one naming
    # `cmake --install`), and one more because an unanchored mutation rewrote a
    # comment instead of the key it meant to break — an arm that mutated nothing
    # and would have gone on passing.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME pkg_pipeline_modes_guard
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/check-pkg-pipeline/check-pkg-pipeline.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # The label the scan above could no longer apply, applied by name — same
    # reasoning as the P63 block: an unlabelled guard is not skipped anywhere, it
    # simply runs on every leg, so the failure direction is "too much testing".
    set_tests_properties(pkg_pipeline_modes_guard PROPERTIES LABELS "repo-guard")
    message(STATUS "repo-guard label applied to 1 additional test (pkg_pipeline_modes_guard)")
endif()

# ── The host compiler stamp, wired to every PCH that lists it (see `dss_host_compiler_stamp`) ──
# ⚠ APPENDED AT THE END, for the reason the P63 block states, and because it must run after every
#   directory has defined its targets. One walk over every directory's targets, so a PCH producer
#   added anywhere later is covered without being named here: whatever lists the stamp in its
#   PRECOMPILE_HEADERS depends on the target that makes it. (A REUSE_FROM consumer needs nothing:
#   it already depends on its producer.)
# ⛔ Such a list names every header as an include literal (`<...>` or `"..."`), and a bare path is
#   REFUSED here, the stamp's own included: CMake would give each regenerated cmake_pch.hxx that
#   header's time, and a changed list would then rebuild no PCH (measured, see the stamp's block).
function(_dss_host_compiler_stamp_dependencies dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_pch ${_t} PRECOMPILE_HEADERS)
        if(NOT _pch)
            continue()
        endif()
        if(NOT "\"${DSS_HOST_COMPILER_STAMP_HEADER}\"" IN_LIST _pch
           AND NOT "${DSS_HOST_COMPILER_STAMP_HEADER}" IN_LIST _pch)
            continue()
        endif()
        foreach(_h IN LISTS _pch)
            if(NOT _h MATCHES "^[<\"]")
                message(FATAL_ERROR "The precompiled-header list of target '${_t}' names '${_h}' as a "
                    "bare path. List it as an include literal (<...> or \"...\"): CMake gives a "
                    "regenerated cmake_pch.hxx the time of the first bare-path header that exists, so "
                    "a later change of the list would leave this target's PCH stale.")
            endif()
        endforeach()
        add_dependencies(${_t} dss_host_compiler_stamp)
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_d IN LISTS _subdirs)
        _dss_host_compiler_stamp_dependencies("${_d}")
    endforeach()
endfunction()
_dss_host_compiler_stamp_dependencies("${CMAKE_SOURCE_DIR}")

if(DSS_BUILD_TESTS)
    # The structural pin: (a) the build graph's stamp edge lists the compiler BINARY as an input,
    # (b) every PCH's generated header list includes the stamp -- or, in the PCH-free Debug canary,
    # no PCH exists at all (checked, not assumed) -- and (c) every target building a PCH is ordered
    # after the stamp's target. Registered on every tree, so every leg lists the same ctest names.
    add_test(
        NAME build/pch_host_compiler_stamp
        COMMAND ${CMAKE_COMMAND}
                -DDSS_HCS_MODE=check
                "-DDSS_HCS_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DDSS_HCS_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DDSS_HCS_STAMP=${DSS_HOST_COMPILER_STAMP_HEADER}"
                -DDSS_HCS_TARGET=dss_host_compiler_stamp
                "-DDSS_HCS_GENERATOR=${CMAKE_GENERATOR}"
                "-DDSS_HCS_PCH_DISABLED=${CMAKE_DISABLE_PRECOMPILE_HEADERS}"
                -P "${CMAKE_SOURCE_DIR}/cmake/DssHostCompilerStamp.cmake")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DssTestBudgets.cmake")  # every ctest entry's TIMEOUT — keep this LAST
