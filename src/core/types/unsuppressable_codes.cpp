#include "core/types/unsuppressable_codes.hpp"

#include <algorithm>
#include <array>

namespace dss {

namespace {

// D-FF2-UNSUPP closed-table. Sorted by phase letter (D / F / H / I / K
// / L / R / A) + numeric value within each phase for at-a-glance
// audit. The linear scan via `std::ranges::find` is O(N) over the
// table — still faster than hash lookup at this size + needs no
// static-init dance.
//
// Membership rule: a code is unsuppressable when its surface is a
// load-bearing structural invariant whose silent re-opening (via
// `--suppress=<code>`) would let a miscompile / wrong-bytes /
// undefined-extern artifact ship green. Examples in shipped
// closed-table: D-LK6-8.2 split codes (silent ABI mismatch ⇒
// SIGILL at user runtime), I_* verifier invariants (SSA / CFG
// violations sailing through), K_ImageWrite* (silently truncated
// on-disk image), F_FfiIngest* architectural exclusions (silent
// wrong-shape FfiMetadata for the wrong abiModel). Table size
// grows monotonically as new architectural surfaces close; each
// addition includes a one-line rationale block alongside the
// entry.
constexpr std::array<DiagnosticCode, 79> kUnsuppressableCodes{{
    // D_* driver / target band — pending-plan announcement,
    // permanent architectural exclusion of operand-stack / result-id
    // abiModels from the register-machine LIR pipeline, and the
    // D-LK6-8.2 split codes that close the SIGILL surface
    // (suppressing either would let `--target=arm64:elf64-x86_64...`
    // or schema-typo'd `machine` dispatch wrong PLT-stub emitter).
    DiagnosticCode::D_PlanNotLanded,
    DiagnosticCode::D_TargetAbiModelUnsupportedByDriver,
    DiagnosticCode::D_TargetMachineCodeMismatch,
    DiagnosticCode::D_TargetAbiModelMismatch,

    // F_* FFI band — architectural exclusions on the FF5 ingest path
    // (WASM/SPIR-V abiModels don't take FF4 mangling; empty canonical
    // names would silently shadow `bySymbol[""]` rows).
    DiagnosticCode::F_FfiIngestAbiModelUnsupported,
    DiagnosticCode::F_FfiIngestEmptyCanonical,
    // F_FfiNoImportLibraryForFormat (FF6 Slice 2, 2026-06-02): the
    // source-declared FFI synthesis path fails loud when the active
    // language's `DeclarationRule.externLibraryByFormat` map has no
    // entry for the target's ObjectFormatKind. Suppressing this
    // would let externs land with `importLibrary=""` → downstream
    // K_FormatLacksImportSupport or K_SymbolUndefined diagnostics
    // would fire instead, masking the upstream config gap and
    // forcing operators to debug from the wrong end of the
    // pipeline. The closed-table membership pins the upstream
    // surface.
    DiagnosticCode::F_FfiNoImportLibraryForFormat,
    // F_BinaryReaderPartialCorruption (silent-failure-hunter
    // 2nd-order audit on 9dbdc8e): the Warning's stated intent is
    // "operators must see this signal". Without unsuppressable
    // membership, the four cap/dedup gates at report() could
    // silently drop it under multi-target cap saturation —
    // re-opening the very silent-skip surface the commit closed.
    // The closed-table's invariant is "must reach `all_` regardless
    // of policy", which is independent of severity; Warning members
    // are admissible when their visibility is load-bearing.
    DiagnosticCode::F_BinaryReaderPartialCorruption,
    // F_ShippedHeaderNotFound (FF11, 2026-06-05): a `#include <h>`
    // SYSTEM header not found on any `shippedLibDirs` search dir. A
    // missing system header is a HARD error in C (unlike a local
    // include's soft `D_UnresolvedImport`) — suppressing or cap-dropping
    // it would let a program that calls an undeclared shipped-library
    // symbol compile SILENTLY, exactly the silent-miscompile this
    // fail-loud closes. The closed-table membership pins it.
    DiagnosticCode::F_ShippedHeaderNotFound,
    // F_ShippedLibDescriptorMalformed / F_ShippedLibUnsupportedType
    // (neutral shipped-lib descriptor, 2026-06-06): the LANGUAGE-NEUTRAL
    // shipped-library JSON descriptor read by
    // `dss::ffi::readShippedLibDescriptor`. The first fires on a
    // malformed descriptor (bad JSON / missing-required-key / wrong-type
    // / unknown-key / bad enum); the second on a symbol whose
    // `signature` hir-text type fails to decode. Both are load-bearing:
    // suppressing either would let the lowering synthesize NO externs
    // (or skip a symbol that fails to decode) and silently compile a
    // program whose `#include <stdio.h>` symbols resolve to nothing —
    // exactly the silent dropped-import surface these fail-louds close.
    // The CRITICAL invariant is that a signature that does not decode
    // MUST NOT reach `makeExternFunction` with InvalidType.
    DiagnosticCode::F_ShippedLibDescriptorMalformed,
    DiagnosticCode::F_ShippedLibUnsupportedType,
    // F_ShippedHeaderUnavailableForTarget (p18 Cluster G c8, 2026-06-25):
    // a `#include <h>` whose shipped descriptor declares the header is NOT
    // available on the active target's object-format (POSIX <sys/time.h> on
    // windows-pe). Suppressing it would let the semantic phase resume past the
    // gate and INJECT the header's symbols/structs/typedefs on the wrong
    // platform — the exact wrong-platform silent miscompile this fail-loud
    // closes. A direct sibling of the three shipped-header surfaces above.
    DiagnosticCode::F_ShippedHeaderUnavailableForTarget,
    // F_ShippedStructVariantAmbiguous (p18 Cluster G, plan 25, 2026-06-26): a
    // shipped `structs` entry's per-target `variants` had MORE THAN ONE match the
    // active (arch, format). The selection contract is exactly-one-matches;
    // suppressing this would re-open the "pick the first" silent wrong-layout
    // surface (e.g. an under-specified `when:{arch:"x86_64"}` matching both
    // x86_64-elf and x86_64-pe → the linux struct layout used on windows). A
    // direct sibling of the four shipped-lib surfaces above — its invariant is the
    // SAME class (a wrong-bytes import must never ship green).
    DiagnosticCode::F_ShippedStructVariantAmbiguous,
    // F_ShippedConstantVariantAmbiguous / F_ShippedTypedefVariantAmbiguous /
    // F_ShippedMacroVariantAmbiguous (p18 Cluster G, plan 25 extension,
    // 2026-06-26): the per-target `variants` mechanism extended from `structs` to
    // the CONSTANTS, TYPEDEFS, and MACROS surfaces — a macOS build can get a
    // different constant VALUE / typedef WIDTH / macro REPLACEMENT than the linux
    // build from one descriptor. Each fires when MORE THAN ONE variant matches the
    // active target. Same selection contract + same silent-miscompile class as the
    // struct-variant sibling above: an under-specified `when` would silently pick
    // the first → a wrong constant value / typedef width / macro replacement on
    // this target. Suppressing any would re-open that "pick the first" wrong-value
    // surface — so all three are members like F_ShippedStructVariantAmbiguous.
    DiagnosticCode::F_ShippedConstantVariantAmbiguous,
    DiagnosticCode::F_ShippedTypedefVariantAmbiguous,
    DiagnosticCode::F_ShippedMacroVariantAmbiguous,

    // H_* HIR-lowering / verifier band — structural invariants (cannot
    // reach MIR codegen without violating downstream contracts). Post-
    // fold #11 type-design CRITICAL: both `H_ExternDeclMalformed` and
    // `H_ExternHasInitializer` MUST be here — they are the two arms
    // of the H2 split, both terminate lowering with `return
    // errorNode(node)` + gate ok via errorCount.
    DiagnosticCode::H_TypeUnresolved,
    DiagnosticCode::H_VerifierFailure,
    DiagnosticCode::H_UnsupportedLoweringForKind,
    DiagnosticCode::H_ExternHasInitializer,
    DiagnosticCode::H_ExternDeclMalformed,

    // I_* MIR-verifier band — frozen-module invariants. A suppressed
    // violation here would let a miscompile sail past the verifier.
    // Post-fold #11 F2 expansion: all 12 I_* codes are structural
    // invariants in the same band as I_VerifierFailure/I_NoEntryBlock;
    // pre-fold only 5 were listed — the gap let `--suppress=I_NotDominated`
    // (or any other I_* code) re-open the SSA / CFG miscompile surface.
    DiagnosticCode::I_VerifierFailure,
    DiagnosticCode::I_NoEntryBlock,
    DiagnosticCode::I_MultipleEntryBlocks,
    DiagnosticCode::I_EntryBlockNotFirst,
    DiagnosticCode::I_BlockNotTerminated,
    DiagnosticCode::I_PhiPredNotInCfg,
    DiagnosticCode::I_NotDominated,
    DiagnosticCode::I_TerminatorTypeMismatch,
    DiagnosticCode::I_ArgIndexOutOfRange,
    DiagnosticCode::I_ArgPositionDuplicate,
    DiagnosticCode::I_ExtensionTypeInMir,
    DiagnosticCode::I_StructCfMismatch,
    DiagnosticCode::I_UnreachableBlock,

    // K_* linker band — image refused / undefined extern + the LK10
    // image-write contract codes. Suppressing any K_ImageWrite* code
    // would let `errorCount() == 0` while the image is missing/truncated
    // on disk — exactly the silent-failure LK10 cycle 1 closed.
    DiagnosticCode::K_SymbolUndefined,
    DiagnosticCode::K_ImageNotOk,
    DiagnosticCode::K_ImageWriteParentMissing,
    DiagnosticCode::K_ImageWriteOpenFailed,
    DiagnosticCode::K_ImageWriteShort,
    DiagnosticCode::K_ImageWriteCloseFailed,
    DiagnosticCode::K_ImageEmpty,
    // Post-fold #12 D-FF2-UNSUPP-FULL-SWEEP additions:
    // K_NoMatchingObjectFormat — format-walker dispatch invariant
    //   (suppressing → wrong walker / corrupted artifact)
    // K_FormatLacksImportSupport — extern resolution against format
    //   without import-table support (suppressing → unresolved extern
    //   in dynamic image)
    // K_RelocationKindMismatch — applying a reloc kind the format
    //   doesn't support (suppressing → silent miscompile bytes)
    // K_WalkerInputContractViolation — walker received malformed input
    //   from the linker driver (suppressing → upstream corruption
    //   propagates downstream silently)
    DiagnosticCode::K_NoMatchingObjectFormat,
    DiagnosticCode::K_FormatLacksImportSupport,
    DiagnosticCode::K_RelocationKindMismatch,
    DiagnosticCode::K_WalkerInputContractViolation,
    // K_EntryPointResolvesToExtern — extern-named-as-entry is a
    // schema misconfiguration that produces a runnable binary
    // pointing at a stub IAT slot. Suppressing → the loader jumps
    // to unrelocated import-stub bytes at process entry → SEGV with
    // no diagnostic trail.
    DiagnosticCode::K_EntryPointResolvesToExtern,
    // K_DuplicateDataSymbol / K_BssDataHasBytes — producer-side
    // AssembledData invariant violations caught by
    // `validateAssembledData()`. Suppressing either would let a
    // producer ship an `AssembledModule` with two items at the
    // same SymbolId (last-write-wins silent resolution) or a Bss
    // item carrying bytes that would either bloat the on-disk
    // image or silently drop. Both are substrate-shape violations
    // that must not be silently accepted. 3rd-order audit fold
    // (D-LK4-RODATA-BSS-INVARIANT).
    DiagnosticCode::K_DuplicateDataSymbol,
    DiagnosticCode::K_BssDataHasBytes,

    // L_* LIR verifier / lowering band — structural invariants
    // (cannot reach assembler-tier codegen without violating
    // downstream contracts). All 11 codes fire from arms that gate
    // the producer's ok() / return value. Suppressing any → silent
    // miscompile through the LIR layer.
    //
    // Post-fold #13 silent-failure CRITICAL: L_UnsupportedLoweringForOpcode
    // (0xB001) is the MIR→LIR analog of H_UnsupportedLoweringForKind —
    // fires from 22+ sites across mir_to_lir.cpp / lir_callconv.cpp /
    // lir_2addr_legalize.cpp / lir_pass_util.cpp / lir_verifier.cpp on
    // every coverage-gap deferral. Was omitted in post-fold #12 →
    // `--suppress=L_UnsupportedLoweringForOpcode` silently re-opened
    // the MIR→LIR miscompile surface for unrecognized opcodes.
    //
    // L_IndirectCalleeClobberedByArgSetup (FC4 c2): the backstop for
    // the indirect-callee regalloc rules — suppressing it would turn
    // a callee-clobbered-by-arg-setup regression back into a SILENT
    // garbage jump through an argument value.
    DiagnosticCode::L_UnsupportedLoweringForOpcode,
    DiagnosticCode::L_RequiredLirOpcodeMissing,
    DiagnosticCode::L_VirtualRegInPostRegalloc,
    DiagnosticCode::L_MemOperandMalformed,
    DiagnosticCode::L_PhysRegOrdinalOutOfRange,
    DiagnosticCode::L_InvalidSpillSlotSentinel,
    DiagnosticCode::L_MoveCycleUnsupported,
    DiagnosticCode::L_IndirectCallUnsupported,
    DiagnosticCode::L_IndirectCalleeClobberedByArgSetup,
    DiagnosticCode::L_StackPassedArgUnsupported,
    DiagnosticCode::L_CcRegLookupFailed,

    // R_* regalloc band — calling-convention / class invariants.
    // R_SpilledDueToPressure + R_SpilledDueToCrossCallExhaustion
    // are Info-severity (intentional informational signal) and stay
    // OUT of the table; only the Error-severity gating codes are
    // members.
    DiagnosticCode::R_NoCallingConventions,
    DiagnosticCode::R_CallingConventionLookupFailed,
    DiagnosticCode::R_VRegHasNoClass,

    // A_* assembler / encoding band — bytes-on-disk invariants
    // (suppressing → wrong machine code emitted). The
    // A_NoMatchingEncodingVariant arm fires from format walkers
    // when no encoding row matches; A_RoundTripMismatch fires from
    // the round-trip self-test; A_NoEncodingDeclared /
    // A_NoEncodingShapeWalker / A_LirToMirSizeMismatch are
    // pipeline-shape invariants.
    DiagnosticCode::A_LirToMirSizeMismatch,
    DiagnosticCode::A_NoMatchingEncodingVariant,
    DiagnosticCode::A_RoundTripMismatch,
    DiagnosticCode::A_NoEncodingDeclared,
    DiagnosticCode::A_NoEncodingShapeWalker,
    // D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): a too-wide immediate that
    // can't fit a fixed32 immediate slot must never be silently
    // truncated to a wrong machine-code constant (e.g. wrong syscall
    // number). Same bytes-on-disk-invariant band as the others above.
    DiagnosticCode::A_ImmediateOperandOutOfRange,

    // S_* semantic band — silent-MISCOMPILE guards.
    // c27 (D-CSUBSET-VOLATILE-POINTEE, 2026-06-27) RETIRED
    // S_VolatilePointeeNotSupported: a pointer-to-volatile-POINTEE is no longer a
    // reject — `volatile` is now a TYPE qualifier (TypeKind::VolatileQual), so
    // `volatile <base> *` builds Ptr<VolatileQual(base)> and the deref carries
    // MirInstFlags::Volatile from the pointee type (the c21 model-B limitation the
    // reject fronted is gone). The diagnostic enum + name are kept for ordinal
    // stability / historical golden references but the code is NEVER emitted, so it
    // is no longer a member of this closed unsuppressable table (an unemittable code
    // cannot be suppressed). The silent-miscompile it once guarded is now prevented
    // by the access chokepoint, pinned red-on-disable by the `volatile_pointee_cse`
    // corpus + the multi-site MIR access tests.
    // S_IncompleteTypeMember (c24, D-CSUBSET-SELF-REFERENTIAL-STRUCT, 2026-06-27):
    // a DIRECT (non-pointer) member of an INCOMPLETE composite — e.g.
    // `struct N { struct N n; }` (a struct containing itself by value, an
    // infinite-size cycle). Suppressing it would let the member fold its size to
    // 0 (the incomplete composite has no layout) — a silent wrong-bytes layout.
    // Same silent-miscompile-guard class as the entries above; a pointer-to-
    // incomplete (`struct N *`) is legal and is NOT rejected.
    DiagnosticCode::S_IncompleteTypeMember,
    // S_IncompleteTypeObject (c35, D-CSUBSET-FORWARD-STRUCT-DECLARATION,
    // 2026-06-28): a by-VALUE OBJECT (local/global) of an INCOMPLETE composite —
    // `struct S v;` where `struct S` is forward-declared but never defined. c35's
    // opaque-tag forward-mint makes the reference RESOLVE (so an opaque `struct S
    // *` pointer compiles); suppressing this by-value reject would let the object
    // fold its frame/.bss size to 0 (the incomplete composite has no layout) — a
    // silent wrong-bytes object. Same silent-miscompile-guard class as
    // S_IncompleteTypeMember (the by-value MEMBER case); a pointer-to-incomplete is
    // legal and is NOT rejected.
    DiagnosticCode::S_IncompleteTypeObject,
    // S_TypeNameDeclaratorNotAbstract (c26, D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME,
    // 2026-06-27): a TYPE-NAME (cast / sizeof / compound-literal) whose abstract
    // declarator illegally carries a NAME (`(int x)expr`). NOTE — unlike the
    // silent-miscompile guards above, suppressing this ships NO wrong bytes: the
    // type resolves to InvalidType regardless of the emit gate, so the build still
    // fails. It is closed here so the failure is never SILENT — a suppressed
    // constraint violation would otherwise fail the build with zero diagnostics
    // shown (a confusing silent failure REASON), which the closed table forbids.
    DiagnosticCode::S_TypeNameDeclaratorNotAbstract,
    // S_StaticAssertFailed (FC16, D-CSUBSET-STATIC-ASSERT, 2026-07-07): a
    // `_Static_assert(cond[, "msg"]);` whose condition is non-constant or folds
    // to zero. Same posture as S_TypeNameDeclaratorNotAbstract above —
    // suppressing it ships NO wrong bytes (the analyzer's error still fails the
    // build via `hasErrors()`), but a suppressed constraint violation would
    // fail the build with ZERO diagnostics shown — a confusing silent failure
    // REASON the closed table forbids. Closed here so a false static_assert is
    // never silent.
    DiagnosticCode::S_StaticAssertFailed,
    // S_GenericSelectionNoMatch / S_GenericSelectionAmbiguous (FC16,
    // D-CSUBSET-GENERIC-SELECTION, 2026-07-07): a `_Generic` whose controlling
    // type matched no typed association (and had no `default`), or matched more
    // than one. Same posture as S_StaticAssertFailed / S_TypeNameDeclaratorNot-
    // Abstract above: on the no-match/ambiguous path the genericExpr node is left
    // UNTYPED (InvalidType), so the build fails via `hasErrors()` regardless of
    // the emit gate — no wrong bytes ship — but a suppressed constraint violation
    // would fail the build with ZERO diagnostics shown, a confusing silent
    // failure REASON the closed table forbids. Closed here so an unselectable
    // `_Generic` is never silent.
    DiagnosticCode::S_GenericSelectionNoMatch,
    DiagnosticCode::S_GenericSelectionAmbiguous,
    // S_Alignas* (C11/C23 6.7.5, D-CSUBSET-ALIGNAS, 2026-07-07): the five
    // `_Alignas`/`alignas` constraint violations — not-power-of-two, exceeds-max,
    // weaker-than-natural, invalid-context (typedef/function/parameter/bit-field),
    // and non-constant. Same posture as S_StaticAssertFailed above: each is a
    // 6.7.5 CONSTRAINT violation; the analyzer's error already fails the build via
    // `hasErrors()` (no wrong bytes ship — the stored alignment is simply not
    // applied), but a SUPPRESSED constraint violation would fail the build with
    // ZERO diagnostics shown, the confusing silent-failure REASON the closed table
    // forbids. Closed here so an invalid alignas is never silent.
    DiagnosticCode::S_AlignasNotPowerOfTwo,
    DiagnosticCode::S_AlignasExceedsMax,
    DiagnosticCode::S_AlignasWeakerThanNatural,
    DiagnosticCode::S_AlignasInvalidContext,
    DiagnosticCode::S_AlignasNonConstant,
}};

// Post-fold #11 code-review F1: consteval uniqueness pin matches the
// codebase pattern at `kAbiCatalogTuplesUnique` + `kHeaderReadErrorTable`'s
// row-alignment static_assert. The runtime `ListSelfConsistent` test
// already catches duplicates, but the codebase prefers compile-time
// closed-table invariants where possible — a paste-error duplicate
// becomes a build failure, not a test failure.
consteval bool kUnsuppressableCodesAreUnique() {
    for (std::size_t i = 0; i < kUnsuppressableCodes.size(); ++i) {
        for (std::size_t j = i + 1; j < kUnsuppressableCodes.size(); ++j) {
            if (kUnsuppressableCodes[i] == kUnsuppressableCodes[j]) return false;
        }
    }
    return true;
}
static_assert(kUnsuppressableCodesAreUnique(),
              "kUnsuppressableCodes must not contain duplicate entries — "
              "every code appears at most once.");

// FF11 audit (2026-06-05): guard against the "bump the array size but
// forget to add the entry" class — a missing initializer value-inits the
// trailing slot to `DiagnosticCode::None` (0), which would silently make a
// real code suppressible AND make `isUnsuppressable(None)` wrongly true.
// `std::array<DiagnosticCode, N>` accepts a short initializer; this catches
// the resulting `None` slot at COMPILE time (the uniqueness check above does
// not — a single `None` is "unique"). It also rejects an intentional `None`.
consteval bool kUnsuppressableCodesHaveNoNone() {
    for (auto const c : kUnsuppressableCodes) {
        if (c == DiagnosticCode::None) return false;
    }
    return true;
}
static_assert(kUnsuppressableCodesHaveNoNone(),
              "kUnsuppressableCodes must not contain DiagnosticCode::None — a "
              "None slot means the array size was bumped without adding the "
              "intended code.");

} // namespace

bool isUnsuppressable(DiagnosticCode code) noexcept {
    return std::ranges::find(kUnsuppressableCodes, code)
         != kUnsuppressableCodes.end();
}

std::span<DiagnosticCode const> unsuppressableCodes() noexcept {
    return kUnsuppressableCodes;
}

} // namespace dss
