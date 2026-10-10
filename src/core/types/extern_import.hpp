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

// ★★ WHERE AN IMPORT's CODE-vs-DATA CAME FROM
// (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL, P68 round 9).
// A C declaration states its kind, and so does a `.s` CALL: a call target is
// code. A `.s` ADDRESS operand or data slot (`leaq x(%rip)`, `.quad x`) naming
// a symbol the file does not define states NOTHING. gas records the name alone
// (✔MEASURED 2026-09-23: the same R_X86_64_PC32 for a datum and a function),
// and ld takes the kind from the DEFINITION it finds. So does DSS. An object's
// untyped undefined symbol that no call relocation names states nothing either
// (gcc, clang and DSS write every undefined ELF symbol STT_NOTYPE, and a
// Mach-O nlist has no type at all), so since P69 the object readers mint it the
// same way (D-LK-MEMBER-UNTYPED-EXTERN-TAKEN-AS-DATA):
//   * `Stated`: `isData` is the reference's own statement (a C declaration, a
//     call, a typed symbol; the default);
//   * `Pending`: the reference stated nothing, and no binder has read a
//     definition yet. `isData` MEANS NOTHING in this state and must not be
//     read as a statement;
//   * `FromLibrary`: a binder read the definition's own kind from the library
//     that owns the name and wrote it into `isData`.
// A sibling unit's definition decides by folding the row away (the link's
// cross-unit resolution), whatever the state. A row bound to a library that
// is still `Pending` is refused by name, and so is a library DATUM that code
// names directly, whoever stated the kind, because that reference needs a copy
// relocation (which DSS does not make). Neither is ever defaulted.
enum class ExternKindOrigin : std::uint8_t {
    Stated,
    Pending,
    FromLibrary,
};

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
    //
    // ⚠⚠ THIS FIELD IS THE SOURCE'S CLAIM, AND IT IS NOT EVIDENCE ABOUT THE
    // LIBRARY (D-FFI-LIBRARY-TLS-EXPORT-BINDS-AS-PLAIN-DATA). `false` here
    // means "no declaration in this program spelled `thread_local`" — it does
    // NOT mean the symbol has static storage duration, because the definition
    // lives in somebody else's binary and has its own answer. Reading this
    // field as though it were the definition's property is precisely the
    // defect that anchor was opened for: a plain `extern int e;` against a
    // library that exports `e` as `STT_TLS` left this `false`, sailed past the
    // `K_FormatLacksThreadLocalSupport` gate below (which keys on THIS field),
    // bound got-indirect, and emitted `R_*_GLOB_DAT` against a thread-local —
    // ✔MEASURED to make the program read the shared library's ELF header
    // instead of its datum, on both ELF legs, with no diagnostic at all.
    // ★ THE LIBRARY'S ANSWER IS NOT CARRIED ON THIS ROW ON PURPOSE, AND THE
    // ABSENCE IS THE DESIGN RATHER THAN AN OMISSION. It is known ONLY where a
    // library is read and matched — the FF1 reader's `ImportSurface::kind` —
    // and the link tier never opens a library at all (it has a NAME, not a
    // path), so a field here could only ever be a copy made at the binder,
    // which is the tier that can simply refuse. The agreement rule therefore
    // lives at the binder, once, in `ffi::reportLibraryThreadStorageDisagreement`
    // (ffi/ingest.hpp), and is applied by all three binders: the C/HIR one in
    // `ffi::ingest`, and the assembly + pulled-archive-member ones in
    // `program/compile_pipeline.cpp`.
    bool        isThreadLocal = false;
    // ★ THE OBJECT'S OWN SYMBOL RECORD STATES THE NAME'S STORAGE DURATION (P69,
    // `link/thread_storage_agreement.hpp`). TRUE only on a row an OBJECT READER
    // made from a record whose TYPE says whether the object is thread-local —
    // today the ELF reader's COMMON rows: every common it reads is typed an
    // ordinary object (a thread-local common is refused at the read). The link
    // then judges the row against the definition its name binds to EVEN WHERE NO
    // RELOCATION OF THE UNIT NAMES IT, as GNU ld and ld.lld compare the two
    // records; a row whose record states nothing (a COFF symbol, a Mach-O nlist,
    // a C declaration no code uses) is judged by the access its unit's code
    // makes, and not at all where it makes none — which is what link.exe,
    // lld-link and Apple's ld do with the same pair. Distinct from
    // `isThreadLocal` just above, which is a DECLARATION's claim: this one says
    // only that the record made a statement, and the statement is "ordinary".
    // Read by `linker::reportThreadStorageDisagreements` alone, on the units a
    // link is given; the cross-unit merge does not carry it (that header says
    // why it need not).
    bool        recordStatesStorageDuration = false;
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
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: TRUE ⇒ an EAGER import — one DSS binds
    // even when UNREFERENCED. The linker's reference gate
    // (`rejectOrDropUnreferencedExterns`) KEEPS an eager row unconditionally; a
    // NON-eager import survives ONLY when a relocation references it — gcc's
    // "an unused extern declaration emits no import" rule, now uniform across
    // library-bound AND no-library rows.
    //
    // ⚠ THIS COMMENT NAMED A `#include`d DESCRIPTOR SYMBOL AS *THE* EAGER CASE
    // UNTIL 2026-09-03, AND THAT IS NOW EXACTLY BACKWARDS.
    // [[D-FFI-DESCRIPTOR-EAGER-IMPORT]] closed by flipping
    // `ShippedExternSymbol::eagerImport` to FALSE by default, so an ordinary
    // `#include <stdio.h>` symbol is now the commonest NON-eager row. ✔The
    // measurement that forced it: C23 7.1.4p2 entitles a program to hand-declare
    // a library function instead of including its header and calls the two
    // EQUIVALENT — yet the hand-declared spelling imported 3 symbols where the
    // `#include` spelling imported 86, and the LOADER sees the difference. The
    // flag survives because a descriptor row may still opt IN per symbol
    // (`shippedSourcePath` rows, the UCRT shim-core companions), which is what
    // it was always for.
    // Set at HIR→MIR from
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
    // ★★ A WEAK REFERENCE THAT ASKS FOR THE ARCHIVE SEARCH ITSELF (P69 round 4,
    // D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE). Set by the COFF
    // reader for a weak external whose search policy is SEARCH_LIBRARY (PE/COFF
    // 5.5.3: "a library search for sym1 should be performed"): a static link's
    // archive search fetches a member for it whatever the members' format says
    // of weak references (`archiveWeakReferenceSearch`) — ✔MEASURED link.exe
    // 14.51 fetches it (lld-link 18 does not; the specification decides the
    // fork). FALSE for every other row: DSS's own weak reference states no
    // search (its COFF writer emits ALIAS), and no other format can spell one.
    // OR-combined across the rows a merge folds into one, as
    // `requiredByDirective` is: one row asking for the search is the link's.
    bool searchesArchives = false;

    // ★★ D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING: this row is NOT a
    // foreign name. It is a reference to a symbol THIS ARTIFACT ITSELF DEFINES,
    // routed through the loader on purpose because the active format declares
    // that definition PREEMPTIBLE (`preemptibleDefinitionBindings`) — the
    // loader may hand the whole process a different image's body for that name,
    // and an image that branched to its own body would then disagree with every
    // other image in the process about what one identifier means. `false` for
    // every other producer, so a module that mints none is unchanged.
    //
    // ★ WHY IT IS A DECLARED FLAG AND NOT INFERRED FROM `libraryPath.empty()`
    // PLUS A NAME MATCH. Two folds in the link tier collapse an import onto a
    // definition of the same NAME — `resolveCrossCuSymbols` (which then makes
    // `mergeModules` strip the row) and, one tier down, `mergeCuMirs` — and
    // both are RIGHT for an ordinary import: a sibling CU's definition really
    // does shadow the library fallback, and binding it directly is both correct
    // and cheaper. For THIS row the same fold is the exact defect being closed,
    // reintroduced by the linker after the lowering removed it. Re-deriving
    // "is this that kind of row" from a name match would make two owners of one
    // fact, and the one that got it wrong would fail SILENTLY — back to one
    // process holding two answers. The producer knows; it says so here.
    //
    // ⚠ THE ROW IS STILL A REAL IMPORT ON THE WIRE. It publishes an undefined
    // dynamic symbol and takes the format's ordinary loader-resolved slot (ELF:
    // an SHN_UNDEF `.dynsym` entry + a PLT stub + a GOT slot + its `.rela.dyn`
    // GLOB_DAT; the shape ld emits for the same source). Nothing downstream
    // needs a second mechanism — only permission not to fold it away.
    bool isPreemptionReference = false;

    // ★★ D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING, THE ADDRESS HALF: a
    // SECOND, module-local SymbolId bound by the format walker to the VA of the
    // SLOT that holds this import's loader-resolved ADDRESS. Invalid (the
    // default) on every import that does not need one, so a producer that mints
    // none leaves every tier byte-identical.
    //
    // ★ WHY A SECOND SYMBOL AND NOT A SECOND ROW. One dynamic symbol is one
    // import — `dedupKey` says so and the `isData` conflict enforces it — so a
    // name that is both CALLED and ADDRESSED inside one artifact cannot be two
    // rows. What differs between the two uses is not the symbol but WHICH of the
    // import's realizations the reference wants:
    //   * the CALL wants the entry the format's `externCallDispatch` names — a
    //     PLT stub under `direct-plt`, the slot itself under `indirect-slot`;
    //   * the ADDRESS wants the SLOT's CONTENT, always, because a pointer to a
    //     call thunk is not the function's address and comparing it against the
    //     same name taken in another image would answer false (C 6.2.2p2 gives
    //     one identifier one object/function across the program).
    // Under `indirect-slot` the two coincide — `symbol` IS the slot — and the
    // field stays invalid; under `direct-plt` they are two different VAs of one
    // import, and this is the second one. The precedent is
    // `AssembledFunction::blockSymbols`: a row carrying extra module-local
    // symbols the walker gives VAs, remapped through the merge exactly the same
    // way (D-LINK-MERGE-DOES-NOT-REMAP-BLOCK-SYMBOLS is what happens when that
    // remap is skipped).
    //
    // ⚠ A walker that does not bind it FAILS LOUD rather than mis-binding: the
    // symbol is declared in the compound index, so a relocation naming a VA the
    // walker never assigned is a resolution error at link, never a zero address
    // at run.
    SymbolId addressSlotSymbol{};

    // ★★ D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE (P69 review M1, case (c)): the
    // THIRD realization of an import a reference may want — its CALL ENTRY as
    // an ADDRESS. A module-local SymbolId the format walker binds to the VA of
    // the entry a call to this import reaches (the PE import thunk). Invalid
    // (the default) on every import that does not need one.
    //
    // ★ WHO WANTS IT. A unit whose CODE reaches the import by a PC-relative
    // displacement that is NOT a branch — `leaq puts(%rip)`, MinGW gcc's
    // default `-O2` shape — can only ever compute the call entry, because a
    // displacement reaches nothing outside the image. Where the format says
    // that unit's address of the import IS its call entry
    // (`pcRelativeImportAddress: "callEntry"` — what link.exe, lld-link and
    // GNU ld do with such an object), its other address references to the
    // import (a `.quad puts` in its data) are retargeted here, so the unit
    // agrees with itself (C 6.5.9) instead of mixing the entry with the
    // loader-bound address (`linker::bindPcRelativeImportAddressUnits`).
    //
    // ⚠ Same contract as `addressSlotSymbol`: declared in the compound index,
    // remapped and folded through the merge, and a walker that does not bind
    // it fails the reference loudly.
    SymbolId callEntrySymbol{};

    // Where `isData` came from (see `ExternKindOrigin`). `Pending` for a
    // reference that states no kind: a `.s` address operand or data slot naming
    // a symbol the file does not define, an object's untyped undefined symbol
    // that no call relocation names, and (P69) a COFF linker directive's row and
    // the import an `__imp_X` reference is folded onto. `Stated` for every
    // other producer.
    ExternKindOrigin kindOrigin = ExternKindOrigin::Stated;

    // ★★★ THE CODE READS THIS IMPORT THROUGH A POINTER SLOT (P68 round 9, the
    // archive-DATA crash routed from lane `lm`,
    // D-LK-SIBLING-DATA-IMPORT-SLOT-BOUND-TO-THE-OBJECT). The answer MIR→LIR
    // gave when it chose the shape of every CODE reference to this import: a
    // DATA import under the format's `dataImportBinding: got-indirect`, and an
    // import the `indirect-slot` dispatch routes (its `indirectSlotBindings`).
    // Each such reference is `lea <symbol>` + a load of the POINTER found there,
    // so wherever the import is bound, something must HOLD its address at
    // <symbol>: the image writer's GOT / IAT slot for a library import, the
    // slot a relocatable object CARRIES where its format spells one
    // (`materializeObjectImportSlots`, PE's `.refptr.<name>`), and a slot the
    // merge mints (`mergeModules`) when a sibling module of the link DEFINES
    // it — without one, the code loads the definition's first bytes as a
    // pointer (✔MEASURED 2026-09-24 at the round's base, `int x = 42;` read
    // from a DSS static archive: an access violation on pe64, SIGSEGV — exit
    // 139 — on ELF x86_64 and ELF aarch64).
    // ★ ONE OWNER: MIR→LIR sets it (`lowerToLir`); an object READER never does —
    // a relocatable object's code states its own shape (a direct reference, a GOT
    // relocation, or a slot of its own, like PE's object-carried one) — and the
    // merge, the object-slot pass and the image link's import-reference judgment
    // (`refuseUnbindableImportReferences`: a unit that does NOT read a library
    // datum through a slot and names it in code needs a copy relocation, P68
    // round 11) only read it. Deriving it there from
    // "data + a got-indirect format" would send a DIRECT load (a pulled
    // member's, a `.s`'s) to a slot and load the ADDRESS instead of the value.
    bool readThroughSlot = false;

    // ★★ THE LINK MUST DEFINE THIS NAME, WHETHER OR NOT ANY CODE NAMES IT
    // (P69, D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE). Set by the COFF
    // reader for an `/INCLUDE:<name>` directive naming a symbol its object
    // does not define (`LinkerDirectiveMeaning::IncludeSymbol`). Such a row is
    // a USE wherever the link asks whether a row is used, even with no
    // relocation naming it: the reference gate keeps it and the archive pull
    // follows it, so an archive member defining the name is pulled, a library
    // binds it and the image imports it, and one nothing defines is refused by
    // name (`K_SymbolUndefined`), as link.exe (LNK2001) and lld-link
    // ("undefined symbol") refuse it — ✔MEASURED 2026-10-06: link.exe imports
    // an otherwise unreferenced `puts` for `/INCLUDE:puts` (run
    // 20261006-222410-df088936). OR-combined across the rows a merge folds into
    // one: any one requirement is the link's.
    bool requiredByDirective = false;

    // ★★ WHAT A REFERENCE TO THIS NAME BINDS TO WHEN NOTHING DEFINES IT
    // (P69, D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE). Empty for every
    // row but one a COFF object gave a FALLBACK: `/alternatename:<name>=<B>`
    // (cl, clang; link.exe and lld-link honour it) and a weak external whose
    // default is another undefined symbol (PE/COFF 5.5.3). The reference
    // linkers' meaning: when no linked unit and no library defines `<name>`,
    // every reference to it resolves to `<B>` instead — wherever `<B>` is
    // defined, an import library's member included (✔MEASURED 2026-10-06,
    // link.exe: `/alternatename:my_puts=puts` calls the imported `puts`, run
    // 20261006-222410-df088936). The reader keeps a row for a `<B>` its object
    // does not define, so `<B>` is bound to its library and its archive member
    // pulled like any other name; an IMAGE link decides the fallback once it
    // knows what the link defines (`linker::bindFallbackReferences`), before
    // anything binds a unit's imports, so no later stage of it sees one. A
    // RELOCATABLE artifact keeps it, and the COFF writer hands it on as the
    // directive it came from.
    std::string fallbackName;

    // ★★ A COMMON (TENTATIVE) DEFINITION, NOT A REFERENCE (P69,
    // D-LK-OBJECT-READERS-MISREAD-COMMON-SYMBOLS). Zero for every row but one an
    // object READER made from a COMMON symbol: zero-filled storage of
    // `commonSize` bytes, aligned to `commonAlignment` (a power of two), that the
    // final link allocates ONCE for its name — the LARGEST of the name's
    // commons, at the WIDEST of their alignments — unless a linked unit DEFINES
    // the name with strong binding, whose definition wins and makes every common
    // of it a reference to that definition; a common in turn wins over a WEAK
    // definition (the System V gABI symbol-table rules, PE/COFF 5.4.2, ld64).
    // Every format spells it as a symbol with no section: COFF an UNDEF external
    // whose Value is the size, ELF `SHN_COMMON` with the alignment in
    // `st_value`, Mach-O an `N_UNDF` external whose `n_value` is the size and
    // whose `n_desc` carries the alignment's log2. ✔MEASURED 2026-10-06: cl
    // 19.51 writes EVERY C tentative definition (`int x;` at file scope) so, at
    // /O2 and /Od (run 20261006-224902-a6201782), as gcc and clang do under
    // -fcommon. Such a row is a DEFINITION: the reference gate keeps it with no
    // relocation naming it, an IMAGE link turns it into storage before any
    // other pass reads the rows (`linker::allocateCommonDefinitions`), and a
    // RELOCATABLE artifact hands it on as a common, a merge folding two of one
    // name into the larger size, the wider alignment and the stricter
    // visibility. `commonVisibility` is the definition's visibility.
    std::uint64_t    commonSize       = 0;
    std::uint64_t    commonAlignment  = 0;
    SymbolVisibility commonVisibility = SymbolVisibility::Default;
};

} // namespace dss
