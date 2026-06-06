# LIR unit tests (ML5).

dss_add_test(NAME lir/test_lir           SOURCES test_lir.cpp)
dss_add_test(NAME lir/test_lir_liveness  SOURCES test_lir_liveness.cpp)
dss_add_test(NAME lir/test_lir_callconv  SOURCES test_lir_callconv.cpp)
# Cycle 10k: mutate_target_schema.hpp consumes nlohmann_json for the
# in-memory JSON-strip negative pin (D-CSUBSET-LOCAL-INT-CODEGEN-
# NEGATIVE-PIN).
target_link_libraries(dss_lir_test_lir_callconv PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lir/test_lir_regalloc  SOURCES test_lir_regalloc.cpp)
dss_add_test(NAME lir/test_lir_rewrite   SOURCES test_lir_rewrite.cpp)
dss_add_test(NAME lir/test_lir_text      SOURCES test_lir_text.cpp)
dss_add_test(NAME lir/test_mir_to_lir    SOURCES test_mir_to_lir.cpp)
