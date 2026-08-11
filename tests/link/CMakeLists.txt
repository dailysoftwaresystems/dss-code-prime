# Linker unit tests (plan 14 LK4 substrate).

dss_add_test(NAME link/test_object_format_schema SOURCES test_object_format_schema.cpp)

# D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN — the per-format C-symbol decoration
# axis (`cSymbolDecoration`), given its own file the way the sibling
# `headerNameMatching` axis has `tests/core/test_header_name_matching.cpp`:
# the closed vocabulary + its non-nameable sentinel, the loader's REQUIRED
# rule, the loader-BYPASSING `validate()` rule (which no JSON-fed test can
# cover), the unconditional-across-every-flavor proof, and the
# enumerated-from-disk sweep over all 24 shipped formats.
dss_add_test(NAME link/test_c_symbol_decoration  SOURCES test_c_symbol_decoration.cpp)
# Parses a shipped `.format.json` directly so its "shipped file MINUS exactly one
# key" fixture is copied VERBATIM and cannot drift from the real file — hence the
# json link (the `ffi/test_shipped_type_consistency` precedent).
target_link_libraries(dss_link_test_c_symbol_decoration
                      PRIVATE nlohmann_json::nlohmann_json)
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

# D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING — the emitted artifact must be a
# NEW FILE IDENTITY on every write (staged in a sibling temp, renamed over the
# target), never a truncate-in-place that recycles the destination's inode.
# macOS attaches an exec DENY to a soured inode, so a repeatedly-rebuilt binary
# dies with exit 137 at that inode while a byte-identical copy runs. Bytes are
# NOT the property under test: the defective code produced correct bytes.
# Pinned portably (no `#ifdef`, so the windows-msvc-release leg really runs it)
# via `create_hard_link` + `equivalent`, plus the fail-loud/no-residue belts for
# the temp-create, rename, missing-parent and filename-less failure paths and a
# concurrent-writers uniqueness pin.
dss_add_test(NAME link/test_link_writer_new_identity
             SOURCES test_link_writer_new_identity.cpp)

# D-LINK-WRITER-NOREPLACE-WIDE-PATH-UNSUPPORTED — the staging-temp claim must be
# EXCLUSIVE (`fopen` mode "x" = O_CREAT|O_EXCL): race-free against a concurrent
# process, and it STEPS OVER a stale `.dsstmp-*` left by a killed run instead of
# adopting and truncating it. Nothing pinned that — disabling the exclusive
# create left EVERY existing link-writer test green. That count is LEG-DEPENDENT
# (MEASURED): 19 on POSIX = 12 in test_link_writer.cpp + 7 in
# test_link_writer_new_identity.cpp; 18 on Windows, where
# LinkWriter.ExecutableOutputCarriesExecuteBitPosix is compiled out by
# `#ifndef _WIN32`. On either leg none of them touched exclusivity: each
# concurrent writer targets a DIFFERENT path, and no test ever pre-created a
# stale temp. The disable recipe is itself PER ARM — the two arms are genuinely
# different code, so one measurement cannot cover both — Windows: `L"wbx"` ->
# `L"wb"`; POSIX: drop `O_EXCL` from the `open` flags. Pinned by calling
# `linker::detail::createExclusiveBinary`
# DIRECTLY (fresh path opens; an existing path is refused and left byte-intact;
# the refusal has no side effect), because the property is invisible through
# `writeBytes` — its candidate names embed a pid and a process-wide counter. The
# closing test then reaches the retry/`exists()`-continue branch at the
# `writeBytes` level, and earns the right to pre-create candidate names by first
# requiring an exhaustion failure over exactly `detail::kMaxClaimAttempts`
# occupied slots — READ from writer.hpp, never hand-copied, so lowering the cap
# cannot silently vacate those pins. A zero-byte write covers the empty-span
# commit branch. The test file's header records which sub-assertions are
# host-specific: exclusivity is red-on-disable on BOTH arms, but "leaves it
# byte-intact" discriminates only on Windows (POSIX has no O_TRUNC here) and the
# directory refusal passes either way on both.
dss_add_test(NAME link/test_link_writer_exclusive_claim
             SOURCES test_link_writer_exclusive_claim.cpp)

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

# c164 (D-LK-RELOCATABLE-OBJECT-READER, ELF slice) — the ELF ET_REL
# relocatable-object MEMBER READER: the inverse of elf.cpp's ET_REL writer.
# Writer<->reader FULL-object round-trip (functions + data + externs +
# relocations {offset,target,kind,addend}), real gcc-produced .o cross-check
# (WSL), and header/section-table/symtab/rela truncation red-pins. Unblocks
# the c165 static-link (pull a member, read it into a mergeable module, merge).
dss_add_test(NAME link/test_relocatable_object_reader SOURCES test_relocatable_object_reader.cpp)

# c168 (D-LK-RELOCATABLE-OBJECT-READER-MACHO) -- the Mach-O MH_OBJECT
# relocatable-object MEMBER READER: the inverse of macho.cpp's MH_OBJECT writer
# (the arm64 sibling of the c164 ELF reader). Writer<->reader FULL-object
# round-trip (functions sliced by n_value + data by (segment,name) pair +
# externs + relocations {offset,target,kind,in-slot-addend}), truncation-at-
# every-length fuzz, the corruption red-pins (bad magic, MH_EXECUTE filetype,
# unknown reloc nativeId, r_extern=0 section-relative reloc), non-Mach-O schema
# fail-loud, and the x86_64 leaf-only agnosticism round-trip.
dss_add_test(NAME link/test_macho_object_reader SOURCES test_macho_object_reader.cpp)

# c170 (D-LK-RELOCATABLE-OBJECT-READER-COFF) -- the Windows COFF `.obj`
# relocatable-object MEMBER READER: the inverse of pe.cpp's Obj-arm writer
# (the PE/COFF sibling of the c164 ELF + c168 Mach-O readers). Writer<->reader
# FULL-object round-trip (functions sliced by sorted section-relative Value +
# data by name with the reloc-presence rodata-vs-relro disambiguation + externs
# + relocations {offset,target,kind,in-slot-addend}), truncation-at-every-length
# fuzz, the corruption red-pins (nonzero SizeOfOptionalHeader, unknown reloc
# nativeId, out-of-range SymbolTableIndex, out-of-range SectionNumber), non-PE
# schema fail-loud, and the DTYPE_FUNCTION extern isData inference.
dss_add_test(NAME link/test_coff_object_reader SOURCES test_coff_object_reader.cpp)

# D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED: the
# cross-format dependency-table pin. ONE two-library import set through the
# ELF, PE, and Mach-O writers; each format's real dependency table (DT_NEEDED
# / IMAGE_IMPORT_DESCRIPTOR.Name / LC_LOAD_DYLIB) is parsed back out of the
# emitted bytes and must name EVERY resolved library. The defect it guards
# survived because nothing compared the three writers on shared input.
dss_add_test(NAME link/test_image_library_deps
             SOURCES test_image_library_deps.cpp)

# D-FORMAT-MACHO-X86_64-STATICLIB-DECLARES-NO-EXTERN-CALL-DISPATCH — the CLASS,
# not the instance. Groups the 24 shipped `.format.json` files into families by
# stripping the ARCH token, then diffs each arm's declared ROOT-KEY SET against
# its siblings: an asymmetry must be justified at the site by a
# `$absentKeyRationale` entry, AND every such entry must still correspond to a
# real absence (both directions fail loud, so the table can neither be skipped
# nor rot). Three gaps of this exact shape shipped in three cycles because
# nothing ever compared a format against its own siblings; applying the method
# by hand found a fourth. Reads the JSON directly — the whole subject is which
# keys are DECLARED, which the loaded schema object can no longer tell you —
# hence the json link (the `test_c_symbol_decoration` precedent).
dss_add_test(NAME link/test_object_format_family_symmetry
             SOURCES test_object_format_family_symmetry.cpp)
target_link_libraries(dss_link_test_object_format_family_symmetry
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125) — the
# backend seam. Fail-closed resolution FIRST (a null backend that meant "skip
# the rules" would let all 24 formats validate clean while validating nothing),
# then a whole-family MUTATION PROBE: break each required identity field of each
# shipped format and demand a reject at that exact JSON pointer. Reads the
# shipped `.format.json` files and mutates them in memory, so the fixtures
# cannot drift from what we ship — hence the json link.
dss_add_test(NAME link/test_object_format_backend_registry
             SOURCES test_object_format_backend_registry.cpp)
target_link_libraries(dss_link_test_object_format_backend_registry
                      PRIVATE nlohmann_json::nlohmann_json)

# UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION): the `runtimeLibraries` ROLE TABLE's
# red-on-disable levers + the migration's mechanical exit criterion. Both levers are
# the C4 shape -- change CONFIG, recompile no C++, observe different EMITTED BYTES.
# The json link is for the `$`-prefixed OMISSION RATIONALE key: `ObjectFormatSchema`
# drops documentation keys on load, so the parsed schema cannot answer whether an
# EXCLUDED pe flavour still carries the written reason for its exclusion. Same
# precedent as test_object_format_family_symmetry / test_c_symbol_decoration.
dss_add_test(NAME link/test_runtime_library_roles
             SOURCES test_runtime_library_roles.cpp)
target_link_libraries(dss_link_test_runtime_library_roles
                      PRIVATE nlohmann_json::nlohmann_json)
