# HIR unit tests (HR1). One executable per file for ctest parallelism + per-file
# failure isolation.

dss_add_test(NAME hir/test_hir_node          SOURCES test_hir_node.cpp)
dss_add_test(NAME hir/test_hir_kind_registry SOURCES test_hir_kind_registry.cpp)
dss_add_test(NAME hir/test_hir               SOURCES test_hir.cpp)
dss_add_test(NAME hir/test_hir_cursor        SOURCES test_hir_cursor.cpp)
dss_add_test(NAME hir/test_hir_genericity    SOURCES test_hir_genericity.cpp)
