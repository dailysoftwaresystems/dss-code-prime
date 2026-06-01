#include "program/cross_validate_target_format.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <format>

namespace dss {

namespace {

// Closed-enum table of known target-arch machine codes. Two arches
// shipped at v1; add a row here when a new arch lands AND the
// shipped target.json + format.json pair declare the matching
// machine value.
//
// Sources:
//   ELF EM_* values per the AMD64 / AArch64 ELF psABIs + gABI fig 4-2.
//   PE IMAGE_FILE_MACHINE_* per Microsoft PE/COFF spec.
//   Mach-O CPU_TYPE_* per <mach/machine.h>.
constexpr std::array<TargetArchMachineCodes, 2> kTargetArchMachineCodes{{
    // x86_64: EM_X86_64=62, IMAGE_FILE_MACHINE_AMD64=0x8664, CPU_TYPE_X86_64=CPU_ARCH_ABI64|7=0x01000007.
    { "x86_64", 62u,  0x8664u, 0x01000007u },
    // arm64: EM_AARCH64=183, IMAGE_FILE_MACHINE_ARM64=0xAA64, CPU_TYPE_ARM64=CPU_ARCH_ABI64|12=0x0100000C.
    { "arm64",  183u, 0xAA64u, 0x0100000Cu },
}};

// Static uniqueness check: `lookupTargetArch` returns the FIRST
// match by linear scan, so a duplicate `targetName` row (paste-error
// from adding a 3rd arch) would silently be dead code while the
// first row served as the sole authority. Catch the typo at compile
// time. (silent-failure HIGH-1 post-fold #1.)
consteval bool targetArchNamesAreUnique() {
    for (std::size_t i = 0; i < kTargetArchMachineCodes.size(); ++i) {
        for (std::size_t j = i + 1; j < kTargetArchMachineCodes.size(); ++j) {
            if (kTargetArchMachineCodes[i].targetName
                == kTargetArchMachineCodes[j].targetName) {
                return false;
            }
        }
    }
    return true;
}
static_assert(targetArchNamesAreUnique(),
              "kTargetArchMachineCodes contains duplicate targetName "
              "rows. lookupTargetArch returns the FIRST match, so a "
              "duplicate row is dead code that masks a paste-error.");

[[nodiscard]] TargetArchMachineCodes const*
lookupTargetArch(std::string_view targetName) noexcept {
    for (auto const& row : kTargetArchMachineCodes) {
        if (row.targetName == targetName) return &row;
    }
    return nullptr;
}

void emitMismatch(DiagnosticReporter& reporter,
                  std::string_view targetName,
                  std::string_view kindName,
                  std::uint64_t expected,
                  std::uint64_t actual,
                  std::string_view fieldName) {
    dss::report(reporter, DiagnosticCode::D_TargetMachineCodeMismatch,
                DiagnosticSeverity::Error,
                std::format("target '{}' expects {} {}=0x{:X} ({}), "
                            "but the format schema declares 0x{:X} ({}). "
                            "Mismatched (target, format) pair would "
                            "silently dispatch to the wrong walker — "
                            "fix the format JSON's '{}' field OR pair "
                            "the source with a format whose machine "
                            "code matches the target arch. "
                            "Anchored: plan 14 §3.1 D-LK6-8.2.",
                            targetName, kindName, fieldName,
                            expected, expected,
                            actual, actual,
                            fieldName));
}

} // namespace

std::span<TargetArchMachineCodes const>
targetArchMachineCodesTable() noexcept {
    return {kTargetArchMachineCodes.data(), kTargetArchMachineCodes.size()};
}

// ABI-model ↔ format-kind compatibility (silent-failure CRITICAL-1
// post-fold #1). The previous version of this file claimed `abiModel()`
// gated WASM/SPIR-V compatibility "upstream" but no consumer ever read
// the field. Result: a `RegisterMachine` x86_64 target paired with a
// WASM format silently passed through cross-validate, reached
// `compileSingleUnit`, and the WASM walker had no awareness of the
// register-machine LIR shape. The check below makes the claim real.
//
// Closed-enum dispatch on the target's abi-model:
//   RegisterMachine → Elf / Pe / MachO are valid; Wasm / Spirv reject.
//   OperandStack    → Wasm valid; everything else rejects.
//   ResultId        → Spirv valid; everything else rejects.
[[nodiscard]] bool
abiModelMatchesFormatKind(TargetAbiModel abi, ObjectFormatKind kind) noexcept {
    switch (abi) {
        case TargetAbiModel::RegisterMachine:
            return kind == ObjectFormatKind::Elf
                || kind == ObjectFormatKind::Pe
                || kind == ObjectFormatKind::MachO
                || kind == ObjectFormatKind::Unknown;  // defer to linker
        case TargetAbiModel::OperandStack:
            return kind == ObjectFormatKind::Wasm
                || kind == ObjectFormatKind::Unknown;
        case TargetAbiModel::ResultId:
            return kind == ObjectFormatKind::Spirv
                || kind == ObjectFormatKind::Unknown;
    }
    return false;  // unreachable per closed enum
}

bool crossValidateTargetFormat(TargetSchema const&       target,
                                ObjectFormatSchema const& format,
                                DiagnosticReporter&       reporter) {
    // ABI-model ↔ format-kind cross-check (CRITICAL-1 post-fold #1).
    // Catches `RegisterMachine` target paired with Wasm/Spirv format
    // AND `OperandStack`/`ResultId` target paired with native format.
    // This MUST run before the per-arch machine-code check below —
    // WASM/SPIR-V skip the machine check (no machine field), so a
    // mismatched abi-model would slip past the table lookup silently.
    if (!abiModelMatchesFormatKind(target.abiModel(), format.kind())) {
        dss::report(reporter, DiagnosticCode::D_TargetAbiModelMismatch,
                    DiagnosticSeverity::Error,
                    std::format("target '{}' declares abiModel='{}' "
                                "but the format schema has kind='{}'. "
                                "Register-machine targets require ELF/PE/Mach-O; "
                                "operand-stack requires WASM; result-id requires "
                                "SPIR-V. Anchored: plan 14 §3.1 D-LK6-8.2.",
                                target.name(),
                                targetAbiModelName(target.abiModel()),
                                objectFormatKindName(format.kind())));
        return false;
    }

    auto const* row = lookupTargetArch(target.name());
    if (row == nullptr) {
        // Target not in the cross-check table. Defer to format-side
        // validation (WASM/SPIR-V/future ISAs without machine codes
        // legitimately reach this branch).
        return true;
    }

    switch (format.kind()) {
        case ObjectFormatKind::Elf: {
            std::uint32_t const actual =
                static_cast<std::uint32_t>(format.elf().machine);
            if (actual != row->elfMachine) {
                emitMismatch(reporter, target.name(), "ELF",
                             row->elfMachine, actual, "elf.machine");
                return false;
            }
            return true;
        }
        case ObjectFormatKind::Pe: {
            std::uint32_t const actual =
                static_cast<std::uint32_t>(format.pe().machine);
            if (actual != row->peMachine) {
                emitMismatch(reporter, target.name(), "PE",
                             row->peMachine, actual, "pe.machine");
                return false;
            }
            return true;
        }
        case ObjectFormatKind::MachO: {
            std::uint32_t const actual = format.macho().cputype;
            if (actual != row->machoCpuType) {
                emitMismatch(reporter, target.name(), "Mach-O",
                             row->machoCpuType, actual,
                             "macho.cputype");
                return false;
            }
            return true;
        }
        case ObjectFormatKind::Wasm:
        case ObjectFormatKind::Spirv:
            // WASM + SPIR-V are abstract VMs with no machine-code
            // identity. Target/format compatibility is enforced via
            // `abiModel()` (OperandStack / ResultId) at the dispatch
            // layer; no cross-check at this tier.
            return true;
        case ObjectFormatKind::Unknown:
            // Unknown format kind reaches the linker's own
            // K_NoMatchingObjectFormat guard — don't double-fail at
            // the driver tier.
            return true;
    }
    return true;
}

} // namespace dss
