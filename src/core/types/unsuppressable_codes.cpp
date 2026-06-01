#include "core/types/unsuppressable_codes.hpp"

#include <algorithm>
#include <array>

namespace dss {

namespace {

// D-FF2-UNSUPP closed-table. Sorted by phase letter (D / F / H / I / K
// / L / R / A) + numeric value within each phase for at-a-glance
// audit. The linear scan via `std::ranges::find` is O(N) on N=51
// members — still faster than hash lookup at this size + needs no
// static-init dance.
//
// Post-fold #11 silent-failure F2: table expanded from 16→27 to match
// the documented taxonomy. Missing entries (D-LK6-8.2 split codes,
// remaining I_* verifier invariants, image-write K_* codes, FFI
// architectural codes) were silently re-opening their respective
// failure surfaces under `--suppress=<code>` — particularly the
// D-LK6-8.2 SIGILL surface (D_TargetMachineCodeMismatch /
// D_TargetAbiModelMismatch) and the LK10 image-write contract
// (K_ImageWrite* + K_ImageEmpty).
constexpr std::array<DiagnosticCode, 51> kUnsuppressableCodes{{
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

    // L_* LIR verifier / lowering band — structural invariants
    // (cannot reach assembler-tier codegen without violating
    // downstream contracts). All 9 codes fire from arms that gate
    // the producer's ok() / return value. Suppressing any → silent
    // miscompile through the LIR layer.
    DiagnosticCode::L_RequiredLirOpcodeMissing,
    DiagnosticCode::L_VirtualRegInPostRegalloc,
    DiagnosticCode::L_MemOperandMalformed,
    DiagnosticCode::L_PhysRegOrdinalOutOfRange,
    DiagnosticCode::L_InvalidSpillSlotSentinel,
    DiagnosticCode::L_MoveCycleUnsupported,
    DiagnosticCode::L_IndirectCallUnsupported,
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

} // namespace

bool isUnsuppressable(DiagnosticCode code) noexcept {
    return std::ranges::find(kUnsuppressableCodes, code)
         != kUnsuppressableCodes.end();
}

std::span<DiagnosticCode const> unsuppressableCodes() noexcept {
    return kUnsuppressableCodes;
}

} // namespace dss
