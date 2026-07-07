#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

// D-LK10-ENTRY Slice C (plan 14 §2.13): linker-synthesized `_start`
// entry trampoline — the third and final slice of the runnable-
// binary spine. Builds on:
//
//   * Slice A (2026-06-02): `syscall` + `call_indirect_via_extern`
//     + `unreachable` encoding LIR opcodes on `x86_64.target.json`.
//   * Slice B (2026-06-02): `ExitMechanism` + `ProcessExit` POD
//     vocabulary + `processExit` + `entryCallingConvention` fields
//     on `ObjectFormatData` + JSON loader + cross-field validate().
//
// The emitter is bucket-2 universal — ZERO `if (target == ...)`
// branches, ZERO hardcoded byte sequences. It constructs a tiny
// one-function `Lir` module and runs it through the existing
// `assemble()` substrate, so per-CPU encoding lives in the
// assembler's `*.target.json`-declared encoding tables.
//
// ── Trampoline shape per mechanism ──────────────────────────────
//
// Program-entry argument prefix (D-RUNTIME-MAIN-ARGC-ARGV, c88) —
// emitted FIRST when the format declares a `processArgs` block,
// BEFORE the ABI prologue (the stack offsets are defined against
// the untouched process-entry SP). StackVector arm — 2 LIR ops:
//   -1a. load argGprs[0], [sp + argcStackOffset] ; argc (machine word)
//   -1b. lea  argGprs[1], [sp + argvStackOffset] ; the in-place argv
//                                     vector's ADDRESS (never deref'd)
// Signature-independent: main(void) simply ignores the registers.
// No `processArgs` block ⇒ nothing emitted (Mach-O LC_MAIN already
// receives argc/argv in the arg registers from dyld — pass-through;
// PE's out-parameter CRT route is anchored D-RUNTIME-PE-MAIN-ARGS).
//
// Syscall arm (Linux ELF / Mach-O BSD syscall) — 5 LIR ops:
//   1. call user_entry              ; intra-image REL32 to user fn
//   2. mov argGprs[0], returnGprs[0]; status: rax → rdi (SysV) / x0
//   3. mov syscallNumGpr, syscallNumber  ; e.g. mov rax, 231
//   4. syscall                      ; SYSCALL / SVC #0
//   5. unreachable                  ; ud2 / BRK #0 (verifier hint)
//
// ByNameImport arm (Windows PE / Mach-O libSystem) — 4 LIR ops:
//   1. call user_entry              ; REL32 to user fn
//   2. mov argGprs[0], returnGprs[0]; status: rax → rcx (MS x64)
//   3. call_indirect_via_extern exit_import  ; FF 15 disp32 → IAT
//   4. unreachable
//
// ── Mutates `module` in place ───────────────────────────────────
//
//   * Mints a fresh SymbolId for the trampoline (`_start`).
//   * For ByNameImport: appends a synthetic `ExternImport` for the
//     exit symbol (e.g. kernel32!ExitProcess) into
//     `module.externImports` — the existing PE IAT writer (LK6
//     cycle 2a) emits the IAT slot.
//   * Prepends a synthetic `AssembledFunction` as `functions[0]`;
//     bumps `expectedFuncCount`.
//   * Sets `module.imageEntryOverride = std::optional<size_t>{0}`
//     — the OPTIONAL discriminant matters (index 0 IS a valid
//     override target — see `AssembledModule.imageEntryOverride`
//     docblock for the Unknown=0-vs-valid-0 rationale).
//
// ── Fail-loud diagnostics ───────────────────────────────────────
//
//   * `K_NoMatchingObjectFormat` — format declared no
//     `processExit`, or `entryCallingConvention` does not resolve
//     against `target.callingConventionByName(...)`.
//   * `K_SymbolUndefined` — user-entry symbol cannot be resolved
//     from `format.entryPoint()` (or `functions.empty()`).
//   * `K_RelocationKindMismatch` / `A_NoMatchingEncodingVariant` —
//     surface through the inner `assemble()` call if the target
//     lacks one of the required Slice A opcodes (e.g. ARM64 today
//     pending D-LK10-ENTRY-ARM64).
//
// ── Bucket purity ──────────────────────────────────────────────
//
//   * Resolves all per-OS data from the FORMAT schema's
//     `processExit` block (mechanism dispatch).
//   * Resolves all per-CPU register names from the TARGET schema's
//     calling convention (`argGprs[0]` for status; `returnGprs[0]`
//     for source).
//   * Resolves opcode mnemonics via `target.opcodeByMnemonic(...)`
//     — no opcode-number hardcodes.
//   * Resolves physical register ordinals via
//     `target.registerByName(...)` — no per-CPU register-name
//     literals beyond the format-declared `syscallNumGpr`.
//
// ARM64 lands by populating `arm64.target.json` (`syscall` +
// `call_indirect_via_extern` opcodes — anchored D-LK10-ENTRY-ARM64)
// + the `elf64-aarch64-linux-exec.format.json` $processExit block
// (anchored). The emitter requires zero change.

namespace dss::linker {

[[nodiscard]] DSS_EXPORT bool injectEntryTrampoline(
    AssembledModule&            module,
    TargetSchema const&         target,
    ObjectFormatSchema const&   format,
    DiagnosticReporter&         reporter);

} // namespace dss::linker
