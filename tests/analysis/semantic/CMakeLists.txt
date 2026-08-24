# SE1+ semantic analysis test suite.

dss_add_test(NAME analysis/semantic/test_scope_tree
             SOURCES test_scope_tree.cpp)

dss_add_test(NAME analysis/semantic/test_symbol_table
             SOURCES test_symbol_table.cpp)

dss_add_test(NAME analysis/semantic/test_type_rules
             SOURCES test_type_rules.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_toy
             SOURCES test_semantic_analyzer_toy.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_c
             SOURCES test_semantic_analyzer_c.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_tsql
             SOURCES test_semantic_analyzer_tsql.cpp)

# Inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS): the front-end CAPTURE — the
# per-operand NodeIds, the constraint-string split, and the seven P5 refusals.
dss_add_test(NAME analysis/semantic/test_inline_asm_capture
             SOURCES test_inline_asm_capture.cpp)
target_include_directories(dss_analysis_semantic_test_inline_asm_capture
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_genericity
             SOURCES test_semantic_analyzer_genericity.cpp)

# FC3 c1: width/signedness front-end semantics (typeSpecifiers multiset
# table, dataModel threading, the integer-literal ladder, descriptor
# signatureByDataModel, loader fail-louds, toy/tsql typing pins).
dss_add_test(NAME analysis/semantic/test_fc3_width_semantics
             SOURCES test_fc3_width_semantics.cpp)

# D-LANG-TYPE-IDENTITY-VOCABULARY: type identity comes from the language
# vocabulary entry, representation from the target core — the `_Generic`
# distinctness pins, the same-representation conversion pins, the pointer-
# incompatibility tightening, the name-keyed conversion rank, and the loader's
# vocabulary-consistency fail-louds.
dss_add_test(NAME analysis/semantic/test_type_identity_vocabulary
             SOURCES test_type_identity_vocabulary.cpp)
target_include_directories(dss_analysis_semantic_test_type_identity_vocabulary
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
# Perturbs the SHIPPED c JSON (the loader accept/reject matrix).
target_link_libraries(dss_analysis_semantic_test_type_identity_vocabulary
                      PRIVATE nlohmann_json::nlohmann_json)

# FC4 c1: the C-declarator engine substrate against a SYNTHETIC schema —
# the `declarators` role-block loader matrix (M1), the shared name-
# extraction walk (M2), the declarator-inversion fold (M3), and the
# gatedMarkers fail-loud gates (M5).
dss_add_test(NAME analysis/semantic/test_declarator_engine
             SOURCES test_declarator_engine.cpp)

# All semantic e2e tests share `semantic_test_fixture.hpp`. Add tests/
# (NOT the repo root — its `VERSION` file shadows the C++ standard
# `<version>` header on case-insensitive filesystems and breaks
# libstdc++/gtest; same trap the compilation_unit suite hit).
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_toy
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_c
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_tsql
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_genericity
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_fc3_width_semantics
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
# The FC3 suite perturbs the SHIPPED c JSON — it needs nlohmann.
target_link_libraries(dss_analysis_semantic_test_fc3_width_semantics
                      PRIVATE nlohmann_json::nlohmann_json)
# The FC4 declarator suite perturbs its SYNTHETIC schema doc the same way
# (loader accept/reject matrix) and reuses the fixture + RawTreeBuilder.
target_include_directories(dss_analysis_semantic_test_declarator_engine
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(dss_analysis_semantic_test_declarator_engine
                      PRIVATE nlohmann_json::nlohmann_json)

# D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER: the DETECTOR,
# REPOINTED when the row CLOSED (P5c, 2026-08-17). It no longer compares two
# owners — there is one: it proves every shipped dialect's template lexer mode is
# DERIVED from the language's `semantics.inlineAsmTemplateLexemes`, by moving the
# language declaration in a scratch config tree and requiring every dialect's
# mode AND the semantic scan to move with it, plus the three load refusals
# (R1/R2/R3). Reads and REWRITES shipped JSON in a scratch tree, so it needs
# nlohmann and the tests/ include root for `test_support/repo_root.hpp` +
# `test_support/scoped_env.hpp`.
dss_add_test(NAME analysis/semantic/test_inline_asm_template_sigil_agreement
             SOURCES test_inline_asm_template_sigil_agreement.cpp)
target_include_directories(dss_analysis_semantic_test_inline_asm_template_sigil_agreement
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(dss_analysis_semantic_test_inline_asm_template_sigil_agreement
                      PRIVATE nlohmann_json::nlohmann_json)

# D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED: the FRONT-END half of
# the three-armed constraint binding. `AsmConstraintBinding::OperandKind` — the
# arm `"m"` and `"i"` land on — used to resolve to nothing, which is
# byte-identical to "the letter is not declared", so a declared letter was
# refused three tiers down with a reason that was false. Lives beside
# `test_inline_asm_capture` (the P5 capture pins) because it asks the same
# question one step later: not "what did the source write", but "what did the
# TARGET say the letter binds". It runs the real CST->HIR lowering with a
# shipped target in scope, which is the only configuration in which the
# resolution runs at all.
dss_add_test(NAME analysis/semantic/test_inline_asm_operand_kind_binding
             SOURCES test_inline_asm_operand_kind_binding.cpp)
# Shares `semantic_test_fixture.hpp` with the suites above, so it needs the
# tests/ include root for the same reason they do (NOT the repo root -- its
# `VERSION` file shadows `<version>` on case-insensitive filesystems).
target_include_directories(dss_analysis_semantic_test_inline_asm_operand_kind_binding
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P31 (2026-08-24): the four GNU COMPILE-TIME operand forms -- alignof on a VALUE
# ([[D-CSUBSET-ALIGNOF-VALUE-OPERAND]]), __builtin_offsetof ([[D-FFI-OFFSETOF-MACRO]]),
# __builtin_types_compatible_p and __builtin_choose_expr. Lives here rather than in
# the runnable corpus because it pins what a corpus example structurally CANNOT: the
# REFUSALS and the exact diagnostic CODE each one carries, plus the Pass-1.5
# const-expr tier (a different consumer from the Pass-2 + HIR path an ordinary
# expression takes). Shares `semantic_test_fixture.hpp`, so it needs the tests/
# include root for the same reason its siblings do.
dss_add_test(NAME analysis/semantic/test_builtin_compile_time_operators
             SOURCES test_builtin_compile_time_operators.cpp)
target_include_directories(dss_analysis_semantic_test_builtin_compile_time_operators
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P31 (2026-08-24): the GNU `__extension__` prefix ([[D-C-GNU-EXTENSION-KEYWORD]])
# and the GNU statement expression ([[D-C-GNU-STATEMENT-EXPRESSION]]). Lives here
# rather than only in the runnable corpus because it pins what a corpus example
# structurally CANNOT: the REFUSED positions -- which are the ONLY thing that
# distinguishes `__extension__`-as-a-prefix from `__extension__`-as-a-specifier,
# since every ACCEPTING shape passes under both readings -- plus the two
# statement-expression refusals and their exact diagnostic codes, plus the
# per-DATA-MODEL fold of a constant written through the prefix. Shares
# `semantic_test_fixture.hpp`, so it needs the tests/ include root for the same
# reason its siblings do.
dss_add_test(NAME analysis/semantic/test_gnu_extension_and_statement_expr
             SOURCES test_gnu_extension_and_statement_expr.cpp)
target_include_directories(dss_analysis_semantic_test_gnu_extension_and_statement_expr
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P32 lane A: [[D-C-ATTRIBUTE-CLAUSE-NAME-ADMITS-ONLY-IDENTIFIER-SO-A-KEYWORD-NAMED-ATTRIBUTE-IS-REFUSED]]
# -- an attribute clause NAME spelled with a KEYWORD (`__attribute__((__const__))`,
# 50 occurrences in ONE preprocessed glibc TU). Lives here rather than only in the
# runnable corpus because a corpus example must BUILD, so it can only exercise the
# ACCEPTING half -- and the accepting half is exactly what a GRAMMAR-ONLY widening
# also passes, while silently dropping every keyword-named attribute. The pins
# that separate the two are REFUSALS and their exact codes: an unmodelled
# keyword-spelled name must reach the strict gate and fail loud, and its modelled
# sibling must not. Shares `semantic_test_fixture.hpp`, so it needs the tests/
# include root for the same reason its siblings do.
dss_add_test(NAME analysis/semantic/test_attribute_clause_name_token_class
             SOURCES test_attribute_clause_name_token_class.cpp)
target_include_directories(dss_analysis_semantic_test_attribute_clause_name_token_class
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P32 lane A: [[D-C-SUBTREE-TYPE-WALKS-INTO-A-STATEMENT-EXPRESSION-BODY]] -- the
# semantic expression typer must take a statement expression's type from the LAST
# ITEM'S EXPRESSION (the same place the lowering takes its value) and not from a
# declaration inside the body. The runnable example owns the accepting half; this
# owns the REFUSING half, which an example structurally cannot carry: a `void`
# statement expression in value position must be REFUSED with its own code, not
# silently typed from whatever the body declared.
dss_add_test(NAME analysis/semantic/test_stmt_expr_value_type
             SOURCES test_stmt_expr_value_type.cpp)
target_include_directories(dss_analysis_semantic_test_stmt_expr_value_type
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# P32 lane A: [[D-CSUBSET-NODISCARD-INDIRECT-DISCARD-CONTEXT]] plus the SEMANTIC
# half of [[D-CSUBSET-ATTRIBUTE-PARAM-POSITION]]. Every discard shape is pinned as
# a PAIR -- a must-warn context beside a must-not-warn sibling -- because a
# discard test can fail in two opposite directions and only the pair can tell a
# fix that landed from a fix that over-reached. The parameter-attribute pin
# asserts the EFFECT (a `deprecated` parameter warns at its USE site) rather than
# the parse, which is what a grammar-only change would also pass.
dss_add_test(NAME analysis/semantic/test_nodiscard_discard_context
             SOURCES test_nodiscard_discard_context.cpp)
target_include_directories(dss_analysis_semantic_test_nodiscard_discard_context
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
