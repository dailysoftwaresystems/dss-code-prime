#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"  // SymbolBinding

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
    // format's declared `dataImportBinding` model (every image
    // format declares "got-indirect" — a loader-bound pointer slot
    // holding the library object's ADDRESS); a format that declares no
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
    // D-LK-EXTERN-DATA-IMPORT: the imported DATA object's byte size +
    // alignment, DERIVED from the declared type's layout at HIR→MIR
    // (`computeLayout` under the active target's aggregate-layout
    // params + the format's DataModel — never hardcoded; a `FILE*`
    // object is the data model's pointer width). BOTH stay 0 when the
    // declared type is INCOMPLETE (`extern const char v[];`) — legal C
    // for a cross-TU extern the LK11 merge resolves against its
    // defining sibling CU. Meaningless (0) for function imports.
    // ★ NO EMITTER CONSUMES THESE, and that is a deliberate END STATE,
    // not an oversight. They sized the ELF copy-relocation `.bss` slot
    // (the exec reserved storage of exactly this shape and exported the
    // symbol with this `st_size`); that mechanism is DELETED
    // (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET) because
    // claiming a name of an ALIASED libc object split the object
    // silently. A got-indirect slot holds an ADDRESS, so the object's
    // own size is irrelevant to it — an incomplete `extern char v[];`
    // binds fine, and the walker no longer rejects a surviving size-0
    // import. What the fields still DO is witness the DECLARED shape,
    // so the two merge tiers can fail loud when two CUs declare
    // DIFFERENT objects under one external name.
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
    // therefore holds only where the two PRODUCERS spell the same library.
    //
    // ★★ UCRT-P4 (Decision 1) REDUCED THAT TO ONE PRODUCER, AND THAT IS THE POINT.
    // There used to be two independent owners of "which image owns this name":
    //   * the shipped descriptor's per-format `library` map (e.g.
    //     src/dss-config/shippedLibs/stdio.json → {"pe":"ucrtbase.dll",
    //     "elf":"libc.so.6", "macho":"/usr/lib/libSystem.B.dylib"}), and
    //   * a per-LANGUAGE `externLibraryByFormat` default in the `.lang.json`,
    //     whose pe entry named the LEGACY `msvcrt.dll`.
    // On elf and macho those agreed; ON pe THEY DIVERGED, so a CU that
    // `#include <stdio.h>`d (⇒ ucrtbase.dll) beside a sibling CU that hand-declared
    // `extern int puts(const char*);` without the header (⇒ the language default,
    // msvcrt.dll) produced TWO surviving rows, TWO IMAGE_IMPORT_DESCRIPTORs, and the
    // hand-declaring CU's call bound into msvcrt's copy — a SPLIT CRT in one image.
    // ⇒ THE LANGUAGE DEFAULT HAS BEEN REMOVED, not repointed. A user declaration
    // carries the SIGNATURE; the PLATFORM (the descriptor corpus, per format) carries
    // the REALIZATION, so a hand-written prototype and an `#include`d one now resolve
    // through the SAME descriptor row and produce a BYTE-IDENTICAL import. Repointing
    // the default at ucrtbase would have been the workaround, and a lethal one:
    // ucrtbase exports no bare `printf`, so the flip would have turned a
    // wrong-but-loadable msvcrt import into an unresolvable one — 0xC0000139 at the
    // LOAD of every such binary. Removing the field's authority is the fix.
    //
    // ⇒ WHAT REMAINS REACHABLE, and why the wide key still earns its keep: two
    // CONFIG-DECLARED images can still legitimately own one name — a per-SYMBOL
    // `library` override routes a single name off its header's default image (pe
    // `strftime`→ucrtbase while the rest of <time.h> stays elsewhere). Both of the
    // examples that used to stand here have since been retired — UCRT-P5 moved the
    // last one, `setjmp.json`, off msvcrt — but the KEY is not thereby redundant:
    // the mechanism stays reachable by config, and a corpus that happens to name
    // one image today is not the same fact as a key that can only ever express
    // one. Those are DECLARED
    // divergences, and the (mangledName, libraryPath, version) key keeps them two
    // distinct dynamic symbols instead of silently folding them — which is the
    // misbind D-LK11-EXTERN-IMPORT-DEDUP exists to prevent. What is gone is the
    // UNDECLARED divergence that came from a second owner guessing.
    // Pinned by `MirMerge.TwoConfigDeclaredImagesOwningOneNameStayTwoImports` in
    // tests/mir/test_mir_merge.cpp, so changing a declared image MOVES a test rather
    // than silently changing what a pe binary imports.
    //
    // INVARIANT:
    // isEagerImport ⟹ non-empty `libraryPath` (a descriptor always ships a
    // library); the flag never rides an unbound row. FALSE for every non-
    // descriptor producer (the format-AGNOSTIC default — no arch/format branch).
    bool          isEagerImport = false;
    // ★★ D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE: the REFERENCE binding
    // this import carries onto the wire — the import-side companion of
    // `ModuleSymbol::binding`, in the SAME agnostic `SymbolBinding` vocabulary so
    // the two sides of one symbol are never described in two languages.
    //
    // `Global` (the default, and every import until one is annotated) ⇒ a STRONG
    // reference: the symbol MUST be resolved, and nothing resolving it is a link
    // error. `Weak` ⇒ the reference MAY legally resolve to NOTHING, in which case
    // its address is 0 — which is the whole purpose of the construct (`extern int
    // ea __attribute__((weak)); … if (&ea)`), and the ONE property that
    // distinguishes it. Set at HIR→MIR's `collectExterns` from the declaration's
    // `HirLinkageMap` entry, exactly as `isThreadLocal` is set from
    // `HirThreadLocalMap` at the same site.
    //
    // ★ WHY THIS FIELD EXISTS AT ALL, because the attribute was already
    // understood without it. `weak` on an extern IMPORT reached the HIR linkage
    // map and STOPPED: HIR→MIR consumed `linkageMap` for function DEFINITIONS and
    // GLOBALS only, so the bit was parsed, recorded, and dropped one layer below
    // where it was recorded. The emitted object then marked the undefined symbol
    // STRONG on all three formats (✔MEASURED at HEAD 2026-09-02: ELF `NOTYPE
    // GLOBAL UND`, Mach-O `(undefined) external`, COFF `StorageClass: External`),
    // and a DSS-linked image refused the program outright with `K_SymbolUndefined`
    // where gcc and clang link it and run it to the null branch.
    //
    // ⚠ `Local` IS NOT A REPRESENTABLE IMPORT BINDING and never rides this field.
    // An import is by construction a name this object does NOT define, and
    // module-private is the one thing such a name cannot be — no format spells an
    // undefined LOCAL symbol, and emitting one would make the reference
    // unresolvable by any linker. `collectExterns` REFUSES it at the source span
    // rather than folding it to Global, so a language whose config maps some
    // specifier to `Local` on an extern declaration fails loud at the tier that
    // can still name the declaration.
    //
    // ★ THE MERGE COMBINE IS STRONGEST-WINS, NOT OR-COMBINE. Where two CUs import
    // one identity (the `ffiImportKey` / `dedupKey` triple) and disagree, the
    // surviving row binds `Global`: a strong reference anywhere in the program
    // makes the symbol REQUIRED, which is what C says and what gcc/clang do.
    // Order-INDEPENDENT, like `isEagerImport`'s OR — and, unlike `isData`, a
    // disagreement here is a DEFINED fold rather than a conflict, because the two
    // rows describe the same object bound the same way and differ only in whether
    // this TU could do without it.
    SymbolBinding binding = SymbolBinding::Global;
};

} // namespace dss
