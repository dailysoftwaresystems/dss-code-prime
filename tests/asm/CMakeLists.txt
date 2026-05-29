# Assembler unit tests (plan 13 AS1).

dss_add_test(NAME asm/test_asm_substrate SOURCES test_asm_substrate.cpp)

# Reuse the LIR lowering test fixture so substrate tests can exercise
# the assembler on a real lowered c-subset program. The header includes
# its own dependencies — only the include path needs adding.
target_include_directories(dss_asm_test_asm_substrate
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir)
