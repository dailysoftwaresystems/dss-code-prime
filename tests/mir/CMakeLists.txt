# MIR unit tests (ML1). One executable per file for ctest parallelism +
# per-file failure isolation.

dss_add_test(NAME mir/test_mir_opcode       SOURCES test_mir_opcode.cpp)
dss_add_test(NAME mir/test_mir_node         SOURCES test_mir_node.cpp)
dss_add_test(NAME mir/test_mir_literal_pool SOURCES test_mir_literal_pool.cpp)
dss_add_test(NAME mir/test_mir              SOURCES test_mir.cpp)
dss_add_test(NAME mir/test_mir_verifier     SOURCES test_mir_verifier.cpp)
dss_add_test(NAME mir/test_mir_dom          SOURCES test_mir_dom.cpp)
dss_add_test(NAME mir/test_mir_struct_markers SOURCES test_mir_struct_markers.cpp)
dss_add_test(NAME mir/test_mir_text         SOURCES test_mir_text.cpp)

# ML2 lowering tests.
dss_add_test(NAME mir/test_mir_lowering_c_subset SOURCES test_mir_lowering_c_subset.cpp)

# LK6 cycle 2d — HIR ExternFunction → MIR.externImports thread-through.
dss_add_test(NAME mir/test_mir_lowering_extern SOURCES test_mir_lowering_extern.cpp)

# Cycle 25 Stage B — cross-module MIR merge (mergeCuMirs) tested in isolation.
dss_add_test(NAME mir/test_mir_merge SOURCES test_mir_merge.cpp)

# D-FFI-PE-CRT-UCRT-MIGRATION Phase 3 — synthesizeStdioShim's va_list ARM
# SELECTION, pinned at the operand the synthesized call forwards. Separate from
# test_mir_merge (which pins the shim's SHAPE) because the two HomogeneousPointer
# arms produce structurally identical bodies: only the leaf differs, and picking
# the wrong one is a silent wrong-ABI forward on apple_arm64.
dss_add_test(NAME mir/test_synth_stdio_shim_valist SOURCES test_synth_stdio_shim_valist.cpp)
