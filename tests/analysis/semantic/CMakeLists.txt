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

# FC3 c1: width/signedness front-end semantics (typeSpecifiers multiset
# table, dataModel threading, the integer-literal ladder, descriptor
# signatureByDataModel, loader fail-louds, toy/tsql typing pins).
dss_add_test(NAME analysis/semantic/test_fc3_width_semantics
             SOURCES test_fc3_width_semantics.cpp)

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
target_include_directories(dss_analysis_semantic_test_fc3_width_semantics
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
# The FC3 suite perturbs the SHIPPED c-subset JSON — it needs nlohmann.
target_link_libraries(dss_analysis_semantic_test_fc3_width_semantics
                      PRIVATE nlohmann_json::nlohmann_json)
