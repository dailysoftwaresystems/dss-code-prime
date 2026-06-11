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
