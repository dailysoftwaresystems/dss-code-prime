dss_add_test(NAME analysis/syntactic/test_parser_toy            SOURCES test_parser_toy.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_speculation    SOURCES test_parser_speculation.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_c_smoke SOURCES test_parser_c_smoke.cpp)
dss_add_test(NAME analysis/syntactic/test_pratt_walker          SOURCES test_pratt_walker.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_recovery       SOURCES test_parser_recovery.cpp)
dss_add_test(NAME analysis/syntactic/test_parser_cast_expr      SOURCES test_parser_cast_expr.cpp)
# D-PARSE-NINE-NESTED-CASTS-ARE-REFUSED-BY-THE-SPECULATION-CAP-WITH-A-FABRICATED-SYNTAX-ERROR:
# the FOUR stacked ceilings a nested cast chain walks, each one's fail-loud
# refusal pinned separately, plus the env-gated ordinary-thread depth probe the
# shipped numbers were measured with. See the file header.
dss_add_test(NAME analysis/syntactic/test_parser_speculation_ceilings SOURCES test_parser_speculation_ceilings.cpp)
# FC4 c1 (M4): the commitRequiresTypeName guard-POLARITY matrix against a
# synthetic grammar (preferType vs requireKnownType, loader shapes, the
# seeded-oracle channel under the strict polarity).
dss_add_test(NAME analysis/syntactic/test_parser_commit_polarity SOURCES test_parser_commit_polarity.cpp)
dss_add_test(NAME analysis/syntactic/test_significant_newline SOURCES test_significant_newline.cpp)
# P53 [[D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS]]: the
# one-owner-per-specifier-lead invariant at `/shapes/topLevel`, and the
# arbitrary-body-size parse it protects. See the file header for the measured
# cliff the pin exists to catch.
dss_add_test(NAME analysis/syntactic/test_parser_decl_specifier_lead SOURCES test_parser_decl_specifier_lead.cpp)
dss_add_test(NAME analysis/syntactic/test_corpus                SOURCES test_corpus.cpp)
# D-PARSE-SPECULATION-REFUSAL-REPLAY-IS-QUADRATIC: the refusal one past the
# speculation-depth ceiling used to cost Theta(D^2). The cost pin is on the
# parser's own tokenAccessCount (deterministic), the loudness pin is at a LARGE
# ceiling, and the identity pin renders every tree so the two renditions can be
# diffed byte-for-byte. See the file header.
dss_add_test(NAME analysis/syntactic/test_parser_speculation_refusal_cost SOURCES test_parser_speculation_refusal_cost.cpp)
# P65 [[D-CSUBSET-DECL-QUALIFIER-BEFORE-STORAGE-CLASS]] + residue (2) of
# [[D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES]]: the PARSER-tier half -- every C 6.7p2
# specifier ordering the references accept, the C 6.7.1p2 refusals, and the
# FIRST-set disjointness (localDeclSpecifiers vs kwDeclHead, and externDecl's
# narrow lead set) that keeps the declaration alts decidable on ONE token.
dss_add_test(NAME analysis/syntactic/test_parser_decl_specifier_order SOURCES test_parser_decl_specifier_order.cpp)
# P65: the empty element in a comma-separated list, and the C23 trailing comma
# that used to be the SAME `{optional}`. Both the enumerator list and the brace
# initializer list silently DROPPED an empty element (`{1,,3}` ran as `{1,3,0}`)
# while all three references refuse it. The mutant for red-on-disable is the
# language DOCUMENT, not an object -- see the file header.
dss_add_test(NAME analysis/syntactic/test_comma_list_empty_element SOURCES test_comma_list_empty_element.cpp)
