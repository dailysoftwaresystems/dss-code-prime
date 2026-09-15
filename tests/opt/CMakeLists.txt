# Optimizer unit tests.

dss_add_test(NAME opt/test_optimizer       SOURCES test_optimizer.cpp)
dss_add_test(NAME opt/test_const_fold      SOURCES test_const_fold.cpp)
dss_add_test(NAME opt/test_pipeline_loader SOURCES test_pipeline_loader.cpp)
dss_add_test(NAME opt/test_dce             SOURCES test_dce.cpp)
dss_add_test(NAME opt/test_dce_linkage     SOURCES test_dce_linkage.cpp)
dss_add_test(NAME opt/test_mem2reg         SOURCES test_mem2reg.cpp)
dss_add_test(NAME opt/test_copy_prop       SOURCES test_copy_prop.cpp)
dss_add_test(NAME opt/test_cse             SOURCES test_cse.cpp)
dss_add_test(NAME opt/test_simplify_cfg    SOURCES test_simplify_cfg.cpp)
dss_add_test(NAME opt/test_prune_unreachable SOURCES test_prune_unreachable.cpp)
dss_add_test(NAME opt/test_licm            SOURCES test_licm.cpp)
dss_add_test(NAME opt/test_inlining        SOURCES test_inlining.cpp)
dss_add_test(NAME opt/test_inline_growth_budget
             SOURCES test_inline_growth_budget.cpp)
dss_add_test(NAME opt/test_compile_options SOURCES test_compile_options.cpp)
dss_add_test(NAME opt/test_mir_alias       SOURCES test_mir_alias.cpp)
dss_add_test(NAME opt/test_mir_memory_clobbers SOURCES test_mir_memory_clobbers.cpp)
dss_add_test(NAME opt/test_call_graph_scc  SOURCES test_call_graph_scc.cpp)
dss_add_test(NAME opt/test_mir_id_remap    SOURCES test_mir_id_remap.cpp)
dss_add_test(NAME opt/test_mir_rebuild_helper SOURCES test_mir_rebuild_helper.cpp)

# Inline-asm P5 (plan 29 §4.4): the MIR asm descriptor survives EACH of the
# FOUR verbatim-copy sites individually, and an asm block is an opaque memory
# barrier to Load motion. Site 4 is `mir/merge/mir_merge.cpp`'s cross-CU cloner —
# it lives here rather than in tests/mir so all four sites are asserted in ONE
# file against ONE sentinel descriptor; a census split across two files is what
# let site 4 go missing in the first place.
dss_add_test(NAME opt/test_inline_asm_rebuild_carriage
             SOURCES test_inline_asm_rebuild_carriage.cpp)

# D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING: the OPTIMIZER half of inline
# gate rule 2. The row's call-site routing sends a shared library's call to its
# own preemptible definition through the LOADER; a splice that removed the call
# first would defeat it in the RELEASE ARM ONLY, which is exactly what was
# MEASURED before this gate arm existed. Its own target because the decision is
# a matrix — the over-refusal directions (a hidden callee, an undeclared format)
# cost every helper a call for a replacement that cannot happen.
dss_add_test(NAME opt/test_preemptible_definition_not_inlined
             SOURCES test_preemptible_definition_not_inlined.cpp)
