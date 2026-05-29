# Assembler unit tests (plan 13 AS1).

dss_add_test(NAME asm/test_asm_substrate     SOURCES test_asm_substrate.cpp)
dss_add_test(NAME asm/test_asm_x86_variable  SOURCES test_asm_x86_variable.cpp)
dss_add_test(NAME asm/test_asm_x86_binaryops SOURCES test_asm_x86_binaryops.cpp)
dss_add_test(NAME asm/test_asm_arm64         SOURCES test_asm_arm64.cpp)
dss_add_test(NAME asm/test_lir_2addr_legalize SOURCES test_lir_2addr_legalize.cpp)

# Tests under tests/asm/ share `asm_test_support.hpp` via this
# include path. test_asm_substrate additionally reuses the LIR
# lowering fixture (lowered_lir_fixture.hpp lives in tests/lir/).
foreach(t test_asm_substrate test_asm_x86_variable test_asm_x86_binaryops
          test_asm_arm64 test_lir_2addr_legalize)
    target_include_directories(dss_asm_${t}
        PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)
endforeach()
target_include_directories(dss_asm_test_asm_substrate
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir)
