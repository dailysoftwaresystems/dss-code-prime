# Linker unit tests (plan 14 LK4 substrate).

dss_add_test(NAME link/test_object_format_schema SOURCES test_object_format_schema.cpp)
# The closed-key-block table splices ONE extra key into a shipped
# `.format.json` at each block whose vocabulary is closed, so the fixtures are
# the real documents rather than copies that can drift — hence the json link
# (the `test_c_symbol_decoration` precedent just below).
target_link_libraries(dss_link_test_object_format_schema
                      PRIVATE nlohmann_json::nlohmann_json)

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

# D-LK-MACHO-STB-LOCAL-LD64-RUNTIME-WITNESS — the ld64 half of
# D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION. Two DSS-emitted
# Mach-O relocatable objects, each defining the SAME internal-linkage `_sym_7`,
# linked by Apple's own linker and RUN. `test_macho_writer.cpp`'s n_type pin
# proves DSS writes bare N_SECT; only ld64 can prove ld64 accepts it, and the
# exit code (20 from TU A's helper + 22 from TU B's) additionally discriminates
# "kept module-private" from "silently folded into one".
#
# TWO TIERS IN ONE FILE, deliberately sharing one object builder: a
# host-independent nlist+relocation structural pin that runs on EVERY leg (the
# always-on guard, the `test_macho_arm64_exit` precedent) plus the Darwin-only
# runtime witness. The runtime tier never skips silently — it records an
# `ArmVerdict` (structural off-Darwin, ENVIRONMENTAL when a Mac has no linker,
# which DSS_STRICT_ARM_VERDICTS=1 promotes to a red) and prints the ledger line
# on every host.
dss_add_test(NAME link/test_macho_ld64_local_collision
             SOURCES test_macho_ld64_local_collision.cpp)
dss_add_test(NAME link/test_macho_chained_fixups SOURCES test_macho_chained_fixups.cpp)
dss_add_test(NAME link/test_string_table         SOURCES test_string_table.cpp)
dss_add_test(NAME link/test_elf_exec_writer      SOURCES test_elf_exec_writer.cpp)

# Plan 15 DB4 -- the DWARF `.eh_frame` CIE/FDE + `.eh_frame_hdr` encoder.
# Includes the load-bearing REFUSAL when a target declares no DWARF register
# numbering (emitting the hardware encoding instead names a DIFFERENT
# register and walks the wrong frame) and an object-level oracle witness.
dss_add_test(NAME link/test_dwarf_cfi            SOURCES test_dwarf_cfi.cpp)

# The ELF WIRING of that encoder: `.eh_frame` + `.eh_frame_hdr` sections and
# the PT_GNU_EH_FRAME segment. ★ Carries the permutation pin — the CIE's CFA
# base register must be DWARF 7 for `%rsp`, whose DSS ordinal AND x86 hardware
# encoding are both 4, so reading either neighbouring field emits a table every
# reader accepts and every unwinder follows into the wrong frame.
dss_add_test(NAME link/test_elf_eh_frame         SOURCES test_elf_eh_frame.cpp)

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
# D-LK-MACHO-ISDATA-NO-CALL-SIGNAL case 5c parses a shipped `.format.json`
# directly so its "shipped file MINUS exactly one key" fixture is copied
# VERBATIM and cannot drift from the real document — hence the json link (the
# `test_c_symbol_decoration.cpp` precedent).
target_link_libraries(dss_link_test_macho_object_reader
                      PRIVATE nlohmann_json::nlohmann_json)

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
# The emission-alias pin splices ONE extra `relocations[]` row into a shipped
# `.format.json`, deriving the aliased wire id and a free `kind` FROM the
# document rather than hardcoding either — hence the json link, matching the
# Mach-O twin above.
target_link_libraries(dss_link_test_coff_object_reader
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM -- the
# ATOM-COVERAGE guard shared by all three relocatable-object readers
# (`link/format/object_atom_coverage.hpp`). The COFF/Mach-O readers demote every
# non-external defined section symbol to a bodiless ModuleSymbol, which is right
# for an `&&label` and wrong for a file-local (`static`) function; a CALLED one
# failed loud downstream, an UNCALLED one linked GREEN with its bytes dropped.
# The pins assert SIZES rather than success -- "a green read accounts for every
# byte of the section" is the assertion that sees the defect, because the link
# succeeding was the problem. Also pins ELF (unaffected -- st_size makes a local
# function an atom) and the interior block label that must NOT trip the guard.
dss_add_test(NAME link/test_object_atom_coverage SOURCES test_object_atom_coverage.cpp)

# D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT (OPEN,
# HIGH) -- the OBJECT-vs-IMAGE relocation-vocabulary census. `readArchiveMemberModule`
# reads a member against the FINAL IMAGE's format, which is harmless only because
# most families' object and image vocabularies happen to declare the same
# `nativeId` per `kind`. This derives the census from the shipped configs at run
# time and pins the divergent set EXACTLY, so the one family where the
# coincidence already fails cannot become two without a red. It pins a KNOWN
# DEFECT'S BLAST RADIUS, not desired behaviour -- a red here may mean the defect
# was REPAIRED (see the file header).
dss_add_test(NAME link/test_archive_member_format_vocabulary
             SOURCES test_archive_member_format_vocabulary.cpp)

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

# D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO (Mach-O half) — the
# `__TEXT,__eh_frame` writer. The DWARF encoder (test_dwarf_cfi) was complete
# and wired into NO writer, so a DSS Mach-O image carried no unwind table at
# all. Pins the section's existence/placement inside __TEXT, `__TEXT.nsects`
# against the section_64 records its own cmdsize reserves (the invariant a
# hand-maintained count breaks), every FDE's pcrel `initial_location` resolving
# to the function it describes (a desync loads fine and walks the wrong frame),
# the DWARF-vs-hardware register numbering on x86_64 where the two genuinely
# differ, the no-CFI control that must emit NO section, and the __got early/late
# VA congruence a __TEXT-appended section would otherwise slide.
dss_add_test(NAME link/test_macho_eh_frame       SOURCES test_macho_eh_frame.cpp)

# D-LK-LINKER-MERGE-DROPS-EVERY-ALIAS-ROW — the LK11b cross-CU merge vs a
# definition carrying SEVERAL NAMES (`__attribute__((weak, alias(...)))`, and what
# all three relocatable-object readers emit). The merge answered every question
# through the CANONICAL row: it deduped `combined.symbols` on the merged SymbolId
# alone (dropping every alias name from `.symtab` / `.dynsym` / the export tables),
# minted one merged id per NAME rather than per KEY (leaving `nameToId` pointing at
# ids no definition carries, so a losing sibling's relocations retargeted onto
# nothing), and decided whether BYTES survive from the canonical name's fate (so an
# atom that lost its canonical name while WINNING an alias name was deleted).
# Every pin runs a REAL multi-module link through a shipped format and reads the
# property back out of the EMITTED BYTES — three tiers (ET_EXEC `.symtab`, ET_DYN
# `.dynsym`, ET_REL `.symtab`), because the merge feeding the carrier and a writer
# reading it are separable and a pin on one alone goes green while the other loses
# the name.
dss_add_test(NAME link/test_cross_cu_alias_merge
             SOURCES test_cross_cu_alias_merge.cpp)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET — the linker tier's
# one site of the class: `linker::link`'s exec-flavored refusal
# (K_FormatLacksProcessExit) ended in a REMEDY that typed the `processExit`
# mechanism vocabulary into the sentence while `kExitMechanismTable` decides what
# is accepted. The pin walks that TABLE and requires the message to name every
# declarable row and no refused sentinel — never today's sentence, which would be
# the same retyping one level up.
dss_add_test(NAME link/test_linker_diagnostic_vocabulary
             SOURCES test_linker_diagnostic_vocabulary.cpp)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET — the OBJECT-FORMAT
# tier: the JSON loader, its JSON-free `validate()` twin, and the five backends.
# Every "expected one of ..." half now renders `dss::detail::renderAllowedList`
# over the table the check consults, and these pins assert the two name the SAME
# set in BOTH directions (completeness + honesty), driven from the table so
# adding a row without touching the message reds the test.
# ✔The class had ALREADY FIRED here, twice: `/sections/{}/kind` advertised 11 of
# `kSectionKindTable`'s 16 rows while the shipped corpus DECLARES five of the
# missing ones (shstrtab 10 rows, relro 10, tdata 5, tbss 4, tvars 2), and
# `/elf/osabi` accepted seven spellings while advertising six (`linux`, an
# ELFOSABI_GNU alias, was accepted and denied at once).
# It also closes a second-owner gap nothing pinned: `format.kind` is resolved by
# the BACKEND REGISTRY, not by `kObjectFormatKindTable`, and no test tied the two
# together — `FormatKindRefusalNamesEveryRegisteredBackend` drives the assertion
# from `objectFormatBackendTable()`.
# The fixtures are the SHIPPED `.format.json` files mutated in memory, so they
# cannot drift from what we ship — hence the json link.
# ⚠ MUST run through ctest, never a bare .exe: the shipped-config resolver walks
# the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME link/test_object_format_vocabulary_projection
             SOURCES test_object_format_vocabulary_projection.cpp)
target_link_libraries(dss_link_test_object_format_vocabulary_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET, the two halves
# that meet in the link tier: a BLOCK-SHAPE sentence (which advertises a KEY set
# and drifts exactly as a value refusal does -- `tlsAccess` advertised 3 of its
# 4 keys, `librarySynthesis` advertised a key that is not one at all), and
# `externCallDispatch`, whose accepted set was retyped in THREE tiers: the format
# loader that owns it, entry_trampoline.cpp and mir_to_lir.cpp. The two lowering
# messages END IN AN INSTRUCTION to declare it in the format JSON, so a stale
# one tells an author to write a value the loader refuses -- which is why the
# honesty arm here loads EVERY advertised spelling through the real loader.
# Mutations edit the SHIPPED format documents in memory (nlohmann consumer),
# never a hand-written format-like fixture that could rot against the real file.
# MUST run through ctest, never a bare .exe: the shipped-config resolver walks
# the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME link/test_extern_dispatch_and_block_shape_projection
             SOURCES test_extern_dispatch_and_block_shape_projection.cpp)
target_link_libraries(dss_link_test_extern_dispatch_and_block_shape_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED: the CONFIG half of the
# weak-DEFINITION dialect row -- the closed vocabulary, its loader, the
# JSON-free validate() twin, and the corpus pin that keeps the key declared
# ONLY where a walker consults it (a key the writer does not READ is worse
# than no key: it drifts silently and reads as authoritative). The WRITER half
# -- the pins proving the COFF .obj arm actually reads it -- lives in
# test_pe_writer.cpp, beside the encoder.
# Mutations edit the SHIPPED pe object document in memory (nlohmann consumer),
# never a hand-written format-like fixture that could rot against the real file.
# MUST run through ctest, never a bare .exe: the shipped-config resolver walks
# the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME link/test_weak_definition_dialect
             SOURCES test_weak_definition_dialect.cpp)
target_link_libraries(dss_link_test_weak_definition_dialect
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE:
# `__LD,__compact_unwind` -- the section Apple's clang emits BY DEFAULT, which
# had no `SectionKind` and so made DSS refuse EVERY stock macOS relocatable
# object. Reads two checked-in STOCK Apple-clang objects (arm64 + x86_64, built
# by `/usr/bin/clang -c` with NO flags on real Apple Silicon), and mutates the
# SHIPPED format document in memory (nlohmann consumer) rather than standing up
# a hand-written format-like fixture that could rot against the real file.
# MUST run through ctest, never a bare .exe: the config-level mutants read the
# shipped document off disk, and the resolver walks the cwd unless
# `DSS_CONFIG_ROOT` is set -- only `dss_add_test` sets it.
dss_add_test(NAME link/test_macho_compact_unwind_section
             SOURCES test_macho_compact_unwind_section.cpp)
target_link_libraries(dss_link_test_macho_compact_unwind_section
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE: a function
# merged from a foreign object used to arrive in a DSS image with NO unwind
# description, in SILENCE on ELF. Reads the two checked-in REAL `gcc -c`
# objects (x86_64 + aarch64, default flags -- which is what makes them carry
# `.eh_frame`), decodes their DWARF call-frame information back into the
# neutral `CfiFunction` vocabulary, and ROUND-TRIPS it through the shipped
# `buildEhFrame` writer. Mutates the SHIPPED format document in memory
# (nlohmann consumer) rather than standing up a hand-written format-like
# fixture that could rot against the real file.
# MUST run through ctest, never a bare .exe: the config-level mutant reads the
# shipped document off disk, and the resolver walks the cwd unless
# `DSS_CONFIG_ROOT` is set -- only `dss_add_test` sets it.
dss_add_test(NAME link/test_merged_foreign_unwind
             SOURCES test_merged_foreign_unwind.cpp)
target_link_libraries(dss_link_test_merged_foreign_unwind
                      PRIVATE nlohmann_json::nlohmann_json)
