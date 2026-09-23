#include "analysis/semantic/target_format_analysis.hpp"

#include "analysis/semantic/semantic_analyzer.hpp"
#include "core/types/object_format_kind.hpp"   // SelectableObjectFormatKind

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

BitFieldStrategy
effectiveBitFieldStrategy(TargetSchema const&       target,
                          ObjectFormatSchema const& format) noexcept {
    // FORMAT wins (the strategy is OS/format-determined); fall back to the
    // target's declared value when the format declared none. Selects on the
    // config-declared enum only — no target/format identity branch.
    if (format.bitFieldStrategy() != BitFieldStrategy::None) {
        return format.bitFieldStrategy();
    }
    return target.aggregateLayout().bitFieldStrategy;
}

LongDoubleFormat
effectiveLongDoubleFormat([[maybe_unused]] TargetSchema const& target,
                          ObjectFormatSchema const&            format) noexcept {
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): FORMAT-only — no target-side field
    // exists to fall back to (see the header docblock). `None` propagates as
    // the honest undeclared state; the semantic bind fails loud on it.
    return format.longDoubleFormat();
}

UnnamedBitFieldAlignment
effectiveUnnamedBitFieldAlignment([[maybe_unused]] TargetSchema const& target,
                                  ObjectFormatSchema const&            format) noexcept {
    // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: FORMAT-only — no target-side field
    // exists to fall back to (see the header docblock). `None` propagates as the
    // honest undeclared state; the layout engine fails loud on it, and only when an
    // unnamed bit-field actually needs the rule.
    return format.unnamedBitFieldAlignment();
}

// ── NOT HERE: `effectiveCharIsUnsigned` (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM)
// ──────────────────────────────────────────────────────────────
// There is deliberately no third member of the `effective*` family for
// bare-`char` signedness. This family exists because those axes have
// contributions from BOTH schemas that must be RECONCILED — a genuine
// two-sided negotiation. Char signedness has exactly ONE contributor: the
// target declares the whole (processor × platform) fact in its single
// `charIsUnsigned` key. An `effectiveCharIsUnsigned(target, format)` wrapper
// would advertise a negotiation that no longer happens, and would be a second
// place the fact is "about" — the exact duplication this reshape removed.
// The call site asks the owner directly: `target.charIsUnsigned(format.kind())`.

// See the header. The body is the derivation the driver's front half
// (`compile_pipeline.cpp` `buildCuHirImpl`) performed inline until it became the
// derivation EVERY channel performs; its reasoning travelled with it.
TargetFormatAnalysis
analyzeForTargetFormat(std::shared_ptr<CompilationUnit const> cu,
                       DiagnosticBudget                       budget,
                       TargetSchema const&                    target,
                       ObjectFormatSchema const&              format,
                       TargetCallingConvention const*         callingConvention) {
    // D-HIR-RESOLVE-ELEMENT-CORE-UNKNOWN-AS-KEY: the pair's format KIND enters the
    // analysis through the one-spelling type, so no pair can hand it the invalid
    // sentinel (a schema with no resolved backend answers `Unknown`).
    auto const formatKind = SelectableObjectFormatKind::of(format.kind());
    if (!formatKind.has_value()) {
        std::fputs("dss::analyzeForTargetFormat fatal: the object-format schema "
                   "resolved no format kind (ObjectFormatKind::Unknown, the invalid "
                   "sentinel) -- a pair must name a real object format\n",
                   stderr);
        std::abort();
    }
    // FC3 c1: thread the FORMAT's declared data model (its REQUIRED
    // `dataModel` field) into the per-(CU × target) analysis — the
    // single source for every width-dependent resolution downstream
    // (builtinTypes/typeSpecifiers `coreByDataModel`, the integer-
    // literal ladder, descriptor `signatureByDataModel`). The HIR
    // lowering reads the SAME value back off the SemanticModel.
    // FC6 deferral-close: also thread the target's aggregate-layout params so a
    // `sizeof` in an array-dimension const-expression (`int a[sizeof(T)]`) folds
    // through the same `computeLayout` engine MIR uses — `nullopt` when the
    // target declared no block (the fold then fails loud, never a wrong size).
    // D-CSUBSET-BITFIELD-ABI-EXACT: overlay the FORMAT-resolved bit-field strategy
    // onto the target's params (the strategy is OS/format-determined; the target
    // supplies only the alignment rule). A `sizeof` over a bit-field struct in an
    // array dimension then folds with the byte-ABI-exact layout.
    auto const effectiveBfStrategy = effectiveBitFieldStrategy(target, format);
    // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: resolved beside the strategy and
    // overlaid at the SAME three consumer sites, because the two axes are read by one
    // packer and a site that got one without the other would lay out to a mixture of
    // two ABIs.
    auto const effectiveUnnamedBfAlign =
        effectiveUnnamedBitFieldAlignment(target, format);
    std::optional<AggregateLayoutParams> analyzeLayout;
    if (target.aggregateLayoutLoaded()) {
        analyzeLayout = target.aggregateLayout();
        analyzeLayout->bitFieldStrategy = effectiveBfStrategy;
        analyzeLayout->unnamedBitFieldAlignment = effectiveUnnamedBfAlign;
    }
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, BLOCKER-2): capture the RESOLVED CC's
    // WHOLE `vaListLayout` block. Read from the SAME resolved CC the MirLoweringConfig
    // reads its `vaListLayout` from; `nullopt` when the CC declares no
    // variadic-callee ABI.
    //
    // TWO consumers, each taking the part it needs from this ONE lookup:
    //   * the semantic `va_list`-type injection wants only `.strategy`, to size the `ap`
    //     local per ABI (SysV __va_list_tag[1]=24B vs Win64 char*=8B). `nullopt` there ⇒
    //     the SysV-family default, which is inert (a CC with no vaListLayout has no
    //     variadic-callee surface at all).
    //   * D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): `synthesizeStdioShim`, across the MIR/LIR
    //     seam, needs the WHOLE block — `.variadicUsesOverflowBase` is what selects its
    //     va leaf, and reading only `.strategy` there was a latent silent miscompile (see
    //     `CuMirModule::vaListLayout`). Same resolved CC, resolved ONCE — and returned
    //     on `TargetFormatAnalysis::vaListLayout` for exactly that reason.
    std::optional<VaListLayout> analyzeVaLayout;
    if (callingConvention != nullptr && callingConvention->vaListLayout.has_value()) {
        analyzeVaLayout = *callingConvention->vaListLayout;
    }
    std::optional<VaListStrategy> const analyzeVaStrategy =
        analyzeVaLayout.has_value() ? std::optional<VaListStrategy>{analyzeVaLayout->strategy}
                                    : std::nullopt;
    // D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE: this
    // producer BINDS imports, so it answers descriptor role entries — from the
    // active format's own row, or its shipped flavour family's.
    FormatRuntimeLibraryRoleResolver const roleResolver{format};
    SemanticModel model = analyze(
        // D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE: the caller's budget —
        // the driver's is the operator's, carried on `opts` from `rep`, never the
        // relaxed per-target scratch.
        std::move(cu), budget,
        format.dataModel(), analyzeLayout, analyzeVaStrategy,
        formatKind,          // c8: the active object-format → per-target availability gate
        target.name(),       // plan 25: the active arch → per-target shipped-struct variant selector
        // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): the format-resolved `long double`
        // axis — drives the coreByLongDoubleFormat row overrides; None (wasm/
        // spirv) leaves `long double` rows unrealized (loud on use).
        effectiveLongDoubleFormat(target, format),
        // ★ Inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS): THE ACTIVE TARGET.
        // Without it `analyze` runs with `target == nullptr`, and its two
        // target-dependent asm checks — `S_InlineAsmConstraintLetterUndeclared`
        // (0xE065) and `S_InlineAsmClobberUnknown` (0xE068) — correctly decline
        // to guess and DO NOT RUN. ✔MEASURED before this argument existed: a
        // `"=Zq"` constraint and a `"notaregister"` clobber BOTH compiled to a
        // clean `.o` at rc=0 through the driver's pipeline. A diagnostic that
        // fires only in a unit test that passes its own schema is not a shipped
        // diagnostic. `target` must outlive the returned model, which republishes
        // it as `SemanticModel::target()` for the HIR lowering.
        &target,
        // The standard deep-recursion reserve (the `0` sentinel) — spelled only
        // because the resolver behind it is positional.
        /*deepRecursionReserveBytes=*/0,
        // Consulted during analysis only, never republished by the model, so a
        // reference to a local outlives every read of it.
        &roleResolver);
    return TargetFormatAnalysis{std::move(model), std::move(analyzeVaLayout)};
}

} // namespace dss
