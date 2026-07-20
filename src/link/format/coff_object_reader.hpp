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
//     per-atom byte ranges by the SORTED `Value`s of its DEFINED symbols --
//     an atom spans [value_k, next strictly-greater value) (the last runs
//     to the section's SizeOfRawData). A `.text` slice becomes an
//     `AssembledFunction`, a data slice an `AssembledData`.
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
//     extern call is a plain REL32 against the undefined symbol), and PE
//     x86_64 declares NO call-branch-formula reloc (REL32 is `linear`), so
//     `callSignalNativeIds` is EMPTY -- see the isData note.
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
//   * (7) an EXTERNAL (IMAGE_SYM_CLASS_EXTERNAL) defined symbol is an atom
//     BOUNDARY; a STATIC (IMAGE_SYM_CLASS_STATIC) defined symbol is a
//     ModuleSymbol only -- an interior `&&label` / jump-table block label,
//     NOT a boundary (mirrors the Mach-O N_EXT-vs-local rule). An UNDEFINED
//     symbol (SectionNumber == 0) becomes an `externImports` entry; an
//     ABSOLUTE (-1) / DEBUG (-2) symbol a bodiless ModuleSymbol.
//   * isData: COFF carries a function-type hint on the symbol
//     (IMAGE_SYM_DTYPE_FUNCTION in the derived-type bits), UNLIKE Mach-O
//     x86_64 (whose reloc formula cannot distinguish a call from a data
//     reference -- the c168 D-LK-MACHO-ISDATA-NO-CALL-SIGNAL fold). An
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
// the reloc `nativeId` -> RelocationKind reverse map (both config-driven --
// no hardcoded PE numbers in the reader beyond the structural record layout
// the writer also hardcodes). `targetSchema` supplies each reloc kind's
// `widthBytes` (the in-slot data addend width), `addendBias` (the addend
// un-bake), and `formulaKind` (the call/branch "is a function" signal, if
// any).
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
