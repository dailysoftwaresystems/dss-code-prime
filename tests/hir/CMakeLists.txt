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
dss_add_test(NAME hir/test_source_span       SOURCES test_source_span.cpp)
dss_add_test(NAME hir/test_hir_verifier      SOURCES test_hir_verifier.cpp)
dss_add_test(NAME hir/test_const_eval        SOURCES test_const_eval.cpp)
dss_add_test(NAME hir/test_hir_text          SOURCES test_hir_text.cpp)
dss_add_test(NAME hir/test_hir_lowering_c_subset SOURCES test_hir_lowering_c_subset.cpp)
dss_add_test(NAME hir/test_hir_lowering_toy   SOURCES test_hir_lowering_toy.cpp)
dss_add_test(NAME hir/test_hir_lowering_generic   SOURCES test_hir_lowering_generic.cpp)
dss_add_test(NAME hir/test_hir_lowering_tsql  SOURCES test_hir_lowering_tsql.cpp)
dss_add_test(NAME hir/test_hir_lowering_multi_lang SOURCES test_hir_lowering_multi_lang.cpp)

# D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET + D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM
# + D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED + the `_BitInt` half of
# D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS. Separate from
# test_hir_text.cpp because those arms pin the FORMAT and these pin the
# VOCABULARY and the two capability gaps.
dss_add_test(NAME hir/test_hir_text_vocabulary_and_capability SOURCES test_hir_text_vocabulary_and_capability.cpp)

# D-HIR-TEXT-INLINE-ASM-OPERAND-KIND-DROPPED-IN-TRANSIT — the CONSEQUENCE half.
# Separate from test_hir_text.cpp because those arms pin the round trip and this
# one pins what the NEXT tier reads back: a dropped operand-form binding is
# indistinguishable from "no target was in scope", so `hir_to_mir` refuses a
# declared letter with a false reason. It therefore links the MIR lowering.
dss_add_test(NAME hir/test_hir_text_operand_form_reaches_mir
             SOURCES test_hir_text_operand_form_reaches_mir.cpp)
