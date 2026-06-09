# Linker unit tests (plan 14 LK4 substrate).

dss_add_test(NAME link/test_object_format_schema SOURCES test_object_format_schema.cpp)
dss_add_test(NAME link/test_linker               SOURCES test_linker.cpp)
# Cycle 24 — the pure cross-CU DEFINITION resolver tripwire (shared by the linker
# today + a future whole-program MIR merge). Direct value-in/value-out policy pins.
dss_add_test(NAME link/test_cross_cu_resolve     SOURCES test_cross_cu_resolve.cpp)
dss_add_test(NAME link/test_elf_writer           SOURCES test_elf_writer.cpp)
dss_add_test(NAME link/test_pe_writer            SOURCES test_pe_writer.cpp)
dss_add_test(NAME link/test_macho_writer         SOURCES test_macho_writer.cpp)
dss_add_test(NAME link/test_macho_chained_fixups SOURCES test_macho_chained_fixups.cpp)
dss_add_test(NAME link/test_string_table         SOURCES test_string_table.cpp)
dss_add_test(NAME link/test_elf_exec_writer      SOURCES test_elf_exec_writer.cpp)

# D-LK6-1 closure — ARM64 bit-shift relocation-formula variants.
dss_add_test(NAME link/test_aarch64_reloc_formulas SOURCES test_aarch64_reloc_formulas.cpp)

# Plan 14 LK7 — codesign-placeholder reservation tests.
dss_add_test(NAME link/test_codesign_placeholder SOURCES test_codesign_placeholder.cpp)

# D-LK7-ADHOC-CODESIGN-MACHO (increment 2/2) — ad-hoc CodeDirectory +
# SuperBlob FILL (real page-hash signature, not just a zero reservation).
dss_add_test(NAME link/test_macho_codesign       SOURCES test_macho_codesign.cpp)

# D-LK10-ENTRY-MACHO-EXIT — macOS-ARM64 runnable backend host-
# independent byte-pins (arm64 __stubs encoder + direct-plt trampoline
# dispatch + dispatch/symbolVa fail-loud + ad-hoc signature). The
# always-on guard; the runtime corpus runs only on the macos-arm64 leg.
dss_add_test(NAME link/test_macho_arm64_exit     SOURCES test_macho_arm64_exit.cpp)

# Plan 14 LK8 — WebAssembly module writer (skeleton).
dss_add_test(NAME link/test_wasm_writer          SOURCES test_wasm_writer.cpp)

# Plan 14 LK9 — SPIR-V module writer (skeleton).
dss_add_test(NAME link/test_spirv_writer         SOURCES test_spirv_writer.cpp)

# Plan 14 LK10 cycle 1 — file-emission substrate.
dss_add_test(NAME link/test_link_writer          SOURCES test_link_writer.cpp)

# Plan 14 §2.13 D-LK10-ENTRY Slice C — runnable-binary spine
# (first DSS-produced binary the OS actually runs).
dss_add_test(NAME link/test_lk10_entry_slice_c   SOURCES test_lk10_entry_slice_c.cpp)
