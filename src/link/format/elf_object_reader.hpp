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

// ELF64 relocatable (ET_REL) MEMBER READER -- cycle c164, anchor
// D-LK-RELOCATABLE-OBJECT-READER (ELF slice). The EXACT INVERSE of the
// ET_REL arm of `src/link/format/elf.cpp`'s `encode()`.
//
// Where the c159/c160 export-surface readers (`ffi/binary_readers/
// elf_reader.cpp`) read only what a binary EXPORTS, and the c161 `ar`
// reader (`ffi/binary_readers/ar_reader.cpp`) reads only the archive
// symbol-index, THIS reader reconstructs a relocatable object's FULL
// LINKABLE BODY back into an `AssembledModule` -- the exact structure
// the c154 cross-CU merge (`linker.cpp::mergeModules`) consumes. It is
// the read half that unblocks the c165 STATIC-LINK (pull an archive
// member, read it into a mergeable module, merge it).
//
// Reconstruction map (the inverse of the writer, field by field):
//   * `.text` is sliced into per-function byte ranges by the STT_FUNC
//     symbols' (st_value, st_size); each slice becomes an
//     `AssembledFunction` whose `symbol` is a fresh SymbolId (the
//     symbol's `.symtab` INDEX -- a per-CU identity; the merge matches
//     by NAME via `ModuleSymbol`, never by raw id).
//   * `.data` / `.rodata` / `.bss` / `.data.rel.ro` defined OBJECT
//     symbols become `AssembledData` items. The section kind is recovered
//     from the section NAME via the format schema's rows (agnostic, never a
//     hardcoded `.rodata`), with a longest-schema-name-prefix + sh_flags
//     fallback so a `-ffunction-sections`/`-fdata-sections` distro member's
//     `.text.<fn>` / `.rodata.str1.1` / `.data.rel.ro.local` / TLS
//     `.tdata`/`.tbss` body is recovered, never silently dropped.
//   * SHN_UNDEF symbols become `externImports` (the mangled name is the
//     symbol name). `isData` is seeded from the symtab type (STT_FUNC ->
//     false, STT_OBJECT -> true, STT_NOTYPE -> data by default) and forced
//     to false (function) when a CALL/BRANCH-class reloc targets it -- the
//     x86_64 PLT-variant native id AND the aarch64 CALL26 branch formula
//     (agnostic; no hardcoded reloc number). This correctly types an
//     address-taken extern FUNCTION (called AND `&fn`-referenced) and every
//     aarch64 extern call (aarch64 declares no PLT-variant id).
//   * each RELA entry becomes a `Relocation{offset, target, kind,
//     addend}`: the ELF `r_type` (the low 32 bits of r_info) is mapped
//     back to the universal `RelocationKind` through the format schema's
//     `nativeId`/`pltNativeId` rows, and the psABI `r_addend` has the
//     target schema's `addendBias` UN-BAKED (subtracted) so the
//     reconstructed `Relocation::addend` is DSS-native again -- the
//     inverse of the writer's `rel.addend + addendBias`. Re-emitting the
//     read module thus re-adds the bias exactly once.
//   * defined function / data / section symbols become `ModuleSymbol`
//     rows (name + binding + visibility) for the merge's cross-CU
//     name-matching.
//
// Fail-loud discipline (mirrors the c159-c161 readers): EVERY field is
// bounds-checked against the buffer with the overflow-safe
// `rangeExceedsBuffer` shape; any structural violation (header short,
// section table past EOF, symtab/strtab/rela past EOF, an unknown reloc
// type, a relocation whose site lies in no reconstructed function/data
// item) emits an `F_*` diagnostic and returns `nullopt`. There is never
// a silent partial reconstruction.
//
// SCOPE (c164): ELF ET_REL x86_64 / aarch64 (the reference format --
// Linux `.a` is the primary static witness target, WSL-runnable). Mach-O
// MH_OBJECT and COFF are named follow-up anchors
// (D-LK-RELOCATABLE-OBJECT-READER-MACHO and
// D-LK-RELOCATABLE-OBJECT-READER-COFF). Non-producer metadata
// sections (`.eh_frame`, `.comment`, `.note.*`) and their relocations are
// NOT reconstructed (the `AssembledModule` model has no representation
// for them; DSS's own writer emits none) -- a documented boundary, not a
// silent drop. Interior block symbols (computed-goto `&&label`) round-trip
// as LOCAL NOTYPE text symbols but their `blockSymbols` interior-VA
// binding is a named follow-up (not needed to link whole functions).

namespace dss::elf {

// Reconstruct an `AssembledModule` from an ELF64 ET_REL relocatable
// object's raw bytes. Returns `nullopt` (and emits a diagnostic) on any
// structural / bounds / unknown-reloc failure.
//
// `objectFormatSchema` supplies the section-name -> SectionKind mapping
// and the reloc `nativeId`/`pltNativeId` -> RelocationKind reverse map
// (both config-driven -- no hardcoded ELF numbers in the reader beyond
// the structural record layout the writer also hardcodes). `targetSchema`
// supplies each reloc kind's `addendBias` for the addend un-bake.
//
// `cuId` stamps the reconstructed module's `CompilationUnitId` (the merge
// keys its symbol index by `(cuId, SymbolId)`); the c165 static-link path
// assigns a fresh one per pulled member. Defaults to the invalid sentinel
// for single-member / test callers.
//
// NOTE on `AssembledModule::ok()`: the reader sets `expectedFuncCount ==
// functions.size()` by construction, so `ok()` is a TAUTOLOGY for reader
// output (it is the assembler's parallel-index success check, meaningless
// here) -- AND it returns FALSE for a legitimate data-only member (zero
// functions). A c165 driver MUST therefore consume the returned module via
// the merge (`mergeModules` iterates functions + dataItems directly and does
// NOT gate on `ok()`), never gate emission on `ok()`. Use the `nullopt`
// return, not `ok()`, as the read-success signal.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
readRelocatableObject(std::span<std::uint8_t const> bytes,
                      TargetSchema const&            targetSchema,
                      ObjectFormatSchema const&      objectFormatSchema,
                      DiagnosticReporter&            reporter,
                      CompilationUnitId              cuId = {});

} // namespace dss::elf
