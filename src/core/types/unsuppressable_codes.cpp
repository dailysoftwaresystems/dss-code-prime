#include "core/types/unsuppressable_codes.hpp"

#include <array>

namespace dss {

namespace {

// D-FF2-4 closed-table. Sorted by phase letter (D / F / H / I / K) +
// numeric value within each phase for at-a-glance audit. The linear
// scan in `isUnsuppressable` is O(N) on N≈15 members — faster than any
// hash lookup at this size + needs no static-init dance.
constexpr std::array<DiagnosticCode, 15> kUnsuppressableCodes{{
    // D_* driver / target band — pending-plan announcement +
    // permanent architectural exclusion of operand-stack / result-id
    // abiModels from the register-machine LIR pipeline.
    DiagnosticCode::D_PlanNotLanded,
    DiagnosticCode::D_TargetAbiModelUnsupportedByDriver,

    // F_* FFI band — architectural exclusions on the FF5 ingest path
    // (WASM/SPIR-V abiModels don't take FF4 mangling; empty canonical
    // names would silently shadow `bySymbol[""]` rows).
    DiagnosticCode::F_FfiIngestAbiModelUnsupported,
    DiagnosticCode::F_FfiIngestEmptyCanonical,

    // H_* HIR-lowering / verifier band — structural invariants (cannot
    // reach MIR codegen without violating downstream contracts).
    DiagnosticCode::H_TypeUnresolved,
    DiagnosticCode::H_VerifierFailure,
    DiagnosticCode::H_UnsupportedLoweringForKind,
    DiagnosticCode::H_ExternHasInitializer,

    // I_* MIR-verifier band — frozen-module invariants on entry blocks
    // and SSA dominance. A suppressed violation here would let a
    // miscompile sail past the verifier.
    DiagnosticCode::I_VerifierFailure,
    DiagnosticCode::I_NoEntryBlock,
    DiagnosticCode::I_MultipleEntryBlocks,
    DiagnosticCode::I_EntryBlockNotFirst,
    DiagnosticCode::I_NotDominated,

    // K_* linker band — image refused / undefined extern. Suppressing
    // would let the writer emit a corrupted artifact OR the program
    // dispatch a call to a never-resolved address at runtime.
    DiagnosticCode::K_SymbolUndefined,
    DiagnosticCode::K_ImageNotOk,
}};

} // namespace

bool isUnsuppressable(DiagnosticCode code) noexcept {
    for (auto c : kUnsuppressableCodes) {
        if (c == code) return true;
    }
    return false;
}

std::span<DiagnosticCode const> unsuppressableCodes() noexcept {
    return kUnsuppressableCodes;
}

} // namespace dss
