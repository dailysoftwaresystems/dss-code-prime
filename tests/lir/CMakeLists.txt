# LIR unit tests (ML5).

dss_add_test(NAME lir/test_lir           SOURCES test_lir.cpp)
dss_add_test(NAME lir/test_lir_liveness  SOURCES test_lir_liveness.cpp)
dss_add_test(NAME lir/test_lir_callconv  SOURCES test_lir_callconv.cpp)
# Cycle 10k: mutate_target_schema.hpp consumes nlohmann_json for the
# in-memory JSON-strip negative pin (D-CSUBSET-LOCAL-INT-CODEGEN-
# NEGATIVE-PIN).
target_link_libraries(dss_lir_test_lir_callconv PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lir/test_lir_regalloc  SOURCES test_lir_regalloc.cpp)
# D-OPT-REGALLOC-EXCLUSION-BUFFER closure (2026-06-11): the >8-ordinal
# exclusion-union growth pins mutate idiv_op/shl implicitRegisters via
# mutate_target_schema.hpp.
target_link_libraries(dss_lir_test_lir_regalloc PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lir/test_lir_rewrite   SOURCES test_lir_rewrite.cpp)
# D-LIR-PER-INST-REG-CONSTRAINTS: the module side-structure
# carry-across (both pools, all four rebuild passes) and the verifier rules
# that make a forgotten carry loud instead of a silent miscompile.
dss_add_test(NAME lir/test_lir_pass_util SOURCES test_lir_pass_util.cpp)
dss_add_test(NAME lir/test_lir_verifier  SOURCES test_lir_verifier.cpp)
# D-LIR-EARLYCLOBBER-FLAG-UNSETTABLE-AFTER-EMISSION: `"=&r"` on an
# allocator-chosen register. Drives the SHIPPED asm dialects through the
# shared template engine so the flag is stamped on an instruction this
# tier did not emit — the exact shape `mir_to_lir`'s expansion produces.
dss_add_test(NAME lir/test_lir_earlyclobber SOURCES test_lir_earlyclobber.cpp)
# D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE: the two-address tie is an operand
# INDEX. Re-aims x86_64 `add`'s tie via mutate_target_schema.hpp, which
# needs nlohmann_json.
dss_add_test(NAME lir/test_lir_two_address_tie SOURCES test_lir_two_address_tie.cpp)
target_link_libraries(dss_lir_test_lir_two_address_tie PRIVATE nlohmann_json::nlohmann_json)
# D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE (the asm half): a `"+r"` operand's two
# halves bind to ONE register. Drives the REAL c-subset → HIR → MIR → LIR
# lowering for the positive claim and its untied control, and hand-builds the
# two descriptor shapes that must still REFUSE.
dss_add_test(NAME lir/test_lir_asm_tied_operand SOURCES test_lir_asm_tied_operand.cpp)
dss_add_test(NAME lir/test_lir_text      SOURCES test_lir_text.cpp)
dss_add_test(NAME lir/test_mir_to_lir    SOURCES test_mir_to_lir.cpp)
# FC1 (V2-4.X): the role-contract red-on-disable levers strip
# inputRoles/outputRoles off idiv_op via mutate_target_schema.hpp.
target_link_libraries(dss_lir_test_mir_to_lir PRIVATE nlohmann_json::nlohmann_json)
# FC3.5 sweep-c2: the FCmp predicate-mapping truth table + shape pins
# (D-COND-FLOAT-NAN-TRUTHINESS-FCMP). Uses mutate_target_schema.hpp for
# the strip-foeq/strip-fogt capability red-on-disable levers.
dss_add_test(NAME lir/test_fcmp_lowering SOURCES test_fcmp_lowering.cpp)
target_link_libraries(dss_lir_test_fcmp_lowering PRIVATE nlohmann_json::nlohmann_json)
