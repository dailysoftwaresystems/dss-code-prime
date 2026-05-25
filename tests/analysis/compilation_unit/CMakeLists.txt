dss_add_test(NAME analysis/compilation_unit/test_compilation_unit
             SOURCES test_compilation_unit.cpp)

dss_add_test(NAME analysis/compilation_unit/test_unit_attribute
             SOURCES test_unit_attribute.cpp)

dss_add_test(NAME analysis/compilation_unit/test_symbol_population
             SOURCES test_symbol_population.cpp)

dss_add_test(NAME analysis/compilation_unit/test_import_resolver
             SOURCES test_import_resolver.cpp)

# Reaches the shared `core/e2e_harness.hpp` (tokenize-a-shipped-config helper)
# from a sibling test directory. dss_add_test only puts src/ on the include
# path; add tests/ so `core/...` resolves. (Do NOT add the repo root — its
# `VERSION` file shadows the C++ standard `<version>` header on case-insensitive
# filesystems and breaks libstdc++/gtest.)
target_include_directories(dss_analysis_compilation_unit_test_compilation_unit
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_compilation_unit_test_unit_attribute
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_compilation_unit_test_symbol_population
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_include_directories(dss_analysis_compilation_unit_test_import_resolver
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
