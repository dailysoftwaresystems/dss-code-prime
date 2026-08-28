cmake_minimum_required(VERSION 3.28)

# ── TAKE THE RUN'S SHIPPED-CONFIG SNAPSHOT ───────────────────────────────────
#   D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED
#
# Invoked ONCE per `ctest` invocation as the `config/snapshot` entry, which is
# the `FIXTURES_SETUP` of the `DssConfigSnapshot` fixture. Every ctest entry that
# resolves shipped configuration REQUIRES that fixture and reads
# `$DSS_CONFIG_ROOT` = the directory this script produces.
#
#   cmake -DDSS_SNAPSHOT_SOURCE=<repo root> -DDSS_SNAPSHOT_DEST=<dir> -P <this>
#
# ★★★ WHY THIS IS A `cmake -P` SCRIPT AND NOT A BUILD RULE, AND IT IS THE ONE
# DECISION THE WHOLE DESIGN TURNS ON. This project's convention for proving a
# config-level test is not vacuous is: mutate a shipped `.json`, re-run `ctest`
# WITHOUT rebuilding, observe RED. A snapshot taken at BUILD time would serve
# that re-run the config as it stood at the last build — every such mutant would
# go GREEN, and hundreds of pins across this repository would silently stop
# asserting anything. That is strictly worse than no isolation at all. So the
# copy is taken at ctest RUN time, from the tree as it stands the moment the run
# begins: a mutant planted before the run is copied along with everything else
# and reds exactly as it did before this fixture existed.
#
# ⚠ AND IT DELIBERATELY DOES NOT CACHE. "The destination already exists, skip
# the copy" is the same defect wearing a hat — it re-introduces the stale tree by
# a different route. There is no fast path here; the copy is unconditional. The
# guard that keeps it that way is `test_support/config_snapshot`, which compares
# the snapshot against the live tree file by file and reds on any divergence.
#
# ⚠ FAILS LOUD, NEVER FALLS BACK. Every refusal below is a `FATAL_ERROR`, which
# fails the `config/snapshot` entry, which makes ctest refuse to run every entry
# that requires the fixture. A snapshot that could not be made must never leave
# `$DSS_CONFIG_ROOT` pointing at nothing — `findShippedConfig`'s set-but-miss arm
# would then fall THROUGH to the cwd walk, land back on the live tree, and every
# claim this fixture makes would be false while every test still passed.

if(NOT DEFINED DSS_SNAPSHOT_SOURCE OR DSS_SNAPSHOT_SOURCE STREQUAL "")
    message(FATAL_ERROR
        "config snapshot: DSS_SNAPSHOT_SOURCE was not supplied. This script is "
        "driven by the `config/snapshot` ctest entry registered by "
        "dss_config_snapshot_init() in cmake/DssConfigSnapshot.cmake; running it "
        "by hand needs -DDSS_SNAPSHOT_SOURCE=<repo root> -DDSS_SNAPSHOT_DEST=<dir>.")
endif()
if(NOT DEFINED DSS_SNAPSHOT_DEST OR DSS_SNAPSHOT_DEST STREQUAL "")
    message(FATAL_ERROR
        "config snapshot: DSS_SNAPSHOT_DEST was not supplied (see the refusal above).")
endif()

set(_dss_src_config "${DSS_SNAPSHOT_SOURCE}/src/dss-config")
set(_dss_src_version "${DSS_SNAPSHOT_SOURCE}/VERSION")

if(NOT IS_DIRECTORY "${_dss_src_config}")
    message(FATAL_ERROR
        "config snapshot: '${_dss_src_config}' is not a directory, so there is "
        "no shipped configuration to snapshot. Refusing to run the suite against "
        "an unknown config tree.")
endif()

# `VERSION` sits BESIDE `src/`, and both repo-shaped arms of `findShippedConfig`
# read it to refuse a binary/config version skew. A snapshot root without it
# would take the not-found arm of that check and quietly lose the skew refusal,
# so the copy is not optional and its absence is a refusal rather than a warning.
if(NOT EXISTS "${_dss_src_version}")
    message(FATAL_ERROR
        "config snapshot: '${_dss_src_version}' does not exist. The shipped-config "
        "version-skew check reads this file out of the SAME root as src/dss-config, "
        "so a snapshot root without it would silently stop refusing a skewed tree.")
endif()

# ── stage, verify, then swap ─────────────────────────────────────────────────
#
# Built beside the destination and moved into place, rather than written into it,
# so a refusal below cannot leave a HALF tree standing where a later run — or a
# `ctest -FS` that excludes this fixture — would read it as a whole one.
set(_dss_staging "${DSS_SNAPSHOT_DEST}.staging")
file(REMOVE_RECURSE "${_dss_staging}")
file(MAKE_DIRECTORY "${_dss_staging}/src")

# `file(COPY)` is fatal on failure and preserves input timestamps; the timestamp
# half is load-bearing for the staleness check in `test_support/config_snapshot`,
# which compares mtimes between the live tree and the snapshot.
file(COPY "${_dss_src_config}" DESTINATION "${_dss_staging}/src")
file(COPY "${_dss_src_version}" DESTINATION "${_dss_staging}")

# VERIFY THE COPY BEFORE ANYTHING READS IT. The failure this catches is the one
# the fixture exists for: a neighbour rewriting a shipped document IN PLACE
# (`open(path,"wb")` — truncate, then write) while this copy runs produces a
# SHORT file, and a short config surfaces later as a parse error on an unrelated
# assertion in a suite that has nothing to do with it. Named here, it is
# attributable.
#
# ⓘ SCOPE, stated rather than implied: `file(GLOB_RECURSE)` does not report names
# beginning with `.`, so a dotfile inside the config tree is copied but not
# size-checked here. The C++ guard `test_support/config_snapshot` walks both
# trees with `recursive_directory_iterator`, which does see them, and compares
# the two file SETS in both directions.
set(_dss_dst_config "${_dss_staging}/src/dss-config")
file(GLOB_RECURSE _dss_src_files LIST_DIRECTORIES false
     RELATIVE "${_dss_src_config}" "${_dss_src_config}/*")
file(GLOB_RECURSE _dss_dst_files LIST_DIRECTORIES false
     RELATIVE "${_dss_dst_config}" "${_dss_dst_config}/*")

list(LENGTH _dss_src_files _dss_src_count)
list(LENGTH _dss_dst_files _dss_dst_count)
if(_dss_src_count EQUAL 0)
    message(FATAL_ERROR
        "config snapshot: '${_dss_src_config}' enumerated ZERO files. A collapsed "
        "config tree would make every shipped-config lookup in the suite fail for "
        "a reason that has nothing to do with the code under test. Refusing.")
endif()
if(NOT _dss_src_count EQUAL _dss_dst_count)
    message(FATAL_ERROR
        "config snapshot: copied ${_dss_dst_count} file(s) from a tree holding "
        "${_dss_src_count}. The shipped config tree was being REWRITTEN while this "
        "run copied it; this run's verdict would be about that, not about the code "
        "under test.")
endif()

foreach(_dss_rel IN LISTS _dss_src_files)
    file(SIZE "${_dss_src_config}/${_dss_rel}" _dss_src_size)
    file(SIZE "${_dss_dst_config}/${_dss_rel}" _dss_dst_size)
    if(NOT _dss_src_size EQUAL _dss_dst_size)
        message(FATAL_ERROR
            "config snapshot: ${_dss_rel} copied as ${_dss_dst_size} bytes but its "
            "source is ${_dss_src_size}. The shipped config tree was being REWRITTEN "
            "while this run copied it. Nothing in this run is a statement about the "
            "subject under test.")
    endif()
endforeach()

# ── swap ─────────────────────────────────────────────────────────────────────
# Nothing can be reading the destination here: every entry that reads it declares
# `FIXTURES_REQUIRED DssConfigSnapshot`, and ctest does not start such an entry
# until this one has passed.
file(REMOVE_RECURSE "${DSS_SNAPSHOT_DEST}")
file(RENAME "${_dss_staging}" "${DSS_SNAPSHOT_DEST}")

if(NOT IS_DIRECTORY "${DSS_SNAPSHOT_DEST}/src/dss-config")
    message(FATAL_ERROR
        "config snapshot: '${DSS_SNAPSHOT_DEST}/src/dss-config' does not exist after "
        "the swap. Refusing to let the run start with $DSS_CONFIG_ROOT naming a "
        "directory that is not there: a set-but-miss override falls THROUGH to the "
        "cwd walk and back onto the live tree, which is exactly the exposure this "
        "fixture removes.")
endif()

message(STATUS
    "dss: shipped-config snapshot for this ctest run: ${_dss_src_count} file(s) "
    "from ${_dss_src_config} -> ${DSS_SNAPSHOT_DEST}/src/dss-config")
