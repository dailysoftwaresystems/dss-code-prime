#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <string>

// Cross-tier extern symbol descriptor (LK6 cycle 2d substrate hoist
// — plan 14 §3.1 D-LK6-6). Carries the link-time-resolved identity
// of an external symbol referenced by an assembled / lowered module:
//
//   `symbol`      — the SymbolId that matches `Relocation::target`
//                   in every reloc that references this extern.
//   `mangledName` — the on-binary symbol name the linker's import
//                   table must carry verbatim (e.g. "printf" on
//                   Linux/ELF; "printf" on x86_64 PE; "_printf" on
//                   legacy Mach-O i386). Per-platform underscoring
//                   belongs upstream (plan 11 §2.5); MIR / LIR /
//                   assembler stamp whatever the HIR FfiMetadata
//                   provided.
//   `libraryPath` — the dynamic library that owns this symbol
//                   ("kernel32.dll" / "msvcrt.dll" on Windows;
//                   "libc.so.6" on Linux; "/usr/lib/libSystem.B.
//                   dylib" on macOS). Multiple externs sharing this
//                   field collapse to one PE
//                   IMAGE_IMPORT_DESCRIPTOR / ELF DT_NEEDED entry /
//                   Mach-O LC_LOAD_DYLIB load command. The linker
//                   groups by this field.
//
// Lives in `core/types` (alongside `SymbolId`) so HIR / MIR / LIR /
// assembler all consume the same row type without coupling to each
// other's headers. Pre-cycle-2d the type lived in `src/asm/asm.hpp`
// for assembler use only; the cycle 2d thread-through (HIR FfiMap
// → MIR pre-pass → MIR/LIR result side-tables → assembler) needs it
// upstream — hence the hoist (architect "no abstraction explosion"
// rule: hoist when 3+ tiers consume; we now have HIR / MIR / LIR /
// assembler / linker, well past the threshold).

namespace dss {

struct DSS_EXPORT ExternImport {
    SymbolId    symbol{};       // matches Relocation::target
    std::string mangledName;    // on-binary symbol name
    std::string libraryPath;    // owning dylib / DLL / SO
    // c82 (D-LK-EXTERN-DATA-IMPORT): true for an extern DATA object
    // (HIR ExternGlobal — e.g. libc's `stdout`, or a cross-TU
    // `extern const char sqlite3_version[];`), false for a function.
    // A data import that survives to the link tier (the LK11 merge
    // resolves sibling-CU-defined ones away first) binds per the
    // format's declared `dataImportBinding` model (c84: the ELF
    // ET_EXEC R_*_COPY copy-relocation); a format that declares no
    // model FAILS LOUD at the linker's pre-walker gate — a PLT
    // stub bound to a data symbol would be a silent miscompile.
    bool        isData = false;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): true for an extern DATA object
    // declared with thread storage duration (`extern thread_local int e;`).
    // Set at the HIR→MIR extern pre-pass from the declaration's
    // HirThreadLocalMap entry; carried through the LK11 merge's
    // survivingExterns copy. A same-program sibling-CU definition resolves
    // the row away like any extern data (the definition's own
    // MirGlobal.isThreadLocal drives layout); a TRUE library TLS import
    // surviving to the link tier is the initial-exec/GOT-indirect model —
    // NOT implemented (D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC) — and the
    // walker tier rejects it loud (slice C). Meaningless (false) for
    // function imports (S_ThreadLocalOnFunction rejects those upstream).
    bool        isThreadLocal = false;
    // c84 (D-LK-EXTERN-DATA-IMPORT): the imported DATA object's byte
    // size + alignment, DERIVED from the declared type's layout at
    // HIR→MIR (`computeLayout` under the active target's aggregate-
    // layout params + the format's DataModel — never hardcoded; a
    // `FILE*` object is the data model's pointer width). Consumed by
    // the ELF copy-relocation emitter: the exec reserves a `.bss`
    // slot of exactly this shape, exports the symbol with this
    // `st_size`, and the loader memcpy's `st_size` bytes from the
    // library's object. BOTH stay 0 when the declared type is
    // INCOMPLETE (`extern const char v[];`) — legal C for a cross-TU
    // extern the LK11 merge resolves against its defining sibling
    // CU; a TRUE library import that survives to the walker with
    // size 0 fails loud there (an unsized copy slot cannot be
    // reserved). Meaningless (0) for function imports.
    std::uint64_t dataSizeBytes  = 0;
    std::uint64_t dataAlignBytes = 0;
    // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED symbol version this
    // import must bind, as an ELF version STRING (e.g. "GLIBC_2.3"). EMPTY
    // (the default, every symbol until opted in) ⇒ UNVERSIONED: the ELF
    // writer stamps this import's `.gnu.version` slot with VER_NDX_GLOBAL (1)
    // and emits no `.gnu.version_r` requirement for it — byte-identical to the
    // pre-c156 image. NON-EMPTY ⇒ the ELF writer emits a `.gnu.version_r`
    // (verneed) requirement against this import's `libraryPath` naming this
    // version, and points the import's `.gnu.version` slot at it, so ld.so
    // binds the DEFAULT/declared version instead of misbinding an unversioned
    // reference to a library's OLDEST compat version (glibc `realpath` bound
    // `@GLIBC_2.2.5` — the NULL-buffer-rejecting compat form — instead of the
    // `@@GLIBC_2.3` default). CONFIG-DRIVEN + already resolved for the ACTIVE
    // (arch, format) upstream (the descriptor's per-target `version` variant):
    // the writer reads this string exactly as it reads `libraryPath` for
    // DT_NEEDED — no arch/format/symbol-name branch in the shared substrate.
    // Meaningless (stays empty) on formats that carry no symbol versioning
    // (PE/Mach-O ignore it). Rides the LK11 merge's whole-row copy for free.
    std::string   version;
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: TRUE ⇒ an EAGER import — a shipped-
    // library descriptor symbol (a `#include`d library export) DSS binds even
    // when UNREFERENCED (the D-FFI-DESCRIPTOR-EAGER-IMPORT invariant). The
    // linker's reference gate (`rejectOrDropUnreferencedExterns`) KEEPS an eager
    // row unconditionally; a NON-eager import (a source `extern` decl / bare-
    // proto synthesis) survives ONLY when a relocation references it — gcc's
    // "an unused extern declaration emits no import" rule, now uniform across
    // library-bound AND no-library rows. Set at HIR→MIR from
    // `FfiMetadata.isEagerImport`; rides the MIR merge's whole-row copy, and the
    // merge OR-COMBINES it when it collapses two rows — an eager `#include`d
    // symbol plus a hand-written non-eager `extern` yields an EAGER surviving
    // row, order-independent.
    //
    // ★ READ THE PRECONDITION ON THAT OR-COMBINE, because it is NOT "of the same
    // name" (this comment said so until TF-C119, and it was wrong). Both merge
    // tiers collapse on the FULL import identity — the (mangledName,
    // libraryPath, version) triple (`mir_merge.cpp::ffiImportKey`,
    // `linker.cpp`'s `dedupKey`). Two rows that share a NAME but disagree on the
    // owning library are two DIFFERENT dynamic symbols by construction, so they
    // do not fold, and nothing about them is OR-combined. The contract above
    // therefore holds only where the two PRODUCERS spell the same library:
    //   * the shipped descriptor's `library` map, e.g. src/dss-config/
    //     shippedLibs/stdio.json:5 → {"pe":"ucrtbase.dll", "elf":"libc.so.6",
    //     "macho":"/usr/lib/libSystem.B.dylib"};
    //   * the source-declared-extern default, src/dss-config/sources/
    //     c-subset.lang.json:1531 `externLibraryByFormat` →
    //     {"pe":"msvcrt.dll", "elf":"libc.so.6",
    //      "macho":"/usr/lib/libSystem.B.dylib"}.
    // On elf and macho those AGREE, so the fold happens and the contract reads
    // exactly as written. ⚠ ON pe THEY DIVERGE — `ucrtbase.dll` vs `msvcrt.dll`
    // — so on pe the two rows are, correctly per the key, two imports: a CU that
    // `#include <stdio.h>`s (⇒ the descriptor row, ucrtbase.dll) beside a sibling
    // CU that hand-declares `extern int puts(const char*);` without the header
    // (⇒ the source-extern default, msvcrt.dll) yields TWO surviving rows, TWO
    // IMAGE_IMPORT_DESCRIPTORs, and the hand-declaring CU's call bound into
    // msvcrt's copy — the split CRT the UCRT migration retired. Reachable
    // CROSS-CU only: same-TU the semantic tier SUPPRESSES the shipped row when the
    // user declares the name (semantic_analyzer.cpp:13324,13354) and forwards the
    // descriptor's library with it, so one TU yields one row.
    // ★ THIS DIVERGENCE IS A PRE-EXISTING UCRT-MIGRATION RESIDUAL, NOT SOMETHING
    // THE FOLD INTRODUCED: the two config values were already what they are, and
    // widening the key merely stopped a name-only key from HIDING the mismatch
    // behind a silent cross-library fold (which is itself the misbind
    // D-LK11-EXTERN-IMPORT-DEDUP exists to prevent). Reconciling the two pe
    // defaults belongs to D-FFI-PE-CRT-UCRT-MIGRATION, not to the dedup key.
    // The CURRENT behaviour is pinned by
    // `MirMerge.PeUcrtbaseAndMsvcrtRowsOfOneNameStayTwoImports` in
    // tests/mir/test_mir_merge.cpp so that changing either config value MOVES a
    // test rather than silently changing what a pe binary imports.
    //
    // INVARIANT:
    // isEagerImport ⟹ non-empty `libraryPath` (a descriptor always ships a
    // library); the flag never rides an unbound row. FALSE for every non-
    // descriptor producer (the format-AGNOSTIC default — no arch/format branch).
    bool          isEagerImport = false;
};

} // namespace dss
