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

// Mach-O 64-bit relocatable (MH_OBJECT) MEMBER READER -- cycle c168,
// anchor D-LK-RELOCATABLE-OBJECT-READER-MACHO. The EXACT INVERSE of the
// MH_OBJECT arm of `src/link/format/macho.cpp`'s `encode()`.
//
// This is the Mach-O sibling of the c164 ELF ET_REL reader
// (`elf_object_reader.cpp`), which is the exact template it mirrors:
// where the c159/c160 export-surface readers (`ffi/binary_readers/
// macho_reader.cpp`) read only what a binary EXPORTS, THIS reader
// reconstructs a relocatable object's FULL LINKABLE BODY back into an
// `AssembledModule` -- the exact structure the c154 cross-CU merge
// (`linker.cpp::mergeModules`) consumes. It is the read half that lets
// the c165 STATIC-LINK path pull a Mach-O `.o` archive member, read it
// into a mergeable module, and merge it.
//
// Reconstruction map (the inverse of the writer, field by field). Six
// Mach-O-vs-ELF inversions distinguish it from the ELF reader:
//   * (1) `__text` is sliced into per-function byte ranges by the SORTED
//     `n_value`s of the defined N_SECT symbols that START AN ATOM --
//     Mach-O's nlist_64 carries NO size field (ELF's `st_size` slices
//     there), so each atom spans [n_value_k, n_value_{k+1}) (the last
//     runs to the section size). Each slice becomes an
//     `AssembledFunction` whose `symbol` is the symbol's symtab INDEX (a
//     per-CU identity; the merge matches by NAME via `ModuleSymbol`,
//     never by raw id).
//     ★ WHICH symbols start an atom is the format's OWN question, and it
//     is asked from two fields rather than from N_EXT alone
//     (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM):
//     when the mach_header declares MH_SUBSECTIONS_VIA_SYMBOLS, EVERY
//     defined N_SECT symbol starts an atom EXCEPT one carrying
//     N_ALT_ENTRY in `n_desc`; when it does not, the old narrower rule
//     stands (N_EXT starts an atom, a local does not). Apple's clang sets
//     the header flag on every ordinary `.o` and DSS's own writer now
//     emits the same pair, so the subsection arm is the live one for both
//     foreign archive members and DSS's own round trip. A promoted
//     file-local body is bound `SymbolBinding::Local`, so a `static`
//     function can never satisfy a sibling TU's extern.
//   * (2) `n_value` is a FLAT `.o`-space ADDRESS (not ELF's section
//     -relative `st_value`), so the section-relative offset is
//     `n_value - section.addr`.
//   * (3) the section kind is resolved from the (SEGMENT, SECTION) NAME
//     PAIR via the format schema's rows (agnostic -- never a hardcoded
//     `__text`). The two `__const` rows differ ONLY by segment
//     (rodata = `__TEXT,__const`, relro = `__DATA,__const`), so the pair
//     is the identity, not the section name alone.
//   * (4) the reverse reloc map keys on the PACKED nativeId
//     `r_info & 0xF7000000` (the r_type|r_length|r_pcrel bits, NOT
//     r_extern / r_symbolnum). There is NO `pltNativeId` variant on
//     Mach-O -- an extern call is BRANCH26/BRANCH against the same
//     nativeId whether or not ld64 synthesizes a stub -- so the "extern
//     is a function" signal is the FORMAT row's DECLARED `isCall` role
//     (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL), never a PLT-variant id and
//     never the target's arithmetic formula.
//   * (5) each `relocation_info` becomes a `Relocation{offset, target,
//     kind, addend}`. Mach-O has NO RELA addend column: a DATA-section
//     reloc's addend lives IN the patched slot bytes (widthBytes LE at
//     r_address -- the writer's in-place convention), while a `__text`
//     call/branch reloc carries addend 0 (the writer rejects a non-zero
//     __text addend). The target-schema `addendBias` is un-baked so a
//     re-emission re-adds it once (0 for the non-pcrel absolute kinds a
//     data slot uses -- a schema invariant).
//   * (6) DSS ALWAYS writes r_extern=1 (every reloc points at a symbol).
//     An r_extern=0 SECTION-INDEX relocation (foreign clang) FAILS LOUD
//     (anchor D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC) -- the section
//     -relative-redirect analog is a named follow-up; DSS never emits it.
//     ★ A FORMAT THAT DECLARES NO `isCall` ROW REFUSES rather than
//     guesses -- see the extern bullet below.
//   * SHN_UNDEF-equivalent N_UNDF symbols become `externImports`. Mach-O
//     carries NO STT_FUNC-style type hint, so `isData` is seeded DATA and
//     forced to false (function) ONLY when a relocation the FORMAT
//     declares `"isCall": true` on targets the extern (agnostic -- a
//     declared role read from the schema, no hardcoded reloc number and
//     no arch test). This is the same reloc-based inference the ELF
//     reader's STT_NOTYPE path uses. If the format declares no such row
//     AT ALL, an extern reached by any relocation FAILS LOUD: DATA would
//     be a silent guess, and a mis-typed import is a wrong answer rather
//     than a diagnostic (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL).
//   * defined function / data symbols become `ModuleSymbol` rows (name +
//     binding + visibility) for the merge's cross-CU name-matching.
//   * TWO defined symbols at ONE section offset are ONE atom under SEVERAL
//     NAMES, never two byte-identical twins: the most externally-visible
//     name owns the body, every other name keeps its row but carries the
//     owner's SymbolId, and a relocation naming any of them binds to the
//     owner. clang emits this on every ordinary `.o` (a section-start
//     `ltmp0` at the first function's offset). The rule is shared, not
//     per-format -- `link/format/object_atom_coverage.hpp`,
//     D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS.
//
// Fail-loud discipline (mirrors the c159-c161 readers + the c164 ELF
// reader): EVERY field is bounds-checked against the buffer with the
// overflow-safe `rangeExceedsBuffer` shape; any structural violation
// (header short, load command past `sizeofcmds`, a section body /
// symtab / strtab / reloc table past EOF, a non-MH_OBJECT filetype, an
// unknown reloc nativeId, an r_extern=0 reloc, a symbol section ordinal
// out of range, a relocation whose site lies in no reconstructed
// function/data item) emits an `F_*` diagnostic and returns `nullopt`.
// There is never a silent partial reconstruction.
//
// SCOPE (c168): Mach-O MH_OBJECT arm64 / x86_64 (the Apple `.a` static
// witness target). Foreign clang shapes are named follow-ups, NOT silent
// drops: an r_extern=0 section-relative relocation fails loud
// (D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC); anonymous data-section
// content owned by no symbol (string literals / jump tables reached via a
// section symbol) has no gap-atom reconstruction here -- a DSS-written
// `.o` covers every section byte with one named symbol per item, so it
// has NO gaps. Interior `&&label` block symbols round-trip as LOCAL
// `ModuleSymbol`s but do NOT split a function -- their interior-VA binding
// is a named follow-up (not needed to link whole functions). ⚠ Those are
// now identified by N_ALT_ENTRY, not by the absence of N_EXT: the two were
// conflated, and a whole file-local (`static`) function has the
// SAME nlist shape as a label, so the old rule dropped its bytes.
//   * SIZE-LESS-NLIST PADDING (D-LK-MACHO-MULTI-ITEM-SECTION-PADDING): with
//     TWO+ named items in ONE section (e.g. two rodata globals of differing
//     alignment packed into `__TEXT,__const`), the writer inserts alignment
//     PADDING between them and records each item's PADDED offset as its
//     n_value. Since nlist_64 has no size, an item's slice [off_k, off_{k+1})
//     ABSORBS the trailing inter-item padding into the preceding item -- the
//     reconstructed `bytes` are the real content followed by zero padding
//     (inflated size). This is VALUE-BENIGN: the padding is zero-filled, the
//     merge re-lays-out every item, and each item's real content sits at
//     offset 0 of its atom so every reference still resolves correctly (a
//     reloc only ever sits in the real content, never the trailing padding).
//     Byte-EXACT per-item reconstruction would need a per-item size Mach-O
//     does not carry -- the named follow-up. The value-correctness is pinned
//     by a multi-item round-trip test.

namespace dss::macho {

// Reconstruct an `AssembledModule` from a Mach-O 64-bit MH_OBJECT
// relocatable object's raw bytes. Returns `nullopt` (and emits a
// diagnostic) on any structural / bounds / unknown-reloc failure.
//
// `objectFormatSchema` supplies the (segment, section)-name -> SectionKind
// mapping, the reloc `nativeId` -> RelocationKind reverse map AND each
// row's declared `isCall` role -- the call/branch "is a function" signal
// (all config-driven -- no hardcoded Mach-O numbers in the reader beyond
// the structural record layout the writer also hardcodes). `targetSchema`
// supplies each reloc kind's `widthBytes` (the in-slot data addend width)
// and `addendBias` (the addend un-bake) -- ARITHMETIC only. It no longer
// supplies the call signal: that was `formulaKind`, and a formula describes
// arithmetic rather than role (D-LK-MACHO-ISDATA-NO-CALL-SIGNAL).
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
// NOT gate on `ok()`). This mirrors the ELF reader's contract exactly.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
readRelocatableObject(std::span<std::uint8_t const> bytes,
                      TargetSchema const&            targetSchema,
                      ObjectFormatSchema const&      objectFormatSchema,
                      DiagnosticReporter&            reporter,
                      CompilationUnitId              cuId = {});

} // namespace dss::macho
