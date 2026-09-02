#include "program/cross_validate_target_format.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <format>

namespace dss {

// ── D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES ─────────────────────────
//
// FOURTEEN of the anchor's twenty-six mentions lived in this file, and they were
// TWO different things wearing one costume. Sorting them was the work; both
// piles are gone, but for different reasons.
//
// ★ PILE ONE — `abiModelMatchesFormatKind` (8 mentions). It decided whether a
// pairing was LEGAL by testing the format's IDENTITY against a hardcoded set
// per abi-model (`kind == Elf || kind == Pe || kind == MachO`, …). A validator
// may compare two declarations; it may not carry its own hardcoded opinion
// about which identities satisfy which rule. It is replaced by a coherence
// check between two DECLARATIONS that both already exist and are both REQUIRED:
// the target's `abiModel` and the format's `cCallingConvention`. A format that
// names a calling convention passes arguments in registers; one that declares
// the reserved `none` does not. That IS the execution-model claim, stated by
// the document, and it costs no new key.
//
// ★ PILE TWO — the machine-code `switch` (6 mentions). It read a DIFFERENT
// FIELD per identity (`elf.machine` / `pe.machine` / `macho.cputype`) and
// compared each against `kTargetArchMachineCodes`, a C++ table keyed on
// (target name x format kind). That table is DELETED, with `lookupTargetArch`,
// `emitMismatch`, its `consteval` uniqueness assert and the public
// `targetArchMachineCodesTable()` accessor, which had no caller outside this
// file. ⚠ THE ARGUMENT IS ONE-OWNER, NOT TIDINESS: "arm64 is EM_AARCH64 = 183"
// had TWO owners — each format document's own `machine` field and that C++ row
// — with nothing forcing them to agree. Exactly the shape of `kCManglingRules`
// and `kAbiCatalog`. The number now lives only in the document that emits it,
// and the pairing question is answered by a NAME the document states outright.
//
// ⚠⚠ WHAT THIS NARROWED, STATED PLAINLY RATHER THAN LEFT TO BE FOUND. The
// deleted `abiModelMatchesFormatKind` distinguished all THREE abi-models;
// `cCallingConvention` distinguishes only register-passing from not. So
// (operand-stack target x SPIR-V format) and (result-id target x WASM format)
// are no longer refused HERE. Both pairings are unreachable today — no
// `wasm32.target.json` or `spirv.target.json` ships, and neither has ever been
// pinned by a test — and both are still refused downstream, where `resolveAbi`
// yields a null cc and the driver emits
// `D_TargetAbiModelUnsupportedByDriver`. ★ CLOSING PREDICATE: this arm returns
// the moment a WASM or SPIR-V `.target.json` ships (plan 18 / plan 17), and the
// key it needs is a one-line `targetAbiModel` on the two format skeletons. It
// is recorded as a narrowing, not carried as a silent loss.
//
// ★ WHAT DID NOT MOVE, AND WHY THAT IS CORRECT. The `D_TargetMachineCodeMismatch`
// code and its message keep their names. The failure CLASS is unchanged — "this
// format is not for this target, and dispatching would emit the wrong
// instruction set into the image" — so renaming a shared `DiagnosticCode`
// enumerator would churn a cross-cutting header to describe the same event.

bool crossValidateTargetFormat(TargetSchema const&       target,
                                ObjectFormatSchema const& format,
                                DiagnosticReporter&       reporter) {
    // ── (1) EXECUTION MODEL — the silent-failure CRITICAL-1 gate ───────────
    //
    // Catches a register-machine target paired with a WASM/SPIR-V format (which
    // would reach `compileSingleUnit` and hand a register-machine LIR shape to a
    // walker that has no awareness of it) AND the inverse.
    //
    // Guarded on `declared()` because `ObjectFormatSchema{ObjectFormatData}` is
    // a public constructor running no validation: an in-memory producer can
    // arrive with no claim at all, and treating "never declared" as "declares
    // none" would invent a refusal the document never asked for. Every LOADED
    // schema has it — the key is REQUIRED — so this guard is unreachable from
    // config and cannot weaken the shipped path.
    if (format.cCallingConvention().declared()) {
        bool const targetPassesInRegisters =
            target.abiModel() == TargetAbiModel::RegisterMachine;
        bool const formatPassesInRegisters =
            !format.cCallingConvention().declaresNoConvention();
        if (targetPassesInRegisters != formatPassesInRegisters) {
            dss::report(reporter, DiagnosticCode::D_TargetAbiModelMismatch,
                        DiagnosticSeverity::Error,
                        std::format("target '{}' declares abiModel='{}' but "
                                    "object format '{}' declares "
                                    "cCallingConvention '{}'. A "
                                    "register-machine target passes arguments "
                                    "in registers and requires a format that "
                                    "names one of its calling conventions; a "
                                    "format declaring the reserved 'none' has "
                                    "no register-level C ABI and requires a "
                                    "non-register target. Anchored: plan 14 "
                                    "§3.1 D-LK6-8.2.",
                                    target.name(),
                                    targetAbiModelName(target.abiModel()),
                                    format.name(),
                                    format.cCallingConvention().convention));
            return false;
        }
    }

    // ── (2) THE PAIRING — two DECLARED NAMES, compared ─────────────────────
    //
    // An empty `targetArch` means the document makes NO CLAIM about which
    // target it serves, and the check does not apply — byte-for-byte the
    // behaviour the deleted `lookupTargetArch(...) == nullptr` path had for a
    // target absent from its table. It is not "any target will do": every
    // shipped format declares the key, and a corpus sweep pins that it does.
    if (!format.targetArch().empty() && format.targetArch() != target.name()) {
        dss::report(reporter, DiagnosticCode::D_TargetMachineCodeMismatch,
                    DiagnosticSeverity::Error,
                    std::format("object format '{}' declares targetArch '{}' "
                                "but was paired with target '{}'. A mismatched "
                                "(target, format) pair would silently dispatch "
                                "to the wrong walker and emit the wrong "
                                "instruction set into the image — fix the "
                                "format JSON's 'targetArch' field OR pair the "
                                "source with a format that serves this target. "
                                "Anchored: plan 14 §3.1 D-LK6-8.2.",
                                format.name(), format.targetArch(),
                                target.name()));
        return false;
    }

    return true;
}

} // namespace dss
