# ── The closing test for D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE ────────
#
# Install to a scratch prefix and compile — and RUN — a hello-world from the
# INSTALLED tree, with the source tree genuinely out of reach.
#
# ★★★ "OUT OF REACH" IS AN ENFORCED CONDITION HERE, NOT A HOPE, and that is the
# entire difference between this test and one that passes by accident. A compile
# run from anywhere inside the repository resolves config through the cwd ancestor
# walk and goes green while proving nothing whatsoever about the install set.
# THREE independent mechanisms make that impossible, and the third is the one that
# actually settles it:
#
#   (1) the scratch prefix and the scratch working directory live OUTSIDE the
#       repository, under the system temp directory;
#   (2) every ancestor of the working directory — all the way to the filesystem
#       root, which is strictly further than the resolver's 8-hop bound — is
#       ASSERTED to contain no `src/dss-config`, and `DSS_CONFIG_ROOT` is asserted
#       absent from the environment the child inherits;
#   (3) ★★ THE NEGATIVE CONTROL: the identical compile is re-run with the
#       INSTALLED config tree renamed away, and it MUST FAIL. If the green run had
#       been finding config anywhere other than the installed tree, this second
#       run would go green too. (1) and (2) are arguments; (3) is the measurement,
#       and it is the same "prove the mutated bytes reached the process" rule the
#       bar states for red-on-disable, applied to a packaging claim.
#
# Run in CMake script mode by the `install_scratch_prefix_smoke` ctest entry.

foreach(_required
        DSS_BUILD_DIR DSS_BUILD_CONFIG DSS_REPO_ROOT DSS_VERSION DSS_EXE_NAME
        DSS_INSTALL_BINDIR DSS_INSTALL_CONFIGDIR DSS_SMOKE_TARGET_SPEC)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "install smoke: -D ${_required} was not supplied.")
    endif()
endforeach()

# ── A scratch root OUTSIDE the repository ─────────────────────────────────────
# Deliberately NOT under the build directory: the build directory lives inside
# the repo, so a walk up from it reaches `src/dss-config` and the whole test
# would measure the repo instead of the install.
if(DEFINED ENV{DSS_SMOKE_TMPDIR})
    set(_tmp_base "$ENV{DSS_SMOKE_TMPDIR}")
elseif(DEFINED ENV{TMPDIR})
    set(_tmp_base "$ENV{TMPDIR}")
elseif(DEFINED ENV{TMP})
    set(_tmp_base "$ENV{TMP}")
elseif(DEFINED ENV{TEMP})
    set(_tmp_base "$ENV{TEMP}")
elseif(EXISTS "/tmp")
    set(_tmp_base "/tmp")
else()
    message(FATAL_ERROR
        "install smoke: no system temp directory (TMPDIR/TMP/TEMP unset and /tmp "
        "absent). The scratch prefix MUST live outside the repository — placing it "
        "inside would let the cwd walk find the repo's own config and the test "
        "would prove nothing. Set DSS_SMOKE_TMPDIR to a writable directory outside "
        "the repository.")
endif()
file(TO_CMAKE_PATH "${_tmp_base}" _tmp_base)

string(RANDOM LENGTH 10 ALPHABET "abcdefghijklmnopqrstuvwxyz0123456789" _nonce)
set(_scratch "${_tmp_base}/dss-install-smoke-${_nonce}")
set(_prefix  "${_scratch}/prefix")
set(_work    "${_scratch}/work")
set(_out     "${_scratch}/out")
file(REMOVE_RECURSE "${_scratch}")
file(MAKE_DIRECTORY "${_prefix}" "${_work}" "${_out}")

# The scratch root must not have landed inside the repository after all (a
# TMPDIR pointed at the source tree, a symlinked temp). Compare real paths.
get_filename_component(_repo_real "${DSS_REPO_ROOT}" REALPATH)
get_filename_component(_scratch_real "${_scratch}" REALPATH)
string(FIND "${_scratch_real}" "${_repo_real}" _inside)
if(_inside EQUAL 0)
    message(FATAL_ERROR
        "install smoke: the scratch root '${_scratch_real}' is inside the "
        "repository '${_repo_real}'. The cwd walk would reach the repo's own "
        "config and the test would pass without exercising the install set.")
endif()

# ── ENFORCE (2): no ancestor of the working directory carries a config tree ────
# All the way to the filesystem root — further than the resolver's 8-hop bound,
# so this rules out reach rather than merely matching it.
set(_probe "${_scratch_real}")
while(TRUE)
    if(EXISTS "${_probe}/src/dss-config")
        message(FATAL_ERROR
            "install smoke: '${_probe}/src/dss-config' exists, so the cwd walk "
            "could resolve config WITHOUT the install set and this test would be "
            "vacuous. Point DSS_SMOKE_TMPDIR at a directory with no DSS source "
            "tree in its ancestry.")
    endif()
    get_filename_component(_parent "${_probe}" DIRECTORY)
    if(_parent STREQUAL _probe OR _parent STREQUAL "")
        break()
    endif()
    set(_probe "${_parent}")
endwhile()

# ── ENFORCE (2b): the explicit override must not be in play ───────────────────
# `DSS_CONFIG_ROOT` outranks every other arm, so a value inherited from the
# surrounding ctest environment would silently make this a test of the override.
# ctest does not set it for this entry (it is deliberately not a `dss_add_test`),
# but the operator's gate shells export it, so unset it here as well — child
# processes inherit this script's environment.
unset(ENV{DSS_CONFIG_ROOT})
if(DEFINED ENV{DSS_CONFIG_ROOT})
    message(FATAL_ERROR
        "install smoke: DSS_CONFIG_ROOT is still set ('$ENV{DSS_CONFIG_ROOT}') "
        "after unsetting it. It outranks the installed layout, so the test would "
        "measure the override instead of the install set.")
endif()

# ── Install to the scratch prefix ─────────────────────────────────────────────
execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${DSS_BUILD_DIR}"
                             --prefix  "${_prefix}"
                             --config  "${DSS_BUILD_CONFIG}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "install smoke: `cmake --install` failed (rc=${_rc}).\n${_o}\n${_e}")
endif()

set(_installed_exe    "${_prefix}/${DSS_INSTALL_BINDIR}/${DSS_EXE_NAME}")
set(_installed_config "${_prefix}/${DSS_INSTALL_CONFIGDIR}")

if(NOT EXISTS "${_installed_exe}")
    message(FATAL_ERROR "install smoke: no executable at '${_installed_exe}'.")
endif()

# ── The install set must carry the config tree — ALL of it ────────────────────
#
# ★ COMPARED AGAINST THE SOURCE TREE, NEVER AGAINST A LIST WRITTEN HERE. A
# hand-listed set of expected files would be a SECOND OWNER of "what the config
# tree contains" — precisely the defect `cmake/DssInstall.cmake` avoids by
# installing the directory whole — and the next subdirectory somebody adds under
# `src/dss-config/` would be silently absent from every package while this test
# stayed green. (Measured: the first draft of this check DID hand-list files, and
# it named one that exists in a work-in-progress tree but not at this commit.)
#
# The comparison is also what makes the check meaningful at all: `install(DIRECTORY)`
# on a missing source happily creates an EMPTY destination, so "the directory
# exists" would pass over a tree that ships nothing.
file(GLOB_RECURSE _src_config_files
     RELATIVE "${DSS_REPO_ROOT}/src/dss-config"
     "${DSS_REPO_ROOT}/src/dss-config/*")
list(LENGTH _src_config_files _src_config_count)
if(_src_config_count LESS 50)
    message(FATAL_ERROR
        "install smoke: only ${_src_config_count} files found under "
        "'${DSS_REPO_ROOT}/src/dss-config' — the source glob is broken, so the "
        "comparison below would pass vacuously.")
endif()

set(_missing "")
foreach(_rel IN LISTS _src_config_files)
    # The generated object cache is deliberately NOT installed (see the anchored
    # exclusion in DssInstall.cmake), so it is not expected on the other side.
    if(_rel MATCHES "^runtime/platform/dist/")
        continue()
    endif()
    if(NOT EXISTS "${_installed_config}/${_rel}")
        list(APPEND _missing "${_rel}")
    endif()
endforeach()
if(_missing)
    list(LENGTH _missing _missing_count)
    string(REPLACE ";" "\n    " _missing_text "${_missing}")
    message(FATAL_ERROR
        "install smoke: ${_missing_count} of ${_src_config_count} config files "
        "were NOT installed. A packaged compiler with an incomplete config tree "
        "fails in the user's hands, not here.\n"
        "  looked in: ${_installed_config}\n"
        "  missing:\n    ${_missing_text}")
endif()

# The GENERATED object cache must NOT have been installed — see the anchored
# exclusion in DssInstall.cmake. Checked in the other direction too, because an
# exclusion that silently over-reaches is the same defect mirrored.
if(EXISTS "${_installed_config}/runtime/platform/dist")
    message(FATAL_ERROR
        "install smoke: the generated cache 'runtime/platform/dist' was "
        "installed. It is a build artifact, not a shipped input.")
endif()

# ── Compile a hello-world from the installed tree ─────────────────────────────
# `#include <stdio.h>` is the point: it can only resolve through the installed
# `shippedLibs/` descriptors. The source is written into the scratch working
# directory, so even the SOURCE is outside the repository.
file(WRITE "${_work}/hello.c"
"#include <stdio.h>\n"
"\n"
"int main(void) {\n"
"    printf(\"hello from the installed tree\\n\");\n"
"    return 42;\n"
"}\n")

function(dss_smoke_compile out_rc out_text)
    cmake_parse_arguments(A "" "CWD;CONFIG_ROOT" "" ${ARGN})
    if(NOT A_CWD)
        set(A_CWD "${_work}")
    endif()
    # `-E env` sets the variable for the child ONLY, so the controls below cannot
    # leak an override into each other.
    set(_env ${CMAKE_COMMAND} -E env --unset=DSS_CONFIG_ROOT)
    if(A_CONFIG_ROOT)
        set(_env ${CMAKE_COMMAND} -E env "DSS_CONFIG_ROOT=${A_CONFIG_ROOT}")
    endif()
    execute_process(
        COMMAND ${_env} "${_installed_exe}"
                --compile hello.c
                --language c
                --target "${DSS_SMOKE_TARGET_SPEC}"
                --output "${_out}"
        WORKING_DIRECTORY "${A_CWD}"
        RESULT_VARIABLE _crc OUTPUT_VARIABLE _co ERROR_VARIABLE _ce)
    set(${out_rc}   "${_crc}"      PARENT_SCOPE)
    set(${out_text} "${_co}${_ce}" PARENT_SCOPE)
endfunction()

dss_smoke_compile(_compile_rc _compile_text)
if(NOT _compile_rc EQUAL 0)
    message(FATAL_ERROR
        "install smoke: the INSTALLED compiler could not compile a hello-world "
        "(rc=${_compile_rc}). This is the defect the install set exists to close: "
        "a packaged compiler that cannot resolve `#include <stdio.h>`.\n"
        "  exe:    ${_installed_exe}\n"
        "  config: ${_installed_config}\n"
        "  cwd:    ${_work}\n"
        "${_compile_text}")
endif()

if(WIN32)
    set(_artifact "${_out}/hello.exe")
else()
    set(_artifact "${_out}/hello")
endif()
if(NOT EXISTS "${_artifact}")
    file(GLOB_RECURSE _produced "${_out}/*")
    message(FATAL_ERROR
        "install smoke: the compile reported success but produced no "
        "'${_artifact}'. Emitted: ${_produced}")
endif()

# ── …and RUN it ───────────────────────────────────────────────────────────────
# The target spec is the host's own, so the artifact executes here. A binary that
# runs correctly proves the whole installed-config -> compile -> link -> execute
# chain; an exit code alone from the compiler would not.
execute_process(COMMAND "${_artifact}"
                RESULT_VARIABLE _run_rc OUTPUT_VARIABLE _run_out ERROR_VARIABLE _run_err)
if(NOT _run_rc EQUAL 42)
    message(FATAL_ERROR
        "install smoke: the binary built by the installed compiler exited "
        "${_run_rc}, expected 42.\n${_run_out}\n${_run_err}")
endif()
string(FIND "${_run_out}" "hello from the installed tree" _said)
if(_said EQUAL -1)
    message(FATAL_ERROR
        "install smoke: the binary exited 42 but did not print through the "
        "installed <stdio.h>. stdout was: '${_run_out}'")
endif()

# ── ENFORCE (3): the negative control ─────────────────────────────────────────
# Rename the installed config tree away and re-run the IDENTICAL compile. It MUST
# fail. A green here would mean the run above resolved config from somewhere else
# — the repository, an ambient override, a previously installed copy — and the
# whole test would have been measuring that instead.
file(RENAME "${_installed_config}" "${_installed_config}.moved")
dss_smoke_compile(_control_rc _control_text)
file(RENAME "${_installed_config}.moved" "${_installed_config}")

if(_control_rc EQUAL 0)
    message(FATAL_ERROR
        "install smoke: THE TEST IS VACUOUS. With the installed config tree "
        "renamed away, the same compile still SUCCEEDED — so the green run above "
        "was not using the installed tree. Something else in this environment is "
        "supplying config (a repository in the cwd ancestry, DSS_CONFIG_ROOT, or "
        "a second installed copy), and the install set is not what was measured.")
endif()

# The failure must NAME where it looked. A packaged compiler that cannot find its
# config and then reports a confusing downstream error is the defect, not the fix.
string(FIND "${_control_text}" "tried:" _named)
if(_named EQUAL -1)
    message(FATAL_ERROR
        "install smoke: the compile correctly failed without its config tree, but "
        "the diagnostic does not list the paths it tried, so a user cannot tell "
        "what is wrong. Output was:\n${_control_text}")
endif()

# ── ENFORCE (4): the installed tree OUTRANKS a config tree in the cwd ancestry ─
#
# The precedence claim — arm 2 (installed) before arm 3 (cwd walk) — is the
# safety property that stops an unrelated directory from silently redefining a
# packaged compiler. Prove it with a DECOY that the walk WOULD accept: a full
# copy of the config tree, planted in an ancestor of the working directory, with
# a `VERSION` declaring a different release.
#
#   * if the installed arm wins (correct), the decoy is never consulted and the
#     compile SUCCEEDS;
#   * if the walk won, the decoy would be found, its version would disagree with
#     this binary, and the compile would be REFUSED.
#
# An EMPTY decoy would prove nothing — the file probe would miss it and the
# search would continue past it either way. It has to be a tree the walk would
# actually accept, which is why it is a real copy.
set(_decoy      "${_scratch}/decoy")
set(_decoy_work "${_decoy}/nested/work")
set(_decoy_version "0.0.0-decoy-not-this-compiler")
file(MAKE_DIRECTORY "${_decoy}/src" "${_decoy_work}")
file(COPY "${_installed_config}" DESTINATION "${_decoy}/src")
file(WRITE "${_decoy}/VERSION" "${_decoy_version}\n")
file(COPY "${_work}/hello.c" DESTINATION "${_decoy_work}")
if(NOT EXISTS "${_decoy}/src/dss-config/sources/c.lang.json")
    message(FATAL_ERROR
        "install smoke: the decoy tree was not planted, so the precedence "
        "control below would pass vacuously.")
endif()

dss_smoke_compile(_prec_rc _prec_text CWD "${_decoy_work}")
if(NOT _prec_rc EQUAL 0)
    message(FATAL_ERROR
        "install smoke: with a foreign config tree in the working directory's "
        "ancestry, the installed compiler failed (rc=${_prec_rc}). The installed "
        "layout must OUTRANK the cwd walk — otherwise any directory a user "
        "happens to stand in can silently redefine the compiler.\n${_prec_text}")
endif()

# ── ENFORCE (5): version skew fails LOUD, in the real binary ──────────────────
# The same decoy, now named explicitly through the override — the one arm that
# outranks the installed layout. It MUST be refused, and the diagnostic must name
# BOTH versions: a compiler paired with a config tree from another release does
# not fail on its own, it silently compiles something else.
dss_smoke_compile(_skew_rc _skew_text CONFIG_ROOT "${_decoy}")
if(_skew_rc EQUAL 0)
    message(FATAL_ERROR
        "install smoke: DSS_CONFIG_ROOT pointed at a config tree declaring "
        "version ${_decoy_version} and this ${DSS_VERSION} compiler USED IT. "
        "Binary/config skew must fail loud — it is the silent-wrong-answer "
        "class, not a warning.")
endif()
foreach(_must_name "${_decoy_version}" "${DSS_VERSION}" "version skew")
    string(FIND "${_skew_text}" "${_must_name}" _has)
    if(_has EQUAL -1)
        message(FATAL_ERROR
            "install smoke: the skew refusal does not mention '${_must_name}', so "
            "a user cannot tell which half to fix. Output was:\n${_skew_text}")
    endif()
endforeach()

file(REMOVE_RECURSE "${_scratch}")
message(STATUS
    "install smoke: an installed compiler (version ${DSS_VERSION}) compiled and "
    "ran a hello-world for ${DSS_SMOKE_TARGET_SPEC} with the source tree out of "
    "reach. Controls: without its config tree the compile failed and listed the "
    "paths tried; with a foreign tree in the cwd ancestry the installed tree "
    "still won; and that same tree named through DSS_CONFIG_ROOT was refused as "
    "version skew.")
