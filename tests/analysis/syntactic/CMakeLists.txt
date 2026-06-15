dss_add_test(NAME analysis/syntactic/test_parser_toy            SOURCES test_parser_toy.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_speculation    SOURCES test_parser_speculation.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_c_subset_smoke SOURCES test_parser_c_subset_smoke.cpp)
dss_add_test(NAME analysis/syntactic/test_pratt_walker          SOURCES test_pratt_walker.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_recovery       SOURCES test_parser_recovery.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_cast_expr      SOURCES test_parser_cast_expr.cpp)
# FC4 c1 (M4): the commitRequiresTypeName guard-POLARITY matrix against a
# synthetic grammar (preferType vs requireKnownType, loader shapes, the
# seeded-oracle channel under the strict polarity).
dss_add_test(NAME analysis/syntactic/test_parser_commit_polarity SOURCES test_parser_commit_polarity.cpp)
dss_add_test(NAME analysis/syntactic/test_corpus                SOURCES test_corpus.cpp)
