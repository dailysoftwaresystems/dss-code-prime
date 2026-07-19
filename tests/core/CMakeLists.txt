# core/ unit tests. Each test lands as a separate executable so ctest can
# parallelize and pinpoint failures.

dss_add_test(NAME core/test_strong_ids          SOURCES test_strong_ids.cpp)
dss_add_test(NAME core/test_source_span         SOURCES test_source_span.cpp)
dss_add_test(NAME core/test_source_buffer       SOURCES test_source_buffer.cpp)
dss_add_test(NAME core/test_token               SOURCES test_token.cpp)
dss_add_test(NAME core/test_rule_id             SOURCES test_rule_id.cpp)
dss_add_test(NAME core/test_parse_diagnostic    SOURCES test_parse_diagnostic.cpp)
dss_add_test(NAME core/test_config_path_walk    SOURCES test_config_path_walk.cpp)
dss_add_test(NAME core/test_char_decode         SOURCES test_char_decode.cpp)
dss_add_test(NAME core/test_diagnostic_reporter SOURCES test_diagnostic_reporter.cpp)
dss_add_test(NAME core/test_unsuppressable_codes SOURCES test_unsuppressable_codes.cpp)
dss_add_test(NAME core/test_grammar_schema      SOURCES test_grammar_schema.cpp)
dss_add_test(NAME core/test_target_schema       SOURCES test_target_schema.cpp)
dss_add_test(NAME core/test_number_decode       SOURCES test_number_decode.cpp)
dss_add_test(NAME core/test_bit_int_value       SOURCES test_bit_int_value.cpp)
dss_add_test(NAME core/test_wide_float_value    SOURCES test_wide_float_value.cpp)
dss_add_test(NAME core/test_tree_builder        SOURCES test_tree_builder.cpp)
dss_add_test(NAME core/test_tree_cursor         SOURCES test_tree_cursor.cpp)
dss_add_test(NAME core/test_tree_node           SOURCES test_tree_node.cpp)
dss_add_test(NAME core/test_tree                SOURCES test_tree.cpp)
dss_add_test(NAME core/test_decl_prefix_strip   SOURCES test_decl_prefix_strip.cpp)
dss_add_test(NAME core/test_tree_visitor        SOURCES test_tree_visitor.cpp)
dss_add_test(NAME core/test_tree_attrs          SOURCES test_tree_attrs.cpp)
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
dss_add_test(NAME core/test_sha256              SOURCES test_sha256.cpp)

# Generalized arena substrate (SP1). The test sources live under substrate/
# and include the shared `substrate/arena_test_types.hpp`, so this directory
# is on their include path.
dss_add_test(NAME core/substrate/test_arena_tag       SOURCES substrate/test_arena_tag.cpp)
dss_add_test(NAME core/substrate/test_arena_container SOURCES substrate/test_arena_container.cpp)
dss_add_test(NAME core/substrate/test_arena_attribute SOURCES substrate/test_arena_attribute.cpp)
dss_add_test(NAME core/substrate/test_arena_concepts  SOURCES substrate/test_arena_concepts.cpp)
dss_add_test(NAME core/substrate/test_mint_monotonic_id SOURCES substrate/test_mint_monotonic_id.cpp)
dss_add_test(NAME core/substrate/test_large_stack_call  SOURCES substrate/test_large_stack_call.cpp)
dss_add_test(NAME core/substrate/test_relocation_table  SOURCES substrate/test_relocation_table.cpp)
target_link_libraries(dss_core_substrate_test_relocation_table PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_core_substrate_test_arena_tag       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_container PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_attribute PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_concepts  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Core type lattice + extension registry (SP2).
dss_add_test(NAME core/type_lattice/test_core_type     SOURCES type_lattice/test_core_type.cpp)
dss_add_test(NAME core/type_lattice/test_type_interner SOURCES type_lattice/test_type_interner.cpp)
dss_add_test(NAME core/type_lattice/test_type_registry SOURCES type_lattice/test_type_registry.cpp)
dss_add_test(NAME core/type_lattice/test_type_reintern SOURCES type_lattice/test_type_reintern.cpp)
dss_add_test(NAME core/test_type_layout                SOURCES test_type_layout.cpp)
dss_add_test(NAME core/test_aggregate_abi              SOURCES test_aggregate_abi.cpp)
# D-CSUBSET-BITFIELD-ABI-EXACT: the CI cross-compile-COMPARE witness — builds the
# struct battery with the host's NATIVE C compiler (cl.exe / gcc / clang) and
# asserts dss's computeLayout (host-ABI strategy) is byte-identical. Skips when no
# toolchain. (The macOS leg is the only runtime witness for Apple-arm64.)
dss_add_test(NAME core/test_bitfield_abi_conformance   SOURCES test_bitfield_abi_conformance.cpp)
# D-CSUBSET-PACKED: the packed-struct cross-compile-COMPARE witness — builds the
# packed battery with the host's NATIVE C compiler (cl.exe `#pragma pack(1)` /
# gcc/clang `__attribute__((packed))`) and asserts dss's computeLayout(packed) is
# byte-identical (sizeof + per-field offsetof). Skips when no toolchain.
dss_add_test(NAME core/test_packed_abi_conformance     SOURCES test_packed_abi_conformance.cpp)
