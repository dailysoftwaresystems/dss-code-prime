#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <optional>
#include <span>

// Windows COFF `.obj` relocatable-object MEMBER READER -- cycle c170,
// anchor D-LK-RELOCATABLE-OBJECT-READER-COFF. The EXACT INVERSE of the
// Obj arm of `src/link/format/pe.cpp`'s `encode()`.
//
// This is the PE/COFF sibling of the c164 ELF ET_REL reader
// (`elf_object_reader.cpp`) and the c168 Mach-O MH_OBJECT reader
// (`macho_object_reader.cpp`, the CLOSEST template it mirrors): where the
// c159-c161 export-surface readers (`ffi/binary_readers/*`) read only what
// a binary EXPORTS, THIS reader reconstructs a relocatable object's FULL
// LINKABLE BODY back into an `AssembledModule` -- the exact structure the
// c154 cross-CU merge (`linker.cpp::mergeModules`) consumes. It is the read
// half that lets the c165 STATIC-LINK path pull a COFF `.obj` archive
// member, read it into a mergeable module, and merge it.
//
// Reconstruction map (the inverse of the writer, field by field). The
// COFF-vs-ELF/Mach-O inversions that distinguish it:
//   * (1) IMAGE_SYMBOL has NO size field (like Mach-O's nlist_64; unlike
//     ELF's st_size). Each section's `.text`/data body is sliced into
//     per-atom byte ranges by the SORTED `Value`s of its BODY-STARTING
//     defined symbols (see (7) for which those are) -- an atom spans
//     [value_k, next strictly-greater value) (the last runs to the
//     section's SizeOfRawData). A `.text` slice becomes an
//     `AssembledFunction`, a data slice an `AssembledData`. Because the
//     extent comes from the NEXT boundary rather than from the symbol, a
//     symbol wrongly left OUT of the boundary set does not shrink an atom
//     -- it silently merges its bytes into the previous one, or, when it
//     is the section's first symbol, loses them entirely. That is why (7)
//     is a correctness clause and not a naming convention.
//   * (2) IMAGE_SYMBOL.Value is ALREADY SECTION-RELATIVE (the offset within
//     the symbol's section -- NOT a flat `.o`-space address like Mach-O's
//     n_value, so there is NO `- section.addr` subtraction).
//   * (3) the section kind is resolved from the section NAME via the format
//     schema's rows (agnostic -- never a hardcoded `.text`/`.rdata`). COFF
//     has NO segment (unlike Mach-O's (segment,section) pair): the object
//     schema declares TWO rows named `.rdata` -- `rodata` (no relocations)
//     and `relro` (reloc-bearing const data). They are header-identical, so
//     the disambiguator is RELOC-PRESENCE: a `.rdata` section carrying its
//     own IMAGE_RELOCATION table resolves to the relro (RelRoConst) row, a
//     reloc-free one to the rodata row. This is the COFF analog of Mach-O's
//     segment-pair key -- and it is the SEMANTIC essence (relro IS "const
//     data that carries load-time relocations"), so a re-emission routes a
//     reloc-bearing const item to a section that permits relocations.
//   * (4) the reverse reloc map keys on the raw IMAGE_RELOCATION.Type ==
//     the schema's `nativeId`. There is NO `pltNativeId` variant on PE (an
//     extern call is a plain REL32 against the undefined symbol), and no
//     shipped PE document declares a `"isCall": true` row either -- COFF
//     x86_64 has no branch-only relocation, REL32 being both the call
//     displacement and the `lea rip+d` data displacement -- so
//     `callSignalNativeIds` is EMPTY; see the isData note.
//   * (5) each IMAGE_RELOCATION becomes a `Relocation{offset, target, kind,
//     addend}`. COFF has NO addend column (like Mach-O): a DATA-section
//     reloc's addend lives IN the patched slot bytes (widthBytes LE at
//     VirtualAddress -- the writer's in-place convention), while a `.text`
//     reloc carries addend 0 (the writer rejects a non-zero `.text`
//     addend). The target-schema `addendBias` is un-baked so a re-emission
//     re-adds it once (0 for the non-pcrel absolute kinds a data slot uses
//     -- a schema invariant).
//   * (6) COFF name mangling is IDENTITY on PE x64 (unlike Mach-O's leading
//     underscore): a symbol name is read VERBATIM. Names <= 8 bytes are
//     INLINE (NUL-padded) in the 8-byte field; longer names use the
//     `[u32 zero][u32 strtab-offset]` form pointing into the COFF string
//     table (a u32 size prefix + NUL-terminated names).
//   * (7) WHAT STARTS AN ATOM -- the correctness clause, and the one place
//     COFF must NOT be read as a Mach-O dialect
//     (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM).
//     A defined symbol is an atom BOUNDARY iff ANY of:
//       (a) it is EXTERNAL (IMAGE_SYM_CLASS_EXTERNAL);
//       (b) it is STATIC (IMAGE_SYM_CLASS_STATIC) and its derived type is
//           IMAGE_SYM_DTYPE_FUNCTION -- a file-local (`static`) function;
//       (c) it is STATIC and its section's kind resolved to something
//           other than Text -- a file-local DATA object, which COFF stamps
//           `notype` whatever its linkage, so the derived type cannot
//           speak for it and the SECTION does instead (a block label is a
//           CODE address; there are no interior labels in `.rdata`);
//       (d) failing all of those, no reconstructed atom covers its offset
//           -- the geometry fallback in `link/format/object_atom_coverage
//           .hpp`, which recovers a body no COFF field could name.
//     A file-local body differs from an external one only in WHO MAY SEE
//     IT, so every one of (b)-(d) takes `SymbolBinding::Local` (C internal
//     linkage: it must never satisfy another TU's extern, and
//     `resolveCrossCuDefs` enforces that by skipping Local) and NO COMDAT
//     Selection lift (that policy dedups BY NAME across objects, which
//     internal linkage has no part in). What is left over -- an interior
//     block label, a class-LABEL case target, a section-definition symbol
//     -- is a ModuleSymbol only, NOT a boundary, so it never splits the
//     item containing its interior offset. An UNDEFINED symbol
//     (SectionNumber == 0) becomes an `externImports` entry; an ABSOLUTE
//     (-1) / DEBUG (-2) symbol a bodiless ModuleSymbol.
//     ⓘ TWO boundaries at ONE section offset are ONE atom under SEVERAL
//     NAMES, never two byte-identical twins: the most externally-visible
//     name owns the body, every other name keeps its row but carries the
//     owner's SymbolId, and a relocation naming any of them binds to the
//     owner. Shared, not per-format --
//     `link/format/object_atom_coverage.hpp`,
//     D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS.
//     ⚠ This clause used to say the rule "mirrors the Mach-O
//     N_EXT-vs-local rule". IT NO LONGER DOES, and the divergence is the
//     point: COFF carries a per-symbol FUNCTION declaration on the wire
//     (✔MEASURED identical on DSS's own writer, MSVC cl.exe and mingw
//     gcc) while `macho.cpp` writes a local function and a block label as
//     the same bare N_SECT with no discriminator at all. Reading either
//     reader as the template for the other is what let whole file-local
//     functions be demoted here.
//     ⓘ (c) is what closes D-LK-COFF-ARCHIVE-MEMBER-READER-LOSES-STATIC-RODATA-SYMBOLS,
//     and it is the only clause that reaches a symbol in
//     TRAILING position: (d) sees nothing there, because the preceding
//     atom already runs to the section end and therefore covers it. So a
//     `static const` array after an exported one is placed by (c) or not
//     at all -- which is why (c) is a classification and not a fallback.
//     ⚠ NOT closed by any of them, and (c) CHANGED WHAT HAPPENS TO IT: an
//     ANONYMOUS region no symbol names -- a mingw-gcc `.rdata` jump table,
//     reached through the section symbol plus an addend (✔MEASURED at
//     `-O2`). Every clause above is symbol-anchored, so those bytes still
//     reconstruct no atom. If the section carries RELOCATIONS the reader
//     still refuses, now as "relocation ... lies in no reconstructed data
//     item" rather than as a symbol-coverage refusal. If it carries NONE,
//     the object now reads GREEN with the anonymous bytes dropped, where
//     before (c) it refused -- because the refusal was firing on the
//     file-local SYMBOL, never on the unnamed bytes it was standing in for.
//     That is the pre-existing D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS row
//     reached by one more shape: the same silent drop an EXTERNAL first
//     symbol has always produced, since the loss follows bytes being
//     unnamed and never the linkage of the symbol after them. Pinned both
//     ways in `test_coff_object_reader.cpp`.
//   * isData: COFF carries a function-type hint on the symbol
//     (IMAGE_SYM_DTYPE_FUNCTION in the derived-type bits), UNLIKE Mach-O,
//     whose nlist_64 has no such field and must therefore read the class
//     off a relocation the FORMAT declares `"isCall": true` on
//     (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL). That is why an empty
//     `callSignalNativeIds` is a REFUSAL there and a harmless no-op here. An
//     extern's `isData` is the canonical `(Type & DTYPE_MASK) !=
//     DTYPE_FUNCTION`. The DSS COFF writer EMITS this hint on function
//     externs (the c170 silent-failure-review fold -- pe.cpp mirrors the
//     defined-function DTYPE_FUNCTION path), so a DSS writer<->reader round
//     -trip preserves the function/data class LOSSLESSLY -- no silent DATA
//     default. A foreign extern with no derived-type set reconstructs DATA;
//     an extern that reaches the exec walker unresolved is rejected loud by
//     the linker's unbound-extern gate regardless, so isData never drives a
//     silent miscompile.
//
// Fail-loud discipline (mirrors the c164 ELF + c168 Mach-O readers): EVERY
// field is bounds-checked with the overflow-safe `rangeExceedsBuffer`
// shape; any structural violation (header short, a NON-zero
// SizeOfOptionalHeader -- i.e. a PE IMAGE, a link OUTPUT, not a
// relocatable input -- a section body / symtab / string table / reloc
// table past EOF, an unknown reloc nativeId, a duplicate nativeId, a
// SectionNumber out of range, a reloc SymbolTableIndex past the symbol
// table, a reloc whose site lies in no reconstructed atom, a section with
// relocations but no reconstructed atom) emits an `F_*` diagnostic and
// returns `nullopt`. There is never a silent partial reconstruction.
//
// SCOPE (c170): PE/COFF `.obj` x86_64 (the native cl.exe / link.exe static
// witness target). The reader is machine-agnostic in shape -- every reloc
// TYPE and section KIND flows from the schema, no hardcoded
// IMAGE_REL_AMD64_* / `.text` / machine identity. DSS's writer emits
// ZERO auxiliary symbol records, so a record's ordinal equals its symbol
// index; a foreign object's aux records are SKIPPED (not decoded as
// symbols) and a reloc naming an aux slot fails loud.

namespace dss::pe {

// Reconstruct an `AssembledModule` from a Windows COFF `.obj` relocatable
// object's raw bytes. Returns `nullopt` (and emits a diagnostic) on any
// structural / bounds / unknown-reloc failure.
//
// `objectFormatSchema` supplies the section-name -> SectionKind mapping and
// the reloc `nativeId` -> RelocationKind reverse map AND each row's declared
// `isCall` role (all config-driven -- no hardcoded PE numbers in the reader
// beyond the structural record layout the writer also hardcodes).
// `targetSchema` supplies each reloc kind's `widthBytes` (the in-slot data
// addend width) and `addendBias` (the addend un-bake) -- ARITHMETIC only. It
// no longer supplies the call/branch signal: that was `formulaKind`, and a
// formula describes arithmetic rather than role
// (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL).
//
// `cuId` stamps the reconstructed module's `CompilationUnitId` (the merge
// keys its symbol index by `(cuId, SymbolId)`); the c165 static-link path
// assigns a fresh one per pulled member. Defaults to the invalid sentinel
// for single-member / test callers.
//
// NOTE on `AssembledModule::ok()`: the reader sets `expectedFuncCount ==
// functions.size()` by construction, so `ok()` is a TAUTOLOGY for reader
// output AND returns FALSE for a legitimate data-only member (zero
// functions). Use the `nullopt` return, not `ok()`, as the read-success
// signal (the c165 merge iterates functions + dataItems directly and does
// NOT gate on `ok()`). This mirrors the ELF / Mach-O readers exactly.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
readRelocatableObject(std::span<std::uint8_t const> bytes,
                      TargetSchema const&            targetSchema,
                      ObjectFormatSchema const&      objectFormatSchema,
                      DiagnosticReporter&            reporter,
                      CompilationUnitId              cuId = {});

} // namespace dss::pe
