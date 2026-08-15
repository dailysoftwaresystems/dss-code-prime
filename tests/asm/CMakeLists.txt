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
# D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE: the MOVN red-on-disable
# pins mutate the shipped arm64 JSON in memory (nlohmann consumer) —
# stripping a variant / a width key and proving the witness disappears.
target_link_libraries(dss_asm_test_asm_arm64
    PRIVATE nlohmann_json::nlohmann_json)

# TLS C2 (D-CSUBSET-THREAD-LOCAL): the arm64 twin — MRS TPIDR_EL0 +
# ADD/ADD hi12/lo12 word pins + the Variant-I identity / tprel-row /
# tlsAccess / TLSLE-native-id schema pins.
dss_add_test(NAME asm/test_asm_arm64_tls      SOURCES test_asm_arm64_tls.cpp)
# D-ASM-ARM64-BITWISE-W-FORMS-UNDECLARED + D-ARM64-C-PATH-32BIT-BITWISE-WIDENS-
# TO-64: byte-exact AND/ORR/EOR W-form and X-form pins (measured against
# aarch64-linux-gnu-as 2.42), the C-path width-32 election pin, and the
# strip-the-W-variant red-on-disable — which mutates the shipped arm64 JSON in
# memory (nlohmann consumer), the `mutate_target_schema.hpp` posture.
dss_add_test(NAME asm/test_asm_arm64_bitwise_w
             SOURCES test_asm_arm64_bitwise_w.cpp)
target_link_libraries(dss_asm_test_asm_arm64_bitwise_w
    PRIVATE nlohmann_json::nlohmann_json)
# D-ASM-ARM64-MOVZ-W-FORM-UNDECLARED: the same treatment for the IMMEDIATE move
# — byte-exact MOVZ W/X word pins (measured against aarch64-linux-gnu-as 2.42),
# the C-path width-32 election pin, and TWO red-on-disable mutants: strip the
# W arm (loud refusal, empty output) and reconstruct the PRE-FIX schema (real
# bytes, the OLD word). Same in-memory `mutate_target_schema.hpp` posture.
# ⚠ ITS MUTANTS ARE CONFIG-LEVEL, so it MUST run through ctest — `dss_add_test`
# sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever
# tree the shell stands in (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
dss_add_test(NAME asm/test_asm_arm64_movz_w
             SOURCES test_asm_arm64_movz_w.cpp)
target_link_libraries(dss_asm_test_asm_arm64_movz_w
    PRIVATE nlohmann_json::nlohmann_json)
# Plan 29 P4: the standalone assembly TEXT -> LIR walker. Its fixture builds a
# dialect grammar by mutating the shipped `asm-x86_64-att.lang.json` in memory
# (nlohmann consumer), the same posture `mutate_target_schema.hpp` takes for
# targets -- so every test stays anchored to the real grammar and token table.
dss_add_test(NAME asm/test_asm_text_to_lir    SOURCES test_asm_text_to_lir.cpp)
target_link_libraries(dss_asm_test_asm_text_to_lir
    PRIVATE nlohmann_json::nlohmann_json)

# Plan 29 P5: the EMBEDDED template path (`lowerAsmTemplateToLirRun`) — the
# SECOND caller of the one text->LIR engine. Drives the SHIPPED dialects and
# the SHIPPED targets unmutated, and pins BYTES rather than opcodes, because a
# template that elected the right opcode and the wrong encoding variant is the
# silent-miscompile class this subsystem exists to refuse.
# WARNING: its byte pins are config-level (they read the shipped `.lang.json`
# and `.target.json`), so it MUST run through ctest -- `dss_add_test` sets
# DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever
# tree the shell stands in (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
dss_add_test(NAME asm/test_asm_template_to_lir
             SOURCES test_asm_template_to_lir.cpp)
target_link_libraries(dss_asm_test_asm_template_to_lir
    PRIVATE nlohmann_json::nlohmann_json)

# The SHIPPED dialect documents, UNMUTATED. Its sibling above replaces
# `assembly.directives` with the rows each test needs, which is right for
# testing the ENGINE but leaves every one of those tests green whether or not
# the two shipped documents declare the rows at all. This file is the other
# half: it drives `asm-x86_64-att.lang.json` and `asm-arm64-gas.lang.json` as
# they ship, and it is the host-independent guard beside the
# `*_section_reserve_fill_readback` corpus examples, whose arm64 arm only runs
# where an emulator is installed.
dss_add_test(NAME asm/test_asm_shipped_dialects
             SOURCES test_asm_shipped_dialects.cpp)
target_link_libraries(dss_asm_test_asm_shipped_dialects
    PRIVATE nlohmann_json::nlohmann_json)

# Tests under tests/asm/ share `asm_test_support.hpp` via this
# include path. test_asm_substrate additionally reuses the LIR
# lowering fixture (lowered_lir_fixture.hpp lives in tests/lir/).
foreach(t test_asm_substrate test_asm_x86_variable test_asm_x86_binaryops
          test_asm_x86_sse test_asm_x86_relocs test_asm_arm64
          test_asm_arm64_binops test_lir_2addr_legalize test_asm_roundtrip
          test_asm_pipeline test_asm_width_axis test_asm_x86_tls
          test_asm_arm64_tls test_asm_text_to_lir test_asm_shipped_dialects
          test_asm_arm64_bitwise_w test_asm_arm64_movz_w
          test_asm_template_to_lir)
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
