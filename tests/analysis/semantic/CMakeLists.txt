# SE1+ semantic analysis test suite.

dss_add_test(NAME analysis/semantic/test_scope_tree
             SOURCES test_scope_tree.cpp)

dss_add_test(NAME analysis/semantic/test_symbol_table
             SOURCES test_symbol_table.cpp)

dss_add_test(NAME analysis/semantic/test_type_rules
             SOURCES test_type_rules.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_toy
             SOURCES test_semantic_analyzer_toy.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_c_subset
             SOURCES test_semantic_analyzer_c_subset.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_tsql
             SOURCES test_semantic_analyzer_tsql.cpp)

dss_add_test(NAME analysis/semantic/test_semantic_analyzer_genericity
             SOURCES test_semantic_analyzer_genericity.cpp)

# All semantic e2e tests share `semantic_test_fixture.hpp`. Add tests/
# (NOT the repo root — its `VERSION` file shadows the C++ standard
# `<version>` header on case-insensitive filesystems and breaks
# libstdc++/gtest; same trap the compilation_unit suite hit).
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_toy
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_c_subset
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_tsql
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_semantic_test_semantic_analyzer_genericity
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
