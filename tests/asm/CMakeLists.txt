# Assembler unit tests (plan 13 AS1).

dss_add_test(NAME asm/test_asm_substrate     SOURCES test_asm_substrate.cpp)
dss_add_test(NAME asm/test_asm_x86_variable  SOURCES test_asm_x86_variable.cpp)
dss_add_test(NAME asm/test_asm_x86_binaryops SOURCES test_asm_x86_binaryops.cpp)
dss_add_test(NAME asm/test_asm_x86_sse       SOURCES test_asm_x86_sse.cpp)
# FC2 Part A⇄B integration: source-level cast → SSE bytes.
dss_add_test(NAME asm/test_asm_cast_integration SOURCES test_asm_cast_integration.cpp)
dss_add_test(NAME asm/test_asm_x86_relocs    SOURCES test_asm_x86_relocs.cpp)
dss_add_test(NAME asm/test_asm_arm64         SOURCES test_asm_arm64.cpp)
dss_add_test(NAME asm/test_asm_arm64_binops  SOURCES test_asm_arm64_binops.cpp)
dss_add_test(NAME asm/test_lir_2addr_legalize SOURCES test_lir_2addr_legalize.cpp)
dss_add_test(NAME asm/test_asm_roundtrip      SOURCES test_asm_roundtrip.cpp)
dss_add_test(NAME asm/test_asm_pipeline       SOURCES test_asm_pipeline.cpp)
dss_add_test(NAME asm/test_lk10_entry_slice_a SOURCES test_lk10_entry_slice_a.cpp)
# FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): width-axis byte pins — x86 32-bit
# no-REX.W forms + arm64 W-forms + sext/trunc + loader width validation
# + the strip-a-variant red-on-disable (mutation-substrate consumer).
dss_add_test(NAME asm/test_asm_width_axis     SOURCES test_asm_width_axis.cpp)
# TLS C1 (D-CSUBSET-THREAD-LOCAL): tlsbase / tls-lea byte pins + the
# tls identity + tlsAccess schema pins + loader/validate fail-louds.
dss_add_test(NAME asm/test_asm_x86_tls        SOURCES test_asm_x86_tls.cpp)
# TLS C2 (D-CSUBSET-THREAD-LOCAL): the arm64 twin — MRS TPIDR_EL0 +
# ADD/ADD hi12/lo12 word pins + the Variant-I identity / tprel-row /
# tlsAccess / TLSLE-native-id schema pins.
dss_add_test(NAME asm/test_asm_arm64_tls      SOURCES test_asm_arm64_tls.cpp)

# Tests under tests/asm/ share `asm_test_support.hpp` via this
# include path. test_asm_substrate additionally reuses the LIR
# lowering fixture (lowered_lir_fixture.hpp lives in tests/lir/).
foreach(t test_asm_substrate test_asm_x86_variable test_asm_x86_binaryops
          test_asm_x86_sse test_asm_x86_relocs test_asm_arm64
          test_asm_arm64_binops test_lir_2addr_legalize test_asm_roundtrip
          test_asm_pipeline test_asm_width_axis test_asm_x86_tls
          test_asm_arm64_tls)
    target_include_directories(dss_asm_${t}
        PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)
endforeach()
target_include_directories(dss_asm_test_asm_substrate
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir)
# FC2 Part B: the registerClassOps schema pins mutate the shipped
# x86_64 JSON via mutate_target_schema.hpp (nlohmann consumer).
target_link_libraries(dss_asm_test_asm_substrate
    PRIVATE nlohmann_json::nlohmann_json)
# FC3 c2: the width-axis red-on-disable + loader pins mutate the
# shipped JSON via the same substrate.
target_link_libraries(dss_asm_test_asm_width_axis
    PRIVATE nlohmann_json::nlohmann_json)
# TLS C1: the tlsAccess negative pins mutate the shipped elf-exec
# format JSON in memory (nlohmann consumer).
target_link_libraries(dss_asm_test_asm_x86_tls
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_pipeline
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir)
