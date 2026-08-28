# ── The INSTALL SET ────────────────────────────────────────────────────────────
# D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE.
#
# ★★★ WHAT WAS BROKEN, AND IT WAS NOT PACKAGING PLUMBING. Before this file there
# were ZERO `install()` rules in the repository (✔MEASURED on this branch AND on
# `origin/main` at fe031376: `install(` over every CMakeLists.txt/.cmake returns
# nothing on both). Every packaging path therefore shipped a bare executable —
# `pipeline-pkg.yml` tarred `bin` out of the build directory, and every
# `packaging/*.tmpl` installed only the CLI and its shared library. A `dsscp`
# with no `src/dss-config/` cannot resolve a single `#include <stdio.h>`: the
# shipped-header DESCRIPTORS, the target register files, the object formats, the
# optimizer pipelines and the source-language grammars all live in that tree. The
# shipped compiler was not "missing a nice-to-have"; it could not compile.
#
# ⛔ THIS IS THE ONE OWNER OF WHAT SHIPS. The workflows and the `packaging/*.tmpl`
# manifests CONSUME what is installed here — they do not maintain a copy list of
# their own. A YAML copy list would be a second owner of the same fact, free to
# drift from this one, and the drift would surface as a user who cannot compile.
#
# SELF-CONTAINED ON PURPOSE: the top-level `CMakeLists.txt` includes this file in
# one line at its tail, so a rebase reconciles by keeping both sides rather than
# by hand-merging two interleaved diffs.

include(GNUInstallDirs)

# ── The version fact must not go stale ────────────────────────────────────────
# The top-level `file(READ ... VERSION)` is the single source of truth for
# `project(VERSION)`, for `DSS_PROJECT_VERSION` (the `__DSSCP__` predefine) and
# now for the installed config directory's version segment below. CMake does NOT
# re-run configure when a `file(READ)` input changes, so before this line editing
# `VERSION` silently did nothing until somebody happened to reconfigure — the
# binary kept the old identity while the tree declared the new one, which is
# exactly the binary/config skew the resolver refuses at runtime. Declaring the
# dependency turns a silent staleness into an automatic reconfigure.
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
             APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")

# ── Where the config tree lands ───────────────────────────────────────────────
#
# `<datadir>/dsscp/<version>/dss-config/`.
#
# ★ THE `<version>` SEGMENT IS A CORRECTNESS DEVICE, NOT TIDINESS, and it is
# gcc's own answer (`/usr/lib/gcc/<triple>/<version>/`). The compiler probes a
# path composed from ITS OWN version, so a 0.0.3 binary CANNOT reach a 0.0.2
# tree: skew is UNREPRESENTABLE on the installed arm rather than merely
# detectable. Drop the segment and the classic upgrade accident — a user drops a
# newer binary over an older install — silently compiles against the old
# descriptors instead of failing loudly. It also makes side-by-side versions work
# for free.
#
# ★ `datadir` (`share/`) rather than `libdir`, because the tree is genuinely
# architecture-independent: the SAME JSON serves all five legs, and a target's
# register file is no more machine-specific to the HOST than a man page is.
set(DSS_INSTALL_CONFIGDIR
    "${CMAKE_INSTALL_DATADIR}/dsscp/${DSS_VERSION}/dss-config")

# The compiler finds that tree by walking RELATIVE to its own executable, so both
# components must be relative to the prefix or the hop is not computable and the
# package stops being relocatable — which every packaging path here requires
# (Homebrew's cellar, Nix's store, Scoop's app dir, a user's `tar -xf` into
# ~/opt). GNUInstallDirs permits absolute overrides; refuse them by name rather
# than emit a compiler that silently cannot find its config.
foreach(_dss_dir_var CMAKE_INSTALL_BINDIR CMAKE_INSTALL_DATADIR)
    if(IS_ABSOLUTE "${${_dss_dir_var}}")
        message(FATAL_ERROR
            "DSS install: ${_dss_dir_var} is absolute ('${${_dss_dir_var}}'). The "
            "installed compiler locates its config tree by a path RELATIVE to its "
            "own executable, so an absolute ${_dss_dir_var} makes the package "
            "non-relocatable and the config unreachable. Use a prefix-relative "
            "value (the GNUInstallDirs default).")
    endif()
endforeach()

# The hop from the installed executable's directory to the installed config root.
# `CMAKE_INSTALL_PREFIX` cancels out of both sides, which is precisely what makes
# the answer prefix-independent — the string below is the same whether the
# package lands in /usr, /opt/homebrew/Cellar/... or C:\Users\me\scoop\apps\...
file(RELATIVE_PATH DSS_INSTALL_CONFIG_RELDIR
     "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
     "${CMAKE_INSTALL_PREFIX}/${DSS_INSTALL_CONFIGDIR}")

# Forward the computed layout to the resolver
# (`src/core/types/config_path_walk.cpp`), which `#error`s without it — the same
# fail-the-build-loudly wireup `DSS_PROJECT_VERSION` uses one define over. `core`
# is the object library whose .o files carry `config_path_walk.cpp` into
# `dsscp-lib`; a define on the SHARED target would not reach them.
target_compile_definitions(core PRIVATE
    DSS_INSTALL_CONFIG_RELDIR="${DSS_INSTALL_CONFIG_RELDIR}")

# ── RPATH so the installed CLI finds its installed shared library ─────────────
# The build tree keeps its own rpath (BUILD_WITH_INSTALL_RPATH stays OFF), so
# this affects only the installed copy. `$ORIGIN`/`@loader_path` are the
# relocatable spellings — a baked absolute libdir would break every relocating
# package manager. Windows needs none of this: the DLL installs BESIDE the exe
# (RUNTIME destination), and the executable's own directory is first in the
# Windows DLL search order.
if(APPLE)
    set_target_properties(dsscp PROPERTIES
        INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
    set_target_properties(dsscp PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

# ── The artifacts ─────────────────────────────────────────────────────────────
install(TARGETS dsscp dsscp-lib
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"    # the CLI, and the DLL on Windows
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"    # the .so / .dylib
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")   # the Windows import library

# GNU-on-Windows: the same three runtime DLLs the build copies beside the
# binaries must travel with the INSTALLED exe, or it dies at load with
# STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) on any machine whose PATH offers a
# different libstdc++. The list has ONE owner — `src/CMakeLists.txt`, which
# discovers it from this toolchain's own bin directory; this consumes it rather
# than re-deriving it, so the installed set and the build tree cannot disagree
# about which runtime the binaries were built against.
if(DSS_MINGW_RUNTIME_DLLS)
    install(FILES ${DSS_MINGW_RUNTIME_DLLS}
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# ── The config tree — the half whose absence made a packaged compiler useless ──
#
# Installed WHOLE rather than as a hand-listed set of subdirectories, and that is
# deliberate: a list here would be a second owner of "what the config tree
# contains", and the next directory added under `src/dss-config/` would be
# silently omitted from every package. The tree's own layout is the manifest.
# That covers `sources/`, `targets/`, `object-formats/`, `pipelines/`,
# `shippedLibs/` and `runtime/platform/src/` by construction.
#
# ⚠ ONE EXCLUSION, ANCHORED RATHER THAN GLOBBED. `runtime/platform/dist/` is the
# GENERATED, gitignored object cache. It is excluded by an anchored regex, never
# by a bare `dist` pattern — an unanchored `build*` rsync exclude has already
# eaten `src/program/build_scripts.cpp` in this repo, and the identical mistake
# here would silently drop any future file or directory whose name merely
# CONTAINS "dist".
#
# ★★ WHY THE WARM CACHE IS NOT INSTALLED, stated rather than left to look like an
# omission. [[D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF]]'s distribution ruling
# wants a release to ship a WARM `dist/release/` so users never pay the cold
# build. ✔MEASURED at the time of writing: the content-hashed object cache does
# not exist yet (`objectCache|runtimeCache|contentHash|cacheKey` over `src/`
# returns nothing), so nothing in this build produces `dist/` and there is
# nothing to install. An `install(DIRECTORY)` aimed at it would create an empty
# destination and report success — an install rule that silently installs nothing
# is the vacuous-fix shape, and it would read for months as though the warm cache
# were shipping. When the cache lands, add `dist/release/` here AND answer the
# question the install set cannot answer on its own: a cache under `share/` is
# READ-ONLY for an ordinary user, so the miss path needs a writable cache root.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/dss-config/"
        DESTINATION "${DSS_INSTALL_CONFIGDIR}"
        REGEX "/runtime/platform/dist(/|$)" EXCLUDE)

# ── The closing test: does an INSTALLED compiler actually compile? ─────────────
#
# ★★ ANYTHING WEAKER PASSES BY ACCIDENT. A test run from inside the repository
# finds the repository's own config through the cwd walk and goes green while
# proving nothing about the install set. This entry therefore installs to a
# SCRATCH PREFIX and compiles a hello-world with the source tree genuinely out of
# reach — and it PROVES the reach is gone with a negative control instead of
# assuming it (see the script).
#
# Not a `dss_add_test`, and that is load-bearing: `dss_add_test` sets
# `DSS_CONFIG_ROOT` to the repo root, which is the highest-precedence arm and
# would make the installed layout unreachable — the test would then measure the
# override it was supposed to be independent of.
if(DSS_BUILD_TESTS)
    # The host's own (target:format) spelling — the produced binary is EXECUTED,
    # so it must be for the machine running the test. About the HOST this test
    # runs on; it says nothing about which targets the compiler supports.
    if(WIN32)
        set(_dss_smoke_spec "x86_64:pe64-x86_64-windows-exec")
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        if(APPLE)
            set(_dss_smoke_spec "arm64:macho64-arm64-darwin-exec")
        else()
            set(_dss_smoke_spec "arm64:elf64-aarch64-linux-exec")
        endif()
    elseif(APPLE)
        set(_dss_smoke_spec "x86_64:macho64-x86_64-darwin-exec")
    else()
        set(_dss_smoke_spec "x86_64:elf64-x86_64-linux-exec")
    endif()

    add_test(
        NAME install_scratch_prefix_smoke
        COMMAND ${CMAKE_COMMAND}
                -D "DSS_BUILD_DIR=${CMAKE_BINARY_DIR}"
                -D "DSS_BUILD_CONFIG=$<CONFIG>"
                -D "DSS_REPO_ROOT=${CMAKE_SOURCE_DIR}"
                -D "DSS_VERSION=${DSS_VERSION}"
                -D "DSS_EXE_NAME=$<TARGET_FILE_NAME:dsscp>"
                -D "DSS_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
                -D "DSS_INSTALL_CONFIGDIR=${DSS_INSTALL_CONFIGDIR}"
                -D "DSS_SMOKE_TARGET_SPEC=${_dss_smoke_spec}"
                -P "${CMAKE_SOURCE_DIR}/cmake/DssInstallSmokeTest.cmake")
    # Installing the tree + two full compiles; generous enough for a cold CI
    # runner without being unbounded.
    set_tests_properties(install_scratch_prefix_smoke PROPERTIES TIMEOUT 600)
endif()
