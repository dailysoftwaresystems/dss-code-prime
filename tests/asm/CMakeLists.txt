# Assembler unit tests (plan 13 AS1).

dss_add_test(NAME asm/test_asm_substrate    SOURCES test_asm_substrate.cpp)
dss_add_test(NAME asm/test_asm_x86_variable SOURCES test_asm_x86_variable.cpp)

# Both test targets get the shared `asm_test_support.hpp` via the
# tests/asm/ include path. The substrate test additionally reuses
# the LIR lowering test fixture (lowered_lir_fixture.hpp lives in
# tests/lir/).
target_include_directories(dss_asm_test_asm_substrate
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir
            ${CMAKE_SOURCE_DIR}/tests/asm)
target_include_directories(dss_asm_test_asm_x86_variable
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)
