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
    dss::report(reporter, DiagnosticCode::D_TargetFormatMismatch,
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

bool crossValidateTargetFormat(TargetSchema const&       target,
                                ObjectFormatSchema const& format,
                                DiagnosticReporter&       reporter) {
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
