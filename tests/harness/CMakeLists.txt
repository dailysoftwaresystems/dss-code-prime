# The GATE on the sqlite harness DRIVER (.harness-config/runner/actions/real-examples/c/sqlite/).
#
# The driver takes hours, so ctest does not run it. When it was a shell + PowerShell
# pair run by hand, nothing in ctest had an opinion about it, which is how both
# halves came to key their leg set on the host, against the project's
# build-ANY-target-inside-ANY-host requirement. The leg DECISION now lives in one
# declared catalogue + one host-independent resolver, and the one Python driver
# (2026-09-21) reads it; this entry holds that architecture, and the driver's
# source, to account on every gate run. The driver's own Step 0 runs as
# `harness/sqlite_driver_selftest` (root CMakeLists.txt).
dss_add_test(NAME harness/test_sqlite_harness_legs
             SOURCES test_sqlite_harness_legs.cpp)
# The catalogue and the resolver's plan are JSON, as are the example manifests
# this test cross-checks the launcher vocabulary against — same dependency the
# two corpus runners take for the same reason.
target_link_libraries(dss_harness_test_sqlite_harness_legs
                      PRIVATE nlohmann_json::nlohmann_json)

# THE NINJA DEPENDENCY-RECORD CHECK'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
# `ninja_deps_selftest_guard`. `.harness-config/runner/actions/check-ninja-deps/
# check-ninja-deps.py --self-test` proves the check's rules on text and on throwaway
# trees and prints one verdict line per arm; this binary RUNS it — by the command line
# the top-level CMakeLists declares — and reads each arm's verdict into a case of its
# own. It re-implements none of them. It keeps the entry's name because it IS that
# entry: until 2026-10-10 the program was started bare by ctest, one verdict for every
# arm and no test binary, so no mutation of the program could be an arm of the mutation
# sweep, which starts a test binary whole and judges the exact set of cases that go red.
# ⓘ The name ends in `guard` and the entry carries NO `repo-guard` label: that label is
# given to the top directory's entries. This one proves a program's rules on the host's
# own paths, which every leg has its own of, so it runs on every leg.
dss_add_test(NAME ninja_deps_selftest_guard
             SOURCES test_ninja_deps_selftest.cpp)
# The opt-in (top-level CMakeLists, `dss_use_script_selftest`): the generated header that
# holds the command line, and the program's files declared as inputs of this target's
# object — no compiler reads a Python program, so without that declaration no object
# depended on either.
dss_use_script_selftest(dss_ninja_deps_selftest_guard ninja_deps)

# THE ORPHAN-TEST-SOURCE GUARD, ONE CASE PER VERDICT — the ctest entry `orphan_tests_guard`.
# `.harness-config/runner/actions/check-orphan-tests/check-orphan-tests.py`, run with no
# argument, judges the tests root of the tree it lives in, then proves each of its refusals
# on a mirror of that root, and prints one verdict line for the tree's own census and one
# per arm; this binary RUNS it — by the command line the top-level CMakeLists declares —
# and reads each verdict into a case of its own. It re-implements none of them. It keeps
# the entry's name because it IS that entry: until 2026-10-12 the program was started bare
# by ctest, one verdict for everything it judges, so no mutation of the guard could be an
# arm of the mutation sweep.
dss_add_test(NAME orphan_tests_guard
             SOURCES test_orphan_tests_selftest.cpp)
dss_use_script_selftest(dss_orphan_tests_guard orphan_tests)
# ★ `repo-guard`, SET BY HAND, unlike its neighbour above: the label scan of the top-level
# CMakeLists reads the top directory's entries, and this one is registered here. It keeps
# the label it carried there because what it judges is the SOURCE TREE, the same bytes on
# every leg, so an indirect leg re-reading it learns nothing — the ruling the label exists
# for. The mutation sweep starts the binary itself and asks no label.
set_tests_properties(orphan_tests_guard PROPERTIES LABELS "repo-guard")
message(STATUS "repo-guard label applied to 1 additional test (orphan_tests_guard), registered in tests/harness")

# THE TREE OWNER, THE DOCUMENTED-FIGURE CENSUS AND THE ONE REDACTOR, ONE CASE PER ARM — the
# ctest entries `owning_tree_selftest_guard`, `doc_census_selftest_guard` and
# `harness/redact_selftest`. Each program proves its own rules with its self-test and prints
# one verdict line per arm; each binary RUNS it — by the command line the top-level
# CMakeLists declares — and reads each verdict into a case of its own. None re-implements
# them. Each keeps the entry's name because it IS that entry: until 2026-10-15 each program
# was started bare by ctest, one verdict for everything it proves, so no mutation of it
# could be an arm of the mutation sweep. The owning-tree program also holds the ONE
# statement of what a walk of a tree leaves out — the tree's own `.gitignore`, as git reads
# it — which the census and the redactor walk through, so its arms are theirs too.
dss_add_test(NAME owning_tree_selftest_guard
             SOURCES test_owning_tree_selftest.cpp)
dss_use_script_selftest(dss_owning_tree_selftest_guard owning_tree)
dss_add_test(NAME doc_census_selftest_guard
             SOURCES test_doc_census_selftest.cpp)
dss_use_script_selftest(dss_doc_census_selftest_guard doc_census)
# ⓘ NO `repo-guard` label on either, though both names end in `guard` and both carried the label
# as entries of the top directory until 2026-10-15: each SELF-TESTS a program -- its verdict
# reads the program's own fixtures, which can depend on the host (the reader of `.gitignore` and
# the walks are exactly that: case, separators, path spelling) -- so every leg runs it, the
# indirect ones included. The rule is stated where the label is given (the top-level
# CMakeLists), and `harness/repo_guard_label_check` holds it.
# ⓘ NO label, as before: what the redactor must hide depends on the HOST's own account and
# machine names, so every leg runs it.
dss_add_test(NAME harness/redact_selftest
             SOURCES test_redact_selftest.cpp)
dss_use_script_selftest(dss_harness_redact_selftest redact)

# THE COMPILER BENCH'S AND THE COMPILE PROFILER'S COMMAND LINES, ONE CASE PER ARM — the ctest entries
# `harness/compile_bench_selftest` and `harness/profile_compile_selftest` (NEW 2026-10-15). Each program writes where
# its command line says, and refuses a path to write that begins with `-` — an option taken for a value,
# `--out --help` — by `owning-tree.py`'s `written_path`; its `--self-test` proves that with no compiler, no kit and
# no build, and each binary reads every verdict into a case of its own. ⓘ No label: neither reads a source tree,
# and each costs nothing on any leg.
dss_add_test(NAME harness/compile_bench_selftest
             SOURCES test_compile_bench_selftest.cpp)
dss_use_script_selftest(dss_harness_compile_bench_selftest compile_bench)
dss_add_test(NAME harness/profile_compile_selftest
             SOURCES test_profile_compile_selftest.cpp)
dss_use_script_selftest(dss_harness_profile_compile_selftest profile_compile)

# THE `repo-guard` LABEL'S RULE, ONE CASE PER ARM — the ctest entry `harness/repo_guard_label_selftest`
# (NEW 2026-10-15). `check-repo-guard-label.py --self-test` proves, on synthetic ctest listings and with
# no build, each refusal of the rule stated where the label is given (the top-level CMakeLists): a
# self-test entry never carries `repo-guard`. Its other entry, `harness/repo_guard_label_check`,
# judges THIS build's own listing and is registered beside the label. No label on either.
dss_add_test(NAME harness/repo_guard_label_selftest
             SOURCES test_repo_guard_label_selftest.cpp)
dss_use_script_selftest(dss_harness_repo_guard_label_selftest repo_guard_label)
