# LIR unit tests (ML5).

dss_add_test(NAME lir/test_lir           SOURCES test_lir.cpp)
dss_add_test(NAME lir/test_lir_liveness  SOURCES test_lir_liveness.cpp)
dss_add_test(NAME lir/test_lir_regalloc  SOURCES test_lir_regalloc.cpp)
dss_add_test(NAME lir/test_mir_to_lir    SOURCES test_mir_to_lir.cpp)
