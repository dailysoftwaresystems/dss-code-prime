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
constexpr std::array<DiagnosticCode, 62> kUnsuppressableCodes{{
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
