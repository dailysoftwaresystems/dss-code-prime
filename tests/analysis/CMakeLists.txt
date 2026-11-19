# Cross-cutting diagnostic-corpus harness (V2-4 Part B): parses +
# semantically analyzes every malformed source under
# `tests/corpus/diagnostics/<lang>/` and pins each emitted diagnostic's
# code + line:col against a sibling `.diag` golden (golden_file.hpp).
# Lives here (not under syntactic/ or semantic/) because it spans both
# the parser (`P_*`) and semantic (`S_*`) tiers.
dss_add_test(NAME analysis/test_diagnostic_corpus
             SOURCES test_diagnostic_corpus.cpp)

# ★★ P69 (lane `cs`, fold f26e): EVERY RECOVERY TREE THE REPOSITORY HOLDS IS ANALYZED TO THE END — every
# diagnostic-corpus source and every example whose manifest expects a parse-family diagnostic, each in a
# CHILD process. Its own binary, so an abort under its arm is a red of this binary alone, and the corpus
# binary above (which analyzes in-process) keeps every fixture's verdict.
dss_add_test(NAME analysis/test_recovery_trees_are_analyzed
             SOURCES test_recovery_trees_are_analyzed.cpp)
# Reads each example's `expected.json` manifest.
target_link_libraries(dss_analysis_test_recovery_trees_are_analyzed
                      PRIVATE nlohmann_json::nlohmann_json)

# [[D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED]]: a diagnostic's reported
# position must be a position in a file the user can open, at EVERY tier — not
# just the parse tier, which is the only one that used to run while the
# preprocessor's line-map remap was still alive. Lives here for the same reason
# the corpus harness above does: it spans the semantic tier, the tiers below it,
# and the driver that renders them. Deliberately NOT folded into the corpus
# harness — that one builds its CU with no target and no `--define`s, so it has
# no synthesized prologue and cannot see this defect at all.
dss_add_test(NAME analysis/test_diagnostic_position_origin_coordinates
             SOURCES test_diagnostic_position_origin_coordinates.cpp)
