# Linker unit tests (plan 14 LK4 substrate).

dss_add_test(NAME link/test_object_format_schema SOURCES test_object_format_schema.cpp)
dss_add_test(NAME link/test_linker               SOURCES test_linker.cpp)
# Cycle 24 — the pure cross-CU DEFINITION resolver tripwire (shared by the linker
# today + a future whole-program MIR merge). Direct value-in/value-out policy pins.
dss_add_test(NAME link/test_cross_cu_resolve     SOURCES test_cross_cu_resolve.cpp)
dss_add_test(NAME link/test_crosscu_link_formats SOURCES test_crosscu_link_formats.cpp)
dss_add_test(NAME link/test_elf_writer           SOURCES test_elf_writer.cpp)
dss_add_test(NAME link/test_pe_writer            SOURCES test_pe_writer.cpp)
dss_add_test(NAME link/test_macho_writer         SOURCES test_macho_writer.cpp)
dss_add_test(NAME link/test_macho_chained_fixups SOURCES test_macho_chained_fixups.cpp)
dss_add_test(NAME link/test_string_table         SOURCES test_string_table.cpp)
dss_add_test(NAME link/test_elf_exec_writer      SOURCES test_elf_exec_writer.cpp)

# c150 (D-LK1-4 shared-library half) — ELF ET_DYN (.so) writer:
# ET_DYN header/phdr pins, DT_SONAME knob, real-named .dynsym exports
# findable via the SysV .hash walk, R_X86_64_RELATIVE for internal
# absolute slots, got-indirect extern data, the undefined-extern
# three-flavor policy, and the dyn fail-loud belts.
dss_add_test(NAME link/test_elf_dyn_writer       SOURCES test_elf_dyn_writer.cpp)

# c152 (D-LK2-4 dll half) — PE32+ .dll writer: IMAGE_FILE_DLL +
# entry-0 header pins, the .edata export directory (SORTED Name
# Pointer Table — the GetProcAddress binary-search invariant, ordinal
# -> EAT mapping, data exports), DIR64 .reloc completeness (fn-ptr
# table + function-extern thunk bake), the extern-DATA-slot reject
# (D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR, dll + exec arms), the
# DYNAMIC_BASE text-abs-reloc belt, and the dll validate() shape
# rules (no entry cluster / characteristics bits).
dss_add_test(NAME link/test_pe_dll_writer        SOURCES test_pe_dll_writer.cpp)

# c153 (D-LK3-3) — Mach-O MH_DYLIB (.dylib) writer: filetype/flags/
# LC_ID_DYLIB header pins (no LC_MAIN / LC_LOAD_DYLINKER /
# __PAGEZERO), the LC_DYLD_INFO export trie WALKED with dyld's own
# algorithm (fn + data exports, strict-prefix radix pair, Local
# omitted), internal fn-ptr slot REBASE completeness, the
# extern-addr data-slot symbol-based BIND
# (D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR Mach-O half — data + function
# extern flavors), dylib codesign execSegFlags=0, weak-export /
# entry-override belts, and the dylib validate() shape rules.
dss_add_test(NAME link/test_macho_dylib_writer   SOURCES test_macho_dylib_writer.cpp)

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

# c163 (D-LK-STATIC-ARCHIVE-WRITER, writer half of D-FF1-AR-WRITER-STATIC-LINK)
# — the GNU/System V `ar` static-archive WRITER: byte-exact framing + armap
# pins, round-trip through the shipped c161 reader (the inverse-pair oracle),
# wrong-endian-count red-on-disable, multi-member + "//" long-name table, the
# fail-loud belts, and the end-to-end `linkAndWriteStaticArchive` composition
# bundling a real DSS ELF ET_REL `.o` member.
dss_add_test(NAME link/test_ar_writer            SOURCES test_ar_writer.cpp)
