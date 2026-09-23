#pragma once

// ── THE SEMANTIC TIER'S HALF OF ONE `<target>:<format>` PAIR ────────────────
//
// `analyze()` takes a pair's consequences as EIGHT separate arguments — the data
// model, the aggregate layout with two format overlays, the `va_list` strategy of
// the resolved calling convention, the format kind, the target's name, the
// `long double` representation, the target itself and the runtime-library role
// resolver — and every one of them has a default, because unit tests and
// format-blind callers need one. That made "analyze under a pair" something each
// caller had to REASSEMBLE, and only one caller ever did: the driver's front
// half. The LSP called `analyze(cu, budget)` and so did the FFI header parser.
//
// ✔MEASURED before this file existed ([[D-LSP-HEADER-CASE-RULE-NOT-WORKSPACE-AWARE]],
// a real `dsscp --lsp` beside `dsscp --project` on the same files):
//   * pe64 workspace, `#include <pthread.h>` / `<dlfcn.h>` / `<sys/mman.h>`: the
//     editor was CLEAN and the build refused `F_ShippedHeaderUnavailableForTarget`;
//     elf workspace, `<direct.h>` / `<process.h>`: the same, mirrored. A silent
//     accept — the semantic tier's availability gate needs `activeFormat`;
//   * pe64 `#include <windows.h>`: the build rc=0, the editor two
//     `F_ShippedTypeIdentityConflict` (the descriptor spells `long` 32-bit, and the
//     editor analyzed under the LP64 default data model);
//   * every `long double`: the editor `S_LongDoubleFormatUndeclared`, the build
//     rc=0;
//   * `_Static_assert(sizeof(long) == …)`: the editor "not an integer constant
//     expression" under every pair — with no aggregate layout `sizeof` never folds
//     — while the build decided it per pair.
//
// ★★★ SO THE DERIVATION HAS ONE HOME AND THREE CALLERS. `analyzeForTargetFormat`
// is the only place that turns (target, format, calling convention) into
// `analyze()`'s arguments. The driver's front half (`compile_pipeline.cpp`
// `buildCuHirImpl`), the LSP and the FFI header parser all call it, so a new
// pair-decided argument added to `analyze()` lands here once and reaches every
// channel, instead of reaching the driver and silently defaulting everywhere
// else — which is how every divergence listed above came to exist. The
// builder's half of the same pair (the preprocessor's view) is
// `applyTargetFormatPair` (`analysis/compilation_unit/compilation_unit.hpp`);
// every channel calls both.
//
// The three `effective*` resolvers moved here UNCHANGED from
// `program/compile_pipeline.{hpp,cpp}`: they are the only pair derivations this
// function needs that lived in the driver tier, which the LSP may not include
// (`WorkspaceProject.TheLspNeverReachesUpIntoTheDriverTier`). The driver's other
// consumers (the HIR→MIR and assembler layout overlays) reach them here too.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"
#include "core/types/aggregate_layout.hpp"   // BitFieldStrategy, UnnamedBitFieldAlignment
#include "core/types/data_model.hpp"         // LongDoubleFormat
#include "core/types/diagnostic_budget.hpp"
#include "core/types/target_schema.hpp"      // TargetSchema, TargetCallingConvention, VaListLayout
#include "link/object_format_schema.hpp"      // ObjectFormatSchema

#include <memory>
#include <optional>

namespace dss {

// D-CSUBSET-BITFIELD-ABI-EXACT: resolve the effective per-ABI bit-field packing
// strategy for a (target, format) pair. C bit-field allocation is FORMAT/OS-
// determined (one CPU target — x86_64 — serves BOTH ELF-SysV `gnu_packed` and
// PE-MS `msvc_straddle`), so the FORMAT's declared strategy WINS; when the format
// declares none (`None`), it falls back to the TARGET's
// `aggregateLayout().bitFieldStrategy` (back-compat for targets that predate the
// format-side field). This is the ONE resolution chokepoint the driver overlays
// onto the target's `AggregateLayoutParams` before threading them into the layout
// engine at all three consumer sites (analyze / HIR→MIR / asm globals). It selects
// purely on config (the declared enum), NEVER on a target/format name.
[[nodiscard]] DSS_EXPORT BitFieldStrategy
effectiveBitFieldStrategy(TargetSchema const&       target,
                          ObjectFormatSchema const& format) noexcept;

// FC17.9(e) (D-CSUBSET-LONG-DOUBLE): resolve the effective `long double` axis
// for a (target, format) pair — the effectiveBitFieldStrategy twin. The
// representation is FORMAT/OS-determined (one x86_64 target serves BOTH pe64's
// 64-bit-IEEE and ELF-SysV's x87 80-bit `long double`), and UNLIKE
// bitFieldStrategy no target-side fallback field exists — the axis is
// format-only, so `None` means genuinely undeclared (wasm/spirv skeletons):
// the semantic bind then leaves `long double` rows unrealized
// (S_LongDoubleFormatUndeclared on use), never a silent width guess. The
// (target, format) signature keeps the resolver-family shape so a future
// target-side contribution slots in without touching the call sites.
[[nodiscard]] DSS_EXPORT LongDoubleFormat
effectiveLongDoubleFormat(TargetSchema const&       target,
                          ObjectFormatSchema const& format) noexcept;

// D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: resolve whether an UNNAMED bit-field
// contributes its declared type's alignment, for a (target, format) pair. The
// `effectiveLongDoubleFormat` twin, NOT the `effectiveBitFieldStrategy` one: the axis
// is FORMAT-ONLY, so there is no target-side field to fall back to and `None` means
// genuinely undeclared. The layout engine then fails loud — but ONLY if an unnamed
// bit-field is actually laid out under `gnu_packed`, so the wasm/spirv skeletons that
// declare no C ABI stay unaffected. Never a silent default: BOTH answers are a real
// ABI somewhere (`ignored` on SysV/Darwin, `contributes` on AAPCS), so guessing one
// would be a silent miscompile on every platform holding the other.
[[nodiscard]] DSS_EXPORT UnnamedBitFieldAlignment
effectiveUnnamedBitFieldAlignment(TargetSchema const&       target,
                                  ObjectFormatSchema const& format) noexcept;

// ★ NO `effectiveCharIsUnsigned` HERE — and its absence is the design, not an
// omission (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM, TF-C75). The two resolvers
// above exist because their axes have contributions from BOTH schemas that must
// be reconciled. Bare-`char` signedness has exactly ONE contributor: the TARGET
// declares the whole (processor × platform) fact in its single `charIsUnsigned`
// key (`{"default": …, "byObjectFormat": {…}}`), and resolution is
// `TargetSchema::charIsUnsigned(ObjectFormatKind)` — a method on the owner, with
// the format KIND as a REQUIRED argument so no caller can silently take the
// processor half alone. A wrapper here would be a second place the fact is
// "about", which is precisely the duplication this reshape removed. The result
// still lands in `MirLoweringConfig.charIsUnsigned`, whose sole reader is
// `isSignedIntKind(TypeKind::Char)` — the single SExt-vs-ZExt decision for the
// char→int promotion.

// What `analyzeForTargetFormat` produces: the model, plus the ONE derived fact
// the driver's lower half still needs from the same derivation — the resolved
// calling convention's whole `vaListLayout` block (`nullopt` when the CC declares
// no variadic-callee ABI, or when no CC was supplied). Carried out rather than
// re-resolved, because resolving one fact twice is how two halves of one compile
// come to disagree (`CuHirModule::vaListLayout`).
struct DSS_EXPORT TargetFormatAnalysis {
    SemanticModel               model;
    std::optional<VaListLayout> vaListLayout;
};

// Run semantic analysis over `cu` exactly as a build of the pair (`target`,
// `format`) runs it: the format's data model, the target's aggregate layout with
// the format's two bit-field overlays, the `va_list` strategy of
// `callingConvention`, the format's kind (the per-format availability gate), the
// target's name (the shipped-struct variant selector), the format's `long double`
// representation, the target (the inline-asm constraint and clobber checks) and
// the format's runtime-library role resolver.
//
// `callingConvention` is the pair's resolved calling convention — the entry of
// `target.callingConventions()` that `ffi::resolveAbi(target, format, …)`
// answers — or `nullptr` for a pair that resolves none (the operand-stack /
// result-id ABI models), which the driver refuses before it gets here. It must be
// an entry of `target`.
//
// `cu` must have been built with `applyTargetFormatPair(…, target, format)` —
// the preprocessor's view of the SAME pair. Diagnostics stay on the returned
// model's own reporter; the caller drains them, as every `analyze()` caller does.
//
// `format` must resolve a real format kind. `ObjectFormatSchema::kind()` answers
// the invalid sentinel `Unknown` for a schema with no resolved backend, and a
// pair carrying it is a caller's bug that is refused here (stderr + abort), never
// handed to `analyze()` — whose tiers read an engaged `Unknown` two different ways
// (D-HIR-RESOLVE-ELEMENT-CORE-UNKNOWN-AS-KEY; `SelectableObjectFormatKind`).
[[nodiscard]] DSS_EXPORT TargetFormatAnalysis
analyzeForTargetFormat(std::shared_ptr<CompilationUnit const> cu,
                       DiagnosticBudget                       budget,
                       TargetSchema const&                    target,
                       ObjectFormatSchema const&              format,
                       TargetCallingConvention const*         callingConvention);

} // namespace dss
