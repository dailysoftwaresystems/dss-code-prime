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
// Source / target / linker agnostic — D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES:
//   * BOTH checks compare two DECLARATIONS and neither reads a format
//     IDENTITY. The execution-model check pairs the target's `abiModel`
//     against the format's REQUIRED `cCallingConvention`; the arch check
//     pairs the target's NAME against the format's declared `targetArch`.
//   * Adding a new arch (RISC-V, PPC64, MIPS) is JSON-ONLY: ship the
//     `.target.json` and the `.format.json`s that name it in `targetArch`.
//     ⚠ It used to ALSO require a C++ row in `kTargetArchMachineCodes`, and
//     that table is DELETED — the machine number it duplicated now has one
//     owner, the format document that emits it.
//   * A format that declares no `targetArch` makes no claim and skips the
//     pairing check — the behaviour the deleted table had for a target it
//     had no row for. Every shipped format declares it, pinned by a sweep.

namespace dss {

// Cross-validate the (target, format) pair. Returns true on a match, or when
// the format makes no `targetArch` claim (i.e. the check does not apply).
// Returns false + emits `D_TargetAbiModelMismatch` (execution model) or
// `D_TargetMachineCodeMismatch` (arch pairing) through `reporter`.
//
// ⓘ `D_TargetMachineCodeMismatch` keeps its name although the comparison is no
// longer of machine NUMBERS: the failure CLASS is unchanged — this format is
// not for this target — and renaming a shared `DiagnosticCode` enumerator would
// churn a cross-cutting header to describe the same event.
[[nodiscard]] DSS_EXPORT bool
crossValidateTargetFormat(TargetSchema const&        target,
                          ObjectFormatSchema const&  format,
                          DiagnosticReporter&        reporter);

} // namespace dss
