# core/ unit tests. Each test lands as a separate executable so ctest can
# parallelize and pinpoint failures.

dss_add_test(NAME core/test_strong_ids          SOURCES test_strong_ids.cpp)
dss_add_test(NAME core/test_source_span         SOURCES test_source_span.cpp)
dss_add_test(NAME core/test_source_buffer       SOURCES test_source_buffer.cpp)
dss_add_test(NAME core/test_token               SOURCES test_token.cpp)
dss_add_test(NAME core/test_rule_id             SOURCES test_rule_id.cpp)
dss_add_test(NAME core/test_parse_diagnostic    SOURCES test_parse_diagnostic.cpp)
dss_add_test(NAME core/test_diagnostic_reporter SOURCES test_diagnostic_reporter.cpp)
dss_add_test(NAME core/test_grammar_schema      SOURCES test_grammar_schema.cpp)
dss_add_test(NAME core/test_tree_builder        SOURCES test_tree_builder.cpp)
dss_add_test(NAME core/test_tree_cursor         SOURCES test_tree_cursor.cpp)
dss_add_test(NAME core/test_tree_node           SOURCES test_tree_node.cpp)
dss_add_test(NAME core/test_tree                SOURCES test_tree.cpp)
dss_add_test(NAME core/test_tree_visitor        SOURCES test_tree_visitor.cpp)
dss_add_test(NAME core/test_tree_attrs          SOURCES test_tree_attrs.cpp)
dss_add_test(NAME core/test_tree_views          SOURCES test_tree_views.cpp)
dss_add_test(NAME core/test_tree_end_to_end     SOURCES test_tree_end_to_end.cpp)
dss_add_test(NAME core/test_c_subset            SOURCES test_c_subset.cpp)
dss_add_test(NAME core/test_operator_table      SOURCES test_operator_table.cpp)
dss_add_test(NAME core/test_schema_cursor       SOURCES test_schema_cursor.cpp)
dss_add_test(NAME core/test_schema_walker       SOURCES test_schema_walker.cpp)
dss_add_test(NAME core/test_contextual_keywords SOURCES test_contextual_keywords.cpp)
dss_add_test(NAME core/test_scope_require       SOURCES test_scope_require.cpp)
dss_add_test(NAME core/test_checkpoint          SOURCES test_checkpoint.cpp)
dss_add_test(NAME core/test_lexer_modes         SOURCES test_lexer_modes.cpp)
dss_add_test(NAME core/test_string_style        SOURCES test_string_style.cpp)
dss_add_test(NAME core/test_tsql_subset         SOURCES test_tsql_subset.cpp)

# Generalized arena substrate (SP1). The test sources live under substrate/
# and include the shared `substrate/arena_test_types.hpp`, so this directory
# is on their include path.
dss_add_test(NAME core/substrate/test_arena_tag       SOURCES substrate/test_arena_tag.cpp)
dss_add_test(NAME core/substrate/test_arena_container SOURCES substrate/test_arena_container.cpp)
dss_add_test(NAME core/substrate/test_arena_attribute SOURCES substrate/test_arena_attribute.cpp)
dss_add_test(NAME core/substrate/test_arena_concepts  SOURCES substrate/test_arena_concepts.cpp)
target_include_directories(dss_core_substrate_test_arena_tag       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_container PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_attribute PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_concepts  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Core type lattice + extension registry (SP2).
dss_add_test(NAME core/type_lattice/test_core_type     SOURCES type_lattice/test_core_type.cpp)
dss_add_test(NAME core/type_lattice/test_type_interner SOURCES type_lattice/test_type_interner.cpp)
dss_add_test(NAME core/type_lattice/test_type_registry SOURCES type_lattice/test_type_registry.cpp)
