# Cross-cutting diagnostic-corpus harness (V2-4 Part B): parses +
# semantically analyzes every malformed source under
# `tests/corpus/diagnostics/<lang>/` and pins each emitted diagnostic's
# code + line:col against a sibling `.diag` golden (golden_file.hpp).
# Lives here (not under syntactic/ or semantic/) because it spans both
# the parser (`P_*`) and semantic (`S_*`) tiers.
dss_add_test(NAME analysis/test_diagnostic_corpus
             SOURCES test_diagnostic_corpus.cpp)
