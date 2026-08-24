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

# Inline-asm P5 (plan 29 §4.4) — the MIR carriage: opcode rows, the ReturnPiece
# payload split, the dedicated-builder refusals, and the `asm goto` edge
# placement. Its own binary because it carries DEATH tests (the structural
# backstops), which gtest runs by forking the whole executable.
dss_add_test(NAME mir/test_mir_inline_asm SOURCES test_mir_inline_asm.cpp)

# ML2 lowering tests.
dss_add_test(NAME mir/test_mir_lowering_c SOURCES test_mir_lowering_c.cpp)

# LK6 cycle 2d — HIR ExternFunction → MIR.externImports thread-through.
dss_add_test(NAME mir/test_mir_lowering_extern SOURCES test_mir_lowering_extern.cpp)

# Cycle 25 Stage B — cross-module MIR merge (mergeCuMirs) tested in isolation.
dss_add_test(NAME mir/test_mir_merge SOURCES test_mir_merge.cpp)

# D-MIR-OVERLAP-STRUCT-ZERO-INIT — brace-init of a struct whose members SHARE
# BYTES (a c107 explicit-offset overlay). Hand-built HIR, because explicit offsets
# cannot be spelled in C source — they arrive only from a shipped descriptor.
dss_add_test(NAME mir/test_overlap_struct_zero_init
             SOURCES test_overlap_struct_zero_init.cpp)

# D-FFI-PE-CRT-UCRT-MIGRATION Phase 3 — synthesizeStdioShim's va_list ARM
# SELECTION, pinned at the operand the synthesized call forwards. Separate from
# test_mir_merge (which pins the shim's SHAPE) because the two HomogeneousPointer
# arms produce structurally identical bodies: only the leaf differs, and picking
# the wrong one is a silent wrong-ABI forward on apple_arm64.
dss_add_test(NAME mir/test_synth_stdio_shim_valist SOURCES test_synth_stdio_shim_valist.cpp)

# D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS — the three
# operand/payload-carrying `.dssir` forms whose writer out-ran its reader, plus
# the class guard for the next one. Its own binary because the pre-fix behaviour
# of the `indirectbr` arm was a PROCESS ABORT, which takes every sibling
# assertion in the same executable down with it.
dss_add_test(NAME mir/test_mir_text_operand_forms SOURCES test_mir_text_operand_forms.cpp)

# D-ASM-IMMEDIATE-CONSTRAINT-FORM-NOT-REALIZED
# D-MIR-LVALUE-REFUSAL-RENDERS-A-RAW-ORDINAL-NOT-A-NAME
# ⚠ Those two ids sit on their own lines because an anchor id split across a
# line break is invisible to every grep AND to the registry guard — the exact
# defect D-ANCHOR-ID-WRAPPED-ACROSS-A-LINE-BREAK-IS-INVISIBLE-TO-EVERY-GREP
# records, and this comment carried an instance of it until the step-10 audit
# found it. Break BEFORE an id or AFTER it, never inside it.
# The `"i"` IMMEDIATE constraint at the tier that owns
# the two facts nothing below it can recover: the CONSTANT PROOF (gcc refuses a
# non-constant with "impossible constraint in 'asm'", and so must this) and the
# fact that the operand FORM is read from the TARGET rather than from the
# letter's spelling. The last arm re-spells `"i"` in the shipped JSON and demands
# the identical lowering, so a `constraint == "i"` implementation is red there
# and green everywhere else -- which is why it needs the nlohmann consumer.
dss_add_test(NAME mir/test_mir_inline_asm_immediate
             SOURCES test_mir_inline_asm_immediate.cpp)
target_link_libraries(dss_mir_test_mir_inline_asm_immediate
                      PRIVATE nlohmann_json::nlohmann_json)
