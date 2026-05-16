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
