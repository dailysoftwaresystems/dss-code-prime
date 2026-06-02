#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

// Plan 14 §3.1 D-LK6-8.2 closure (2026-06-01).
//
// Cross-validate that the (target, format) pair the user supplied
// via `--target=<target>:<format>` declares matching machine
// identity. The silent-failure CRITICAL the architect anchored:
// `arm64:elf64-x86_64-linux-exec` (or a hand-edited format JSON
// declaring the wrong `machine` value) would silently dispatch
// into the x86_64 PLT-stub emitter, producing SIGILL at runtime
// with no driver diagnostic.
//
// Lives at the driver tier (per the anchor's architect Q1 answer):
// `compileOneTarget` loads both schemas, then calls this helper
// BEFORE invoking `compileSingleUnit`. Format-side `validate()`
// can't cross-check because the target schema's arch identity isn't
// reachable from the format-load context.
//
// Source / target / linker agnostic:
//   * Validation routes through a closed-enum table mapping target
//     names → expected machine codes per object-format kind.
//   * Adding a new arch (RISC-V, PPC64, MIPS) = add a row to
//     `kTargetArchMachineCodes` + the format JSONs declare the
//     matching machine value.
//   * Targets not in the table (WASM / SPIR-V / future) skip the
//     check — those format kinds don't carry a `machine` field;
//     compatibility is governed by `abiModel()` upstream.

namespace dss {

// Per-target-arch expected machine codes for each format kind.
// The codebase pattern: closed-enum table, no string drift.
// Fields are u32 to accommodate Mach-O `cputype` (32-bit); ELF +
// PE machine values are u16 but fit u32 trivially.
struct DSS_EXPORT TargetArchMachineCodes {
    std::string_view targetName;     // matches TargetSchema::name()
    std::uint32_t    elfMachine;     // EM_*  (e.g. 62 for x86_64, 183 for AArch64)
    std::uint32_t    peMachine;      // IMAGE_FILE_MACHINE_*  (e.g. 0x8664, 0xAA64)
    std::uint32_t    machoCpuType;   // CPU_TYPE_*  (e.g. 0x01000007, 0x0100000C)
};

// Cross-validate the (target, format) pair's machine identity.
// Returns true on match (or skip: unknown target / format-kind
// without a machine field). Returns false + emits
// `D_TargetFormatMismatch` through `reporter` on mismatch.
[[nodiscard]] DSS_EXPORT bool
crossValidateTargetFormat(TargetSchema const&        target,
                          ObjectFormatSchema const&  format,
                          DiagnosticReporter&        reporter);

// Test-exposed for direct coverage (post-fold pattern from
// `rangeExceedsBuffer`). Lets unit tests pin every arch+kind cell
// without synthesising full TargetSchema/ObjectFormatSchema objects.
[[nodiscard]] DSS_EXPORT std::span<TargetArchMachineCodes const>
targetArchMachineCodesTable() noexcept;

} // namespace dss
