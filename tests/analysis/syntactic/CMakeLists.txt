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
# P69 L2b2 (lane `lm`): the binder sketch's structure — its chains hold live bindings only, a
# declarator-end binding is taken back by its token's node, snapshots restore last in first out.
# Its own runner: a broken close makes every parse abort, and its claims run in child processes.
dss_add_test(NAME analysis/syntactic/test_binder_sketch_structure SOURCES test_binder_sketch_structure.cpp)
# P69 L2b2 (lane `lm`): the type-name triage's base ask, HOISTED before a probe opens. The pins: the
# reach read off the shipped data, a value base costing no probe at each run length, a type or unknown
# base left to its probe, every condition biting on a synthetic grammar beside its control, and the
# grammar-token view looking through a body mode's leaves (tsql-subset). See the file header.
dss_add_test(NAME analysis/syntactic/test_parser_triage_hoist SOURCES test_parser_triage_hoist.cpp)
# P69 L2b2: the hoist's IDENTITY WITNESS — every shipped corpus and example unit parses to the same
# trees and diagnostics with the hoist on and off, and the switch reaches the parser (the unit's probe
# count). See the file header.
dss_add_test(NAME analysis/syntactic/test_parser_triage_hoist_identity SOURCES test_parser_triage_hoist_identity.cpp)
# P69 L2b2: the hoist's off switch has ONE caller, the identity witness. Its verdict reads the
# repository's sources, so it carries `repo-guard`.
dss_add_test(NAME analysis/syntactic/test_triage_hoist_switch_has_one_caller
             SOURCES test_triage_hoist_switch_has_one_caller.cpp)
dss_label_repo_guard(analysis/syntactic/test_triage_hoist_switch_has_one_caller)
# P69 L2b2: a parse's scope chain does not grow with its input — a clean program twice as long ends
# with the same chain. See the file header.
dss_add_test(NAME analysis/syntactic/test_parser_scope_chain_bound SOURCES test_parser_scope_chain_bound.cpp)
# P69 L2b2 (lane `lm`): the empty typedef-named declaration split in two reads every row as before —
# the split table and the diagnostics corpus's scope kinds against the golden beside the test, captured
# before the split and the memory fix, its only deltas the rule names the split moves. See the header.
dss_add_test(NAME analysis/syntactic/test_parser_empty_declaration_split_table
             SOURCES test_parser_empty_declaration_split_table.cpp)
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

# P66 lane `cf` -- WHERE A C23 `[[...]]` ATTRIBUTE SEQUENCE MAY SIT IN A
# DECLARATION. `static [[maybe_unused]] int m = 1;` compiled at rc 0 with zero
# bytes on stderr, in TEN positions across five specifier-run rules, while gcc,
# clang and MSVC all three refuse every one of them; the LEADING position they
# all three accept had to survive, and so did the GNU `__attribute__` spelling,
# which gcc and clang admit mid-run. The mutant for red-on-disable is the
# language DOCUMENT -- see the file header.
dss_add_test(NAME analysis/syntactic/test_decl_attribute_sequence_position
             SOURCES test_decl_attribute_sequence_position.cpp)
