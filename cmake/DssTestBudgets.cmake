# ══ cmake/DssTestBudgets.cmake — EVERY ctest entry gets a TIMEOUT ═════════════
#
# D-TEST-INTEGRATED-RUNNER-HANGS-BEFORE-CREATING-ITS-EX-DIRECTORY
#
# ★★★ A HANG MUST FAIL BY NAME. This project never calls `include(CTest)`, so an
# entry without its own TIMEOUT runs under ctest's default of 10 000 000 s:
# locally it spins until a human stops it, and on CI it holds a parallel slot
# until the leg's `--stop-time`. ✔MEASURED this cycle, twice: two
# `integrated_tests` entries spun in the CLI runner's scratch-root prune (one for
# 896 CPU-seconds before it was stopped by pid), and `run_gate_guard` wedged in
# its own fixtures for 1840 s on a local MSVC Release gate. A TIMEOUT turns
# either into a `(Timeout)` verdict under the entry's own name — ✔MEASURED on
# ctest 4.3.2 for an entry that spins and for one blocked on a child that never
# exits, each ended at its limit with no process left behind.
#
# ⚠ A GUARD, NOT A REMEDY, AND NOT A SPEED LIMIT. A `(Timeout)` here is a hang to
# root-cause. The budgets sit far above every healthy run measured, so an entry
# that is merely slow is a finding for its owner — never a reason to shave a
# budget, and never a reason to raise one past the rule below.
#
# ── THE RULE ─────────────────────────────────────────────────────────────────
#   TIMEOUT = HEADROOM x CEILING(tier, class), in whole seconds
#
# CLASS — read from `cmake/DssBuildClass.cmake`, the ONE owner of what kind of
# build this is (tests/CMakeLists.txt reads the same answer for its shuffle arms):
# `sanitized` when a sanitizer reaches the compiler, else `release` for a
# single-config Release, RelWithDebInfo or MinSizeRel, else `debug`. It comes from
# the build's own configuration, never from a host, leg or runner name, and that
# file states the definition and how CI's sanitized leg reaches it.
#
# TIER — what the entry is:
#   corpus     an entry of either corpus harness, named `examples/...` or
#              `integrated_tests/...`: each compiles and runs one small program
#   named      an entry listed in `_DSS_TB_NAMED` with its OWN measured ceilings:
#              every non-corpus entry whose ceiling passed 60 s on some class
#   unit       everything else, including any entry added since the measurement
# A named entry is never budgeted below its class's unit ceiling.
#
# CEILING — the slowest HEALTHY run measured for that tier and class, rounded up.
#   ✔MEASURED 2026-09-15, `Passed` verdicts only, from: six CI pipeline runs of
#   all five test legs (34868310559, 34878923731, 34889196357, 34902431890,
#   34912822910, 34921813148); this workstation's MinGW Debug and MSVC Release
#   gate logs and the WSL x86_64 Debug leg log; and ctest cost-data averages of
#   the local MinGW Debug, MSVC Release, WSL Debug and WSL Release trees and of
#   the arm64 VPS's Debug and Release trees. linux-clang-asan is `sanitized`; the
#   four CI release legs and every local or VPS Release tree are `release`; every
#   Debug tree is `debug`.
#
#                 corpus  unit   set by (corpus; unit)
#   sanitized        289    53   examples/c/deep_comma_chain_lowers_in_order 288.39 s, CI ASan;
#                                asm/test_asm_x86_width_and_direction 52.04 s, CI ASan
#   release           17    50   examples/c/deep_comma_chain_lowers_in_order 16.66 s, CI Windows;
#                                anchor_registry_guard 49.90 s, CI Windows
#   debug             40    35   examples/c/deep_comma_chain_lowers_in_order 39.97 s, VPS Debug;
#                                analysis/compilation_unit/test_import_resolver_shuffled 34.31 s, VPS Debug
#
# HEADROOM = 9 = 2 x 4.29, rounded up. 4.29 is the WIDEST spread measured between
# two healthy runs of one entry on one leg: max/min over those six CI runs, for
# entries with a median of at least 2 s, on windows-msvc-release
# (linux-clang-asan's widest was 3.79; linux-gcc-release 3.16, macos-clang-release
# 2.78, linux-arm64-gcc-release 1.91). A ceiling is already a maximum, so 4.29
# covers a future run as far above it as the slowest measured run sat above the
# fastest; the 2 covers what six runs cannot have seen — a slower runner, a
# heavier co-running mix.
#
# WHAT A HANG THEREFORE COSTS before it is named (sanitized / release / debug):
# a unit entry 477 / 450 / 315 s, a corpus entry 2601 / 153 / 360 s. A named
# entry's budget can exceed a CI leg's whole `--stop-time` (the round-trip pin's
# sanitized budget is 13 824 s against a 110-minute leg); there the leg's own
# budget is the bound that fires first, and the TIMEOUT still ends it on every
# host that has no stop time.
#
# ★ PATHOLOGICALLY SLOW UNDER ASAN, reported and NOT tuned around: the sanitized
# corpus ceiling is set by ONE example, deep_comma_chain_lowers_in_order, 288.39 s
# under ASan against 16.66 s in the slowest release run and 17.12 s in this
# workstation's MinGW Debug gate; and the heap-heavy deep-nesting pins cost
# 1500.47 s (hir) and 903.37 s (mir) under ASan against 82.40 s and 60.23 s there.
#
# ⚠ THIS MODULE OWNS EVERY BUDGET. A registration may still set its own TIMEOUT,
# but never below the rule's budget for the build's class: the walk leaves such a
# number alone, warns at configure time when it is lower, and `ctest/entry-budgets`
# REFUSES it. The two that existed were removed on 2026-09-15:
# `conformance/test_reference_conformance` carried 900 s, 1.48x its measured
# sanitized ceiling of 606.81 s (the rule gives 5463 s), and
# `install_scratch_prefix_smoke` 600 s, a guess against a measured 14.42 s.
#
# ⚠ INCLUDED LAST by the root CMakeLists.txt, and it must stay last: the walk sees
# only the entries registered before it. The pin `ctest/entry-budgets` reds on an
# entry without a TIMEOUT and on one the walk never saw — and it is registered in
# `integrated_tests/CMakeLists.txt`, deliberately NOT here, because a pin inside
# the mechanism it pins disappears with it.
#
# PROBE — `cmake -D DSS_BUDGET_PROBE_TEST=<name> [-D DSS_BUDGET_PROBE_BUILD_TYPE=<t>]
# [-D DSS_BUDGET_PROBE_MULTI_CONFIG=0|1] [-D "DSS_BUDGET_PROBE_FLAGS=<compile flags>"]
# -P cmake/DssTestBudgets.cmake` prints `class=<c> tier=<t> budget=<s>` from the
# same two functions a configure calls; the pin drives it with synthetic
# configurations and with CI's own configure lines, which is how a sanitized
# selection is proved on a host that never builds one.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/DssBuildClass.cmake")

set(_DSS_TB_HEADROOM 9)
set(_DSS_TB_CORPUS_sanitized 289)
set(_DSS_TB_CORPUS_release 17)
set(_DSS_TB_CORPUS_debug 40)
set(_DSS_TB_UNIT_sanitized 53)
set(_DSS_TB_UNIT_release 50)
set(_DSS_TB_UNIT_debug 35)

# "<entry>|<sanitized>|<release>|<debug>" — each the entry's own ceiling in whole
# seconds, the maximum over every measured run of that class (sources above).
set(_DSS_TB_NAMED
    "program/test_emit_hir_round_trips_every_example|1536|218|308"
    "hir/test_frontend_deep_nesting_costs_heap|1501|36|200"
    "mir/test_deep_nesting_costs_heap|904|22|146"
    "conformance/test_reference_conformance|607|75|182"
    "analysis/preprocess/test_preprocessor_shuffled|354|163|489"
    "analysis/semantic/test_semantic_analyzer_c_shuffled|441|140|273"
    "analysis/semantic/test_semantic_analyzer_c|435|60|85"
    "analysis/test_diagnostic_corpus|377|8|66"
    "hir/test_hir_lowering_c|359|29|59"
    "analysis/syntactic/test_parser_speculation_ceilings|357|5|55"
    "plan_citations_guard|304|345|107"
    "analysis/semantic/test_fc3_width_semantics_shuffled|186|97|334"
    "run_gate_guard|102|288|88"
    "core/test_deep_type_layout_costs_heap|280|8|37"
    "mir/test_mir_lowering_c|251|26|49"
    "program/test_runtime_cache_wiring|244|48|57"
    "analysis/preprocess/test_preprocessor|214|11|38"
    "core/test_key_shape_and_text_tier_vocabulary|190|10|36"
    "core/test_config_closed_key_vocabulary|177|7|33"
    "program/test_compile_pipeline|145|23|32"
    "program/test_ffi_resolve_library|141|25|32"
    "core/test_grammar_loader_chain_vocabulary_projection|134|5|24"
    "program/test_static_link|129|17|27"
    "program/test_dependency_resolver|119|12|24"
    "analysis/semantic/test_type_identity_vocabulary|112|8|19"
    "link/test_descriptor_library_role_agreement|105|14|20"
    "lane_worktree_guard|37|104|33"
    "core/test_type_kind_vocabulary_projection|103|4|17"
    "analysis/semantic/test_fc3_width_semantics|96|6|19"
    "analysis/preprocess/test_preprocess_no_rework|94|5|18"
    "link/test_coff_object_reader|18|89|23"
    "program/test_project_config|84|11|15"
    "orphan_tests_guard|7|76|15"
    "lir/test_mir_to_lir|76|8|16"
    "harness/test_sqlite_harness_legs|66|73|67"
    "link/test_pe_object_data_import_slot|14|71|22"
    "core/test_config_enum_vocabulary_projection|71|3|14"
    "core/test_target_schema|62|3|15"
)

# The tier and the budget of the entry `test_name` in a build of class `class`.
function(dss_test_budget_for out_seconds out_tier class test_name)
    if(test_name MATCHES "^(examples|integrated_tests)/")
        set(_tier corpus)
        set(_ceiling ${_DSS_TB_CORPUS_${class}})
    else()
        set(_tier unit)
        set(_ceiling ${_DSS_TB_UNIT_${class}})
        if(class STREQUAL "sanitized")
            set(_column 1)
        elseif(class STREQUAL "release")
            set(_column 2)
        else()
            set(_column 3)
        endif()
        foreach(_row IN LISTS _DSS_TB_NAMED)
            string(REPLACE "|" ";" _cells "${_row}")
            list(GET _cells 0 _name)
            if(_name STREQUAL test_name)
                set(_tier named)
                list(GET _cells ${_column} _own)
                if(_own GREATER _ceiling)
                    set(_ceiling ${_own})
                endif()
                break()
            endif()
        endforeach()
    endif()
    math(EXPR _seconds "${_DSS_TB_HEADROOM} * ${_ceiling}")
    set(${out_seconds} ${_seconds} PARENT_SCOPE)
    set(${out_tier} ${_tier} PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT DEFINED DSS_BUDGET_PROBE_TEST)
        message(FATAL_ERROR "DssTestBudgets probe: -D DSS_BUDGET_PROBE_TEST=<entry name> is required")
    endif()
    if(NOT DEFINED DSS_BUDGET_PROBE_MULTI_CONFIG)
        set(DSS_BUDGET_PROBE_MULTI_CONFIG 0)
    endif()
    dss_build_class_of(_probe_class "${DSS_BUDGET_PROBE_BUILD_TYPE}"
                       "${DSS_BUDGET_PROBE_MULTI_CONFIG}" "${DSS_BUDGET_PROBE_FLAGS}")
    dss_test_budget_for(_probe_seconds _probe_tier "${_probe_class}" "${DSS_BUDGET_PROBE_TEST}")
    message("class=${_probe_class} tier=${_probe_tier} budget=${_probe_seconds}")
    return()
endif()

# ── the walk: every directory, every entry, once ─────────────────────────────
function(_dss_test_budgets_apply)
    dss_build_class(_class READER "cmake/DssTestBudgets.cmake")
    get_property(_readers GLOBAL PROPERTY DSS_BUILD_CLASS_READERS)
    get_property(_build_type GLOBAL PROPERTY DSS_BUILD_CLASS_BUILD_TYPE)
    get_property(_multi GLOBAL PROPERTY DSS_BUILD_CLASS_MULTI_CONFIG)
    get_property(_compile_flags GLOBAL PROPERTY DSS_BUILD_CLASS_COMPILE_FLAGS)
    get_property(_dir_options GLOBAL PROPERTY DSS_BUILD_CLASS_DIRECTORY_OPTIONS)
    get_property(_shuffle_known GLOBAL PROPERTY DSS_SHUFFLE_SANITIZED SET)
    if(_shuffle_known)
        get_property(_shuffle GLOBAL PROPERTY DSS_SHUFFLE_SANITIZED)
    else()
        set(_shuffle UNSET)
    endif()

    set(_entries "")
    set(_walked "")
    set(_seen_named "")
    set(_below "")
    set(_count_corpus 0)
    set(_count_named 0)
    set(_count_unit 0)
    set(_count_explicit 0)
    # A work list, not recursion: the directory tree is walked with a queue.
    set(_pending "${CMAKE_SOURCE_DIR}")
    list(LENGTH _pending _remaining)
    while(_remaining GREATER 0)
        list(POP_FRONT _pending _dir)
        string(APPEND _walked "walked\t${_dir}\n")
        get_property(_tests DIRECTORY "${_dir}" PROPERTY TESTS)
        foreach(_test IN LISTS _tests)
            dss_test_budget_for(_seconds _tier "${_class}" "${_test}")
            if(_tier STREQUAL "named")
                list(APPEND _seen_named "${_test}")
            endif()
            get_test_property("${_test}" TIMEOUT DIRECTORY "${_dir}" _existing)
            if(_existing STREQUAL "NOTFOUND")
                set_tests_properties("${_test}" DIRECTORY "${_dir}" PROPERTIES TIMEOUT "${_seconds}")
                string(APPEND _entries "entry\t${_test}\t${_tier}\t${_seconds}\tassigned\t-\n")
                math(EXPR _count_${_tier} "${_count_${_tier}} + 1")
            else()
                string(APPEND _entries "entry\t${_test}\t${_tier}\t${_seconds}\texplicit\t${_existing}\n")
                math(EXPR _count_explicit "${_count_explicit} + 1")
                if(_existing LESS _seconds)
                    list(APPEND _below "${_test} sets ${_existing} s where the rule gives ${_seconds} s")
                endif()
            endif()
        endforeach()
        get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND _pending ${_subdirs})
        list(LENGTH _pending _remaining)
    endwhile()

    if(_class STREQUAL "sanitized")
        set(_column 1)
    elseif(_class STREQUAL "release")
        set(_column 2)
    else()
        set(_column 3)
    endif()
    set(_named_lines "")
    set(_stale "")
    foreach(_row IN LISTS _DSS_TB_NAMED)
        string(REPLACE "|" ";" _cells "${_row}")
        list(GET _cells 0 _name)
        list(GET _cells ${_column} _own)
        list(FIND _seen_named "${_name}" _at)
        if(_at EQUAL -1)
            set(_matched 0)
            list(APPEND _stale "${_name}")
        else()
            set(_matched 1)
        endif()
        string(APPEND _named_lines "named\t${_name}\t${_own}\t${_matched}\n")
    endforeach()

    set(_reader_lines "")
    foreach(_reader IN LISTS _readers)
        string(REGEX MATCH "^(.*)=([a-z]+)$" _parts "${_reader}")
        string(APPEND _reader_lines "class.reader\t${CMAKE_MATCH_1}\t${CMAKE_MATCH_2}\n")
    endforeach()

    string(REPLACE "\t" " " _flags_line "${_compile_flags}")
    string(REPLACE "\t" " " _options_line "${_dir_options}")
    set(_report "format\tdss-ctest-entry-budgets-2\n")
    string(APPEND _report "class\t${_class}\n")
    string(APPEND _report "evidence.build_type\t${_build_type}\n")
    string(APPEND _report "evidence.multi_config\t${_multi}\n")
    string(APPEND _report "evidence.compile_flags\t${_flags_line}\n")
    string(APPEND _report "evidence.directory_compile_options\t${_options_line}\n")
    string(APPEND _report "consumer.shuffle_arm_sanitized\t${_shuffle}\n")
    string(APPEND _report "headroom\t${_DSS_TB_HEADROOM}\n")
    string(APPEND _report "ceiling.corpus\t${_DSS_TB_CORPUS_${_class}}\n")
    string(APPEND _report "ceiling.unit\t${_DSS_TB_UNIT_${_class}}\n")
    string(APPEND _report "${_reader_lines}${_walked}${_named_lines}${_entries}")
    file(WRITE "${CMAKE_BINARY_DIR}/ctest-entry-budgets.txt" "${_report}")

    math(EXPR _assigned "${_count_corpus} + ${_count_named} + ${_count_unit}")
    message(STATUS "ctest entry budgets: class ${_class} (build type '${_build_type}'), "
                   "headroom ${_DSS_TB_HEADROOM}: TIMEOUT assigned to ${_assigned} entries "
                   "(corpus ${_count_corpus}, named ${_count_named}, unit ${_count_unit}), "
                   "${_count_explicit} with their own; report ctest-entry-budgets.txt")
    if(_below)
        list(LENGTH _below _below_count)
        message(WARNING "ctest entry budgets: ${_below_count} entry(ies) set their own TIMEOUT "
                        "BELOW the rule (the pin `ctest/entry-budgets` refuses this): ${_below}")
    endif()
    if(_stale)
        list(LENGTH _stale _stale_count)
        message(WARNING "ctest entry budgets: ${_stale_count} measured row(s) name no registered "
                        "entry (the pin `ctest/entry-budgets` refuses this): ${_stale}")
    endif()
endfunction()

_dss_test_budgets_apply()
