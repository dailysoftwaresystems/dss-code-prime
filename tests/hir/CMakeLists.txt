# HIR unit tests (HR1–HR5). One executable per file for ctest parallelism +
# per-file failure isolation.

dss_add_test(NAME hir/test_hir_node          SOURCES test_hir_node.cpp)
dss_add_test(NAME hir/test_hir_kind_registry SOURCES test_hir_kind_registry.cpp)
dss_add_test(NAME hir/test_hir_intrinsic_registry SOURCES test_hir_intrinsic_registry.cpp)
dss_add_test(NAME hir/test_hir               SOURCES test_hir.cpp)
dss_add_test(NAME hir/test_hir_cursor        SOURCES test_hir_cursor.cpp)
dss_add_test(NAME hir/test_hir_genericity    SOURCES test_hir_genericity.cpp)
dss_add_test(NAME hir/test_hir_op            SOURCES test_hir_op.cpp)
dss_add_test(NAME hir/test_hir_expr          SOURCES test_hir_expr.cpp)
dss_add_test(NAME hir/test_hir_stmt          SOURCES test_hir_stmt.cpp)
dss_add_test(NAME hir/test_hir_decl          SOURCES test_hir_decl.cpp)
dss_add_test(NAME hir/test_hir_attrs         SOURCES test_hir_attrs.cpp)
dss_add_test(NAME hir/test_hir_verifier      SOURCES test_hir_verifier.cpp)
