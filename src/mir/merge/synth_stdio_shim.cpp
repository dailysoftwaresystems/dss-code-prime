#include "mir/merge/synth_stdio_shim.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"   // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"   // rederiveStructCfMarkers
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>   // std::sort
#include <array>
#include <cstdint>
#include <optional>    // std::optional (the declared-or-absent va_list strategy; core lookup)
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>     // std::move, std::pair
#include <vector>

namespace dss {

namespace {

// Verbatim clone policy — keep every block so each existing function is re-added
// unchanged; the shim functions are appended after the clone loop. (Same shape as
// synthesizeThreadsShim's / synthesizePeStartup's.)
class IdentityClonePolicy final : public opt::passes::MirRebuildPolicy {
public:
    // The rebuild DRIVER this policy belongs to — printed by every
    // `MirFunctionRebuilder` fatal.
    // See D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS (one line: a wrapped
    // anchor name mints a second, unregistered anchor).
    // A MIR merge step, not a `kPassNameTable` pipeline pass.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SynthStdioShim";
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> blocks;
        std::uint32_t const n = src.funcBlockCount(fn);
        blocks.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) blocks.push_back(src.funcBlockAt(fn, i));
        return blocks;
    }
};

void emitErr(DiagnosticReporter& rep, std::string msg) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// corecrt_stdio_config.h `_Options` bits. These are the CONTRACT between the shim body
// and the UCRT core, and a wrong bit fails SILENTLY (wrong NUL handling / wrong return
// value), never loudly — which is why the exact value was measured with a hand-written
// probe before this pass existed, not read off documentation. `sprintf()` passes
// LEGACY_VSPRINTF_NULL_TERMINATION (bit 0) paired with `_BufferCount = (size_t)-1`.
constexpr std::uint64_t kOptLegacyVsprintfNullTermination = 1ull << 0;

// Bit 1, `_CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR` (corecrt_stdio_config.h) —
// what `snprintf` ALONE passes, and the first shipped recipe to pass a nonzero `_Options`
// other than sprintf's legacy bit 0.
//
// ★ THIS BIT IS THE WHOLE C99 CONTRACT OF `snprintf`, not a tuning knob, and the two
// behaviours it selects between are BOTH silent — neither errors, neither fails to link,
// so the only way to tell them apart is to observe a TRUNCATING call at runtime. That is
// what `examples/c-subset/shipped_snprintf_ucrt` exists to do, and it is also why the older
// `sprintf`-only witness could not pin `_Options` at all: with `_BufferCount = (size_t)-1`
// no truncation is reachable, so bits 0/1/2 are observationally identical there.
//
// WHAT THE DIVERGENCE ACTUALLY IS, SEPARATED INTO MEASURED AND DOCUMENTED because the two
// are not the same size and conflating them would make this comment lie:
//   * MEASURED (clear the bit, rebuild, run on pe64): the RETURN VALUE flips. C99 7.19.6.5
//     requires the length the output WOULD have had; with the bit clear the core returns
//     -1. Verbatim, `snprintf(buf, 4, "%d", 12345)` went `ret=5` → `ret=-1`, and the corpus
//     example went exit 42 → exit 50.
//   * DOCUMENTED, NOT REPRODUCED HERE: the legacy `_snprintf` is also described as leaving
//     the buffer unterminated on truncation. On the ucrtbase build measured, it did NOT —
//     the NUL was still written at `buf[3]` with the bit clear. So NUL-placement is NOT the
//     discriminator; do not cite it as one. (The buffer's NUL and its extent ARE pinned by
//     the example, but against the `_BufferCount` argument, not this bit — see below.)
constexpr std::uint64_t kOptStandardSnprintfBehavior = 1ull << 1;

// ZERO options — what `printf`/`fprintf`/`vfprintf`/`sscanf` pass, and a DECISION rather
// than a placeholder, so it is named instead of spelled `0` five times. On the printf side
// the nonzero bits are the two LEGACY flags a real MSVC link only sets when
// `legacy_stdio_definitions.lib` is in the link line (the pre-UCRT `%s`-of-`wchar_t`
// and NUL-termination quirks) — DSS links no such lib, so plain-0 IS the modern,
// C-conforming behavior. On the scanf side bit 0 is SECURECRT, which turns
// `__stdio_common_vsscanf` into `sscanf_s` (every `%s` then consumes an EXTRA buffer-size
// argument from `ap`) and bit 1 is LEGACY_WIDE_SPECIFIERS (narrow `%s`/`%c` would mean
// WIDE) — both must stay OFF for the C-standard `sscanf` this shim implements, and both
// would corrupt argument consumption rather than diagnose.
constexpr std::uint64_t kOptNone = 0ull;

// UCRT's UNBOUNDED-buffer sentinel for the `_BufferCount` parameter — `(size_t)-1`, what
// the real header inlines pass for the length-less `sprintf`/`sscanf`. `snprintf` is the
// contrast and the reason this is a named constant rather than a literal: it forwards the
// caller's REAL `n`, because the bounded buffer is the entire point of the call.
constexpr std::uint64_t kBufferCountUnbounded = ~0ull;

// `__acrt_iob_func` indices — UCRT's `stdin`/`stdout`/`stderr` are `__acrt_iob_func(0/1/2)`
// (corecrt_wstdio.h). Only `stdout` has a consumer here: `printf` is `fprintf` to it.
constexpr std::uint32_t kIobStdout = 1;

} // namespace

bool synthesizeStdioShim(
    Mir&                                                  mir,
    TypeInterner&                                         interner,
    std::unordered_map<std::uint32_t, std::string> const& recipeBySymbol,
    std::optional<VaListLayout> const&                    vaLayout,
    std::vector<ExternImport> const&                      externImports,
    DiagnosticReporter&                                   reporter) {
    // Presence gate: no tagged shim symbol ⇒ clean no-op. Keys on the MAP (a data
    // property), never on a format check.
    if (recipeBySymbol.empty()) return true;

    // A non-empty recipe map means this target carries synthesize-tagged <stdio.h> symbols
    // whose bodies FORWARD A va_list — so the active calling convention MUST declare which
    // variadic model to forward under. A missing `vaListLayout` is a target/descriptor
    // mismatch: fail LOUD, never invent one. (A defaulted layout would make "the CC
    // declared nothing" indistinguishable from "the CC declared SysVRegisterSave" — exactly
    // the identity branch the bar forbids, and the same refusal `synthesizeThreadsShim`
    // makes on a nullopt `librarySynthesis` vehicle.)
    if (!vaLayout.has_value()) {
        emitErr(reporter,
                "synthesizeStdioShim: <stdio.h> printf-family synth recipes are present but "
                "the active calling convention declares no `vaListLayout` "
                "(D-FFI-PE-CRT-UCRT-MIGRATION) — there is no declared variadic model to "
                "forward, and defaulting one would be an assumed ABI, not a read one");
        return false;
    }

    // The va_list model is a TARGET property read from config, exactly as hir_to_mir
    // selects it. Only the HomogeneousPointer arm is built: no elf/macho descriptor
    // declares stdio synthesize recipes, so a SysVRegisterSave / Aapcs64DualCursor arm
    // would be a speculative build. Fail LOUD rather than emit a wrong-ABI forward.
    //
    // ★ ENUMERATED, NOT EXCLUDED — and the difference is a silent-miscompile channel,
    // not a style preference (D-MIR-STDIO-SHIM-IGNORES-VARIADIC-OVERFLOW-BASE).
    // This gate used to read `strategy != HomogeneousPointer`, which refused the other
    // strategies only INCIDENTALLY. The natural edit when a SysV arm eventually lands is
    // `if (strategy == SysVRegisterSave) { …sysv… } else { …homogeneous… }` — and under
    // an exclusion test `Aapcs64DualCursor` then rides silently into the homogeneous arm,
    // where the shim would hand the callee a bare pointer as `ap` while AAPCS64 expects
    // the ADDRESS OF a 5-field `__va_list`; the callee reads `__gr_top`/`__gr_offs` out of
    // arbitrary stack. Wrong output or crash, no diagnostic at any stage.
    // A `switch` over the closed enum makes that edit a COMPILE error instead — the same
    // protection `hir_to_mir`'s exhaustive dispatch already has, which is precisely why
    // the real lowering could never acquire this bug and this pass could.
    // Naming the offending strategy in the message matters too: "declares a different
    // strategy" sends a reader to diff two configs; naming it ends the search.
    switch (vaLayout->strategy) {
    case VaListStrategy::HomogeneousPointer:
        break;  // the one implemented model — fall through to emission below.
    case VaListStrategy::SysVRegisterSave:
    case VaListStrategy::Aapcs64DualCursor:
        emitErr(reporter,
                std::string("synthesizeStdioShim: the printf-family shim is implemented "
                            "only for the HomogeneousPointer va_list strategy, but the "
                            "active calling convention declares `vaListLayout.strategy = ")
                    + std::string(vaListStrategyName(vaLayout->strategy))
                    + "` — refusing to forward a va_list under an unimplemented model "
                      "rather than emitting a wrong-ABI forward "
                      "(D-FFI-PE-CRT-UCRT-MIGRATION)");
        return false;
    }

    // DETERMINISTIC emission order (unordered_map iteration is not stable; a shifting
    // function order would make the binary non-reproducible). Sort by pre-minted
    // SymbolId.v.
    std::vector<std::pair<std::uint32_t, std::string>> recipes(
        recipeBySymbol.begin(), recipeBySymbol.end());
    std::sort(recipes.begin(), recipes.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    // ── Types ──
    TypeId const voidTy = interner.primitive(TypeKind::Void);
    TypeId const i32Ty  = interner.primitive(TypeKind::I32);
    TypeId const u32Ty  = interner.primitive(TypeKind::U32);
    TypeId const u64Ty  = interner.primitive(TypeKind::U64);
    TypeId const charTy = interner.primitive(TypeKind::Char);
    TypeId const pVoid  = interner.pointer(voidTy);
    TypeId const pChar  = interner.pointer(charTy);

    // The FnSig CC is DOCUMENTARY ONLY — it is INERT, verified, not assumed: no tier
    // downstream of MIR ever reads a FnSig's `cc` scalar. `mir_to_lir` mentions `CallConv`
    // nowhere at all; the real call ABI is derived there from operand order + extern-import
    // status, and the target's convention is applied by `lir_callconv` via the
    // `callingConventionIndex` the driver resolved. Every other `fnSig()` callsite in the
    // tree records the same fact (`semantic_analyzer.cpp`: "Do NOT inspect this CallConv
    // field at MIR tier — it's a semantic placeholder, not the load-bearing CC"). CcMS64 is
    // written here rather than the tree-wide CcSysV placeholder purely for HONESTY, the
    // synth_threads_shim precedent: the only shipped recipe is pe64, whose host ABI this
    // names. Do NOT read this line as a live ABI decision — changing it changes nothing.
    CallConv const cc = CallConv::CcMS64;
    auto sig = [&](std::vector<TypeId> params, TypeId ret) -> TypeId {
        return interner.fnSig(params, ret, cc);
    };
    // The shim's OWN signatures are VARIADIC (the 4-arg fnSig overload) — `...` is a
    // marker, so the declared params are the FIXED arg count.
    auto vsig = [&](std::vector<TypeId> params, TypeId ret) -> TypeId {
        return interner.fnSig(params, ret, cc, /*isVariadic=*/true);
    };

    // The UCRT common-core signatures — each must match stdio.json's declared row exactly.
    // The BUFFERED pair take the same six parameters (the `_BufferCount` slot is what
    // separates them from the stream core), so ONE TypeId serves both:
    //   __stdio_common_vsprintf(u64 opts, char* buf, u64 count, char* fmt, void* loc, va_list ap)
    //   __stdio_common_vsscanf (u64 opts, char* buf, u64 count, char* fmt, void* loc, va_list ap)
    TypeId const coreBufferedSig = sig({u64Ty, pChar, u64Ty, pChar, pVoid, pVoid}, i32Ty);
    // The STREAM core — five parameters, no count (a stream is unbounded):
    //   __stdio_common_vfprintf(u64 opts, FILE* stream, char* fmt, void* loc, va_list ap)
    TypeId const coreVfprintfSig = sig({u64Ty, pVoid, pChar, pVoid, pVoid}, i32Ty);
    // The UCRT stdin/stdout/stderr ACCESSOR: `FILE* __acrt_iob_func(unsigned _Ix)`. A
    // `FILE*` is opaque at this tier (MIR has no such type and needs none — it is an
    // 8-byte pointer the shim only ever passes through), so it spells `ptr<void>`, the
    // same house convention the threads shim uses for `mtx_t*`.
    TypeId const iobFuncSig = sig({u32Ty}, pVoid);

    // DELIBERATE: every arm below EXCEPT `snprintf` RETURNS THE CORE'S VALUE DIRECTLY. For
    // the STREAM and SCANF cores that is also literally what the UCRT header inlines do
    // (`_vfprintf_l` / `_vsscanf_l` are a bare `return __stdio_common_v*(...)`), so those
    // need no argument. The `sprintf` arm is the one that differs: the UCRT's own `vsprintf`
    // inline instead ends with `return _Result < 0 ? -1 : _Result;`, so the omission there
    // looks at first glance like a missing normalization. It is not — the clamp
    // is an IDENTITY on every value this core can actually produce: the only negative it
    // EVER returns is exactly -1, and the one non-(-1) case is a POSITIVE would-be length
    // under STANDARD_SNPRINTF_BEHAVIOR, which the clamp leaves alone (and which is C99's
    // required snprintf truncation result). C also asks only for "a negative value" on
    // output error (7.21.6.6), never for -1 specifically. So an ICmp+Select would buy no
    // observable behavior.
    //
    // ★ SO WHY DOES THE `snprintf` ARM EMIT THE CLAMP ANYWAY? Because the argument above is
    // evidence about ONE ucrtbase build's five probed error paths, not a proof over the
    // core's whole domain — and `ucrtbase.dll` is a serviced OS component that this project
    // neither ships nor pins. For `sprintf` that gap is academic (the clamp is unreachable
    // either way, and C accepts any negative). For `snprintf` it is not: the return value is
    // LOAD-BEARING — callers size buffers with it, and `n == 0` exists purely to be asked
    // "how long would this be?" — so its contract should be a property of DSS's own emitted
    // code, not of an unaudited binary's internals. Emitting the clamp costs one ICmp and a
    // two-block tail and makes this shim byte-for-byte the header inline it stands in for.
    // STATED WITHOUT OVERCLAIM so nobody mistakes it for a bug fix: on every return value
    // measured to date the clamp is an IDENTITY, so no test can distinguish its presence
    // from its absence. It is DEFENSIVE FIDELITY to `ucrt/stdio.h`'s `vsnprintf`
    // inline, not an observed repair, and it is the one part of this recipe
    // with no red-on-disable witness.
    //
    // ★ REPRODUCING THE MEASUREMENT (it is an empirical claim, so it must be re-runnable —
    // a claim nobody can re-check is a claim that silently rots). In a scratch C program:
    // `LoadLibraryA("ucrtbase.dll")` + `GetProcAddress("__stdio_common_vsprintf")` (do NOT
    // link a CRT wrapper — the header inline is what is under test); install a no-op
    // `_set_invalid_parameter_handler` so the parameter-validation paths RETURN instead of
    // aborting the process; then drive, printing each returned int: NULL format · NULL
    // buffer · zero `_BufferCount` · a LEGACY_VSPRINTF_NULL_TERMINATION overrun (a format
    // whose output exceeds the buffer) · an unknown conversion (`"%q"`). All five return
    // exactly -1. Re-run with `_Options = STANDARD_SNPRINTF_BEHAVIOR` (bit 1) and a short
    // count to see the sole non-(-1) result: the positive would-be length.
    //
    // If a future arm adopts an _Options bit whose documented failure return is NOT -1,
    // re-measure by the recipe above and add the clamp THERE. (D-FFI-PE-CRT-UCRT-MIGRATION)

    // ── Rebuild the module (Mir is frozen): clone every existing function verbatim,
    //    then APPEND each shim, then clone globals — the shared rebuild idiom. ──
    MirBuilder builder;
    IdentityClonePolicy policy;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        opt::passes::MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(mir.funcAt(i));
    }

    // Resolve a UCRT core by NAME against the module's existing imports. These are
    // ORDINARY stdio.json symbol rows (user decision) — NOT minted here — so the
    // eager-import law has already proven each is a real export of the bound library.
    // An absent name means stdio.json declared a `synthesize` row without its core: a
    // descriptor/pass drift, and exactly the kind of gap that would otherwise produce a
    // silently-undefined shim.
    std::unordered_map<std::string, SymbolId> helperSyms;
    for (auto const& e : externImports)
        if (!e.isData) helperSyms.emplace(e.mangledName, e.symbol);

    // ★ RESOLUTION IS A PURE LOOKUP — it emits NOTHING and each arm runs it BEFORE opening
    // its function, so a failure returns while the builder is still structurally clean. An
    // earlier shape reported the failure, returned a default-constructed `MirInstId`, and
    // let the caller use it as operand 0 of the `Call`: `MirBuilder::checkSameModule_`
    // waves an untagged id through (`arenaTag == 0` passes, for literal-id test
    // ergonomics), so that Call was appended SILENTLY with a dangling callee. It was
    // harmless only by accident of statement order downstream — a structurally invalid
    // instruction must not exist in the builder at all, however short its life.
    auto coreSym = [&](char const* name) -> std::optional<SymbolId> {
        auto const it = helperSyms.find(name);
        if (it == helperSyms.end()) {
            emitErr(reporter,
                    std::string{"synthesizeStdioShim: the UCRT core '"} + name
                        + "' is not imported by this module — stdio.json must declare it "
                          "as a pe symbol row alongside the `synthesize` row that needs it "
                          "(D-FFI-PE-CRT-UCRT-MIGRATION / D-FFI-DESCRIPTOR-EAGER-IMPORT)");
            return std::nullopt;
        }
        return it->second;
    };

    auto konst = [&](std::int64_t v, TypeKind core, TypeId ty) -> MirInstId {
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = core;
        return builder.addConst(std::move(lit), ty);
    };
    auto u64c  = [&](std::uint64_t v) {
        return konst(static_cast<std::int64_t>(v), TypeKind::U64, u64Ty);
    };
    auto u32c  = [&](std::uint32_t v) {
        return konst(static_cast<std::int64_t>(v), TypeKind::U32, u32Ty);
    };
    auto i32c  = [&](std::int32_t v) {
        return konst(static_cast<std::int64_t>(v), TypeKind::I32, i32Ty);
    };
    auto nullP = [&]() { return konst(0, TypeKind::Ptr, pVoid); };

    // Open a shim function + its ENTRY block. Every printf-family recipe is single-block
    // except `snprintf`, whose return clamp adds a two-block tail (created in-arm, exactly
    // as `synthesizeThreadsShim`'s `thrd_join` does).
    auto begin = [&](SymbolId sym, TypeId fnSig) {
        (void)builder.addFunction(fnSig, sym, SymbolBinding::Global,
                                  SymbolVisibility::Default);
        MirBlockId const entry = builder.createBlock(StructCfMarker::EntryBlock);
        builder.beginBlock(entry);
    };

    // The first vararg under the HomogeneousPointer model — which IS the `va_list` value.
    // The leaf's presence is also lir_callconv's prologue-spill signal.
    //
    // ★★ TWO LEAVES, AND THE CHOICE IS A CORRECTNESS FORK, NOT A STYLE ONE. HomogeneousPointer
    // does NOT imply Win64: `apple_arm64` declares the same strategy with
    // `variadicUsesOverflowBase: true`. This reproduces `hir_to_mir.cpp`'s HomogeneousPointer
    // `va_start` arm EXACTLY, reading the SAME field off the SAME `VaListLayout` struct —
    // which is the whole reason this pass takes the layout rather than the bare strategy:
    //   * false (Win64) — `&home[namedArgCount]`: named args are SPILLED to a home block
    //     contiguous with the overflow area, so the first vararg is just past them. The
    //     payload carries that slot count.
    //   * true (Apple arm64) — the OVERFLOW base, NO payload: Apple has no home area and
    //     forces every vararg onto the stack (`variadicArgsAlwaysStack`), so the first
    //     vararg IS the overflow base and the named args stay in registers, read via SSA.
    // Emitting the home leaf on an overflow-base target would silently point `ap` at
    // named-arg storage — a wrong-output miscompile with no diagnostic anywhere, which is
    // exactly the class this project refuses to ship. It is unreachable today (no darwin
    // stdio synthesize recipe exists) and is written correctly ANYWAY, because the first
    // such recipe must not have to remember this file.
    auto vaStart = [&](std::uint32_t namedArgCount) -> MirInstId {
        return vaLayout->variadicUsesOverflowBase
                   ? builder.addInst(MirOpcode::VaOverflowArgAreaAddr,
                                     std::array<MirInstId, 0>{}, pVoid)
                   : builder.addInst(MirOpcode::VaHomeArgAreaAddr,
                                     std::array<MirInstId, 0>{}, pVoid,
                                     /*payload=*/namedArgCount);
    };

    for (auto const& [symV, recipe] : recipes) {
        SymbolId const sym{symV};

        if (recipe == "printf") {
            // int printf(char const* fmt, ...)
            //   -> __stdio_common_vfprintf(0, __acrt_iob_func(1), fmt, NULL, ap)
            // `printf` IS `fprintf` to stdout, and post-UCRT-flip `stdout` is the accessor
            // call — there is no `_iob[]` array to index any more. BOTH cores are resolved
            // BEFORE the function is opened, so a missing either one leaves the builder
            // structurally clean (see the `coreSym` note above).
            auto const iob  = coreSym("__acrt_iob_func");
            if (!iob.has_value()) return false;    // reported; nothing emitted yet.
            auto const core = coreSym("__stdio_common_vfprintf");
            if (!core.has_value()) return false;   // reported; nothing emitted yet.
            begin(sym, vsig({pChar}, i32Ty));
            MirInstId const fmt = builder.addArg(0, pChar);
            MirInstId const ap  = vaStart(1);
            std::array<MirInstId, 2> iobOps{
                builder.addGlobalAddr(*iob, interner.pointer(iobFuncSig)), u32c(kIobStdout)};
            MirInstId const strm =
                builder.addInst(MirOpcode::Call, iobOps, pVoid, /*payload=*/0);
            std::array<MirInstId, 6> ops{
                builder.addGlobalAddr(*core, interner.pointer(coreVfprintfSig)),
                u64c(kOptNone),
                strm,
                fmt,
                nullP(),              // _Locale = NULL (the ambient locale)
                ap};
            builder.addReturn(builder.addInst(MirOpcode::Call, ops, i32Ty, /*payload=*/0));
            continue;
        }

        if (recipe == "fprintf") {
            // int fprintf(FILE* stream, char const* fmt, ...)
            //   -> __stdio_common_vfprintf(0, stream, fmt, NULL, ap)
            auto const core = coreSym("__stdio_common_vfprintf");
            if (!core.has_value()) return false;   // reported; nothing emitted yet.
            begin(sym, vsig({pVoid, pChar}, i32Ty));
            MirInstId const strm = builder.addArg(0, pVoid);
            MirInstId const fmt  = builder.addArg(1, pChar);
            MirInstId const ap   = vaStart(2);
            std::array<MirInstId, 6> ops{
                builder.addGlobalAddr(*core, interner.pointer(coreVfprintfSig)),
                u64c(kOptNone),
                strm,
                fmt,
                nullP(),              // _Locale = NULL (the ambient locale)
                ap};
            builder.addReturn(builder.addInst(MirOpcode::Call, ops, i32Ty, /*payload=*/0));
            continue;
        }

        if (recipe == "vfprintf") {
            // int vfprintf(FILE* stream, char const* fmt, va_list ap)
            //   -> __stdio_common_vfprintf(0, stream, fmt, NULL, ap)
            // ★ THE ONE ARM WITH NO VA LEAF, and deliberately so — see the header. This
            // function is NOT variadic (C 7.21.6.8): `ap` is a DECLARED PARAMETER already
            // pointing at the CALLER's first unnamed argument, so it is forwarded verbatim
            // as `Arg 2`. A `Va*ArgAreaAddr` here would re-derive `ap` from THIS frame —
            // which has no varargs at all — and would additionally hand lir_callconv a
            // prologue-spill signal for a non-variadic function. Its signature is therefore
            // the NON-variadic `sig`, not `vsig`. `ap` spells `ptr<void>` because the pass
            // has already refused every strategy but HomogeneousPointer, under which a
            // `va_list` IS one pointer (Win64 `char*`; Apple arm64 the same shape).
            auto const core = coreSym("__stdio_common_vfprintf");
            if (!core.has_value()) return false;   // reported; nothing emitted yet.
            begin(sym, sig({pVoid, pChar, pVoid}, i32Ty));
            MirInstId const strm = builder.addArg(0, pVoid);
            MirInstId const fmt  = builder.addArg(1, pChar);
            MirInstId const ap   = builder.addArg(2, pVoid);
            std::array<MirInstId, 6> ops{
                builder.addGlobalAddr(*core, interner.pointer(coreVfprintfSig)),
                u64c(kOptNone),
                strm,
                fmt,
                nullP(),              // _Locale = NULL (the ambient locale)
                ap};
            builder.addReturn(builder.addInst(MirOpcode::Call, ops, i32Ty, /*payload=*/0));
            continue;
        }

        if (recipe == "sprintf") {
            // int sprintf(char* buf, char const* fmt, ...)
            //   -> __stdio_common_vsprintf(LEGACY_NULLTERM, buf, (size_t)-1, fmt, NULL, ap)
            // The (size_t)-1 count is UCRT's UNBOUNDED sentinel — sprintf has no limit.
            auto const core = coreSym("__stdio_common_vsprintf");
            if (!core.has_value()) return false;   // reported; nothing emitted yet.
            begin(sym, vsig({pChar, pChar}, i32Ty));
            MirInstId const buf = builder.addArg(0, pChar);
            MirInstId const fmt = builder.addArg(1, pChar);
            MirInstId const ap  = vaStart(2);
            std::array<MirInstId, 7> ops{
                builder.addGlobalAddr(*core, interner.pointer(coreBufferedSig)),
                u64c(kOptLegacyVsprintfNullTermination),
                buf,
                u64c(kBufferCountUnbounded),
                fmt,
                nullP(),              // _Locale = NULL (the ambient locale)
                ap};
            builder.addReturn(builder.addInst(MirOpcode::Call, ops, i32Ty, /*payload=*/0));
            continue;
        }

        if (recipe == "snprintf") {
            // int snprintf(char* buf, size_t n, char const* fmt, ...)
            //   r = __stdio_common_vsprintf(STANDARD_SNPRINTF, buf, n, fmt, NULL, ap)
            //   return r < 0 ? -1 : r
            // ★ THE SAME CORE AS `sprintf`, differing in exactly the two arguments that
            // carry snprintf's semantics — and that is forced, not chosen: ucrtbase exports
            // no `__stdio_common_vsnprintf` at all (see the header). This mirrors
            // `ucrt/stdio.h`'s own `vsnprintf` inline, which is literally how a
            // real MSVC build spells
            // `snprintf`.
            //   * `_Options` = STANDARD_SNPRINTF_BEHAVIOR — WITHOUT this bit the core is
            //     the pre-C99 `_snprintf`, which returns -1 on truncation where C99 wants
            //     the would-be length. MEASURED both ways; see the constant's own note for
            //     what did and did NOT reproduce.
            //   * `_BufferCount` = the caller's REAL `n`, not `kBufferCountUnbounded`. The
            //     sentinel would make the core write past the caller's buffer on any output
            //     longer than `n` — a silent heap/stack overrun, the worst failure mode in
            //     this file, since it corrupts memory rather than producing wrong text.
            auto const core = coreSym("__stdio_common_vsprintf");
            if (!core.has_value()) return false;   // reported; nothing emitted yet.
            begin(sym, vsig({pChar, u64Ty, pChar}, i32Ty));
            MirInstId const buf = builder.addArg(0, pChar);
            MirInstId const n   = builder.addArg(1, u64Ty);
            MirInstId const fmt = builder.addArg(2, pChar);
            MirInstId const ap  = vaStart(3);      // THREE named args, not two.
            std::array<MirInstId, 7> ops{
                builder.addGlobalAddr(*core, interner.pointer(coreBufferedSig)),
                u64c(kOptStandardSnprintfBehavior),
                buf,
                n,
                fmt,
                nullP(),              // _Locale = NULL (the ambient locale)
                ap};
            MirInstId const r =
                builder.addInst(MirOpcode::Call, ops, i32Ty, /*payload=*/0);
            // The `_Result < 0 ? -1 : _Result` tail. Two blocks with their own returns
            // rather than a select — MIR has no Select opcode, and the `thrd_join`
            // precedent is exactly this shape. Markers are left default and canonicalized
            // by the module-wide `rederiveStructCfMarkers` after `finish()`; never
            // hand-stamp IfThen/IfJoin.
            std::array<MirInstId, 2> lt{r, i32c(0)};
            MirInstId const isNeg = builder.addInst(MirOpcode::ICmpSlt, lt,
                                                    interner.primitive(TypeKind::Bool));
            MirBlockId const negBB = builder.createBlock();
            MirBlockId const okBB  = builder.createBlock();
            builder.addCondBr(isNeg, negBB, okBB);
            builder.beginBlock(negBB);
            builder.addReturn(i32c(-1));
            builder.beginBlock(okBB);
            builder.addReturn(r);
            continue;
        }

        if (recipe == "sscanf") {
            // int sscanf(char const* buf, char const* fmt, ...)
            //   -> __stdio_common_vsscanf(0, buf, (size_t)-1, fmt, NULL, ap)
            // Same six-parameter shape as the vsprintf core (hence the shared TypeId), and
            // the same UNBOUNDED sentinel — a `sscanf` source string is NUL-terminated, not
            // length-counted. `_Options` MUST be 0 here: bit 0 (SECURECRT) would make this
            // `sscanf_s`, which consumes an extra buffer-size argument out of `ap` after
            // every `%s` — silent argument-stream corruption, not a diagnostic.
            auto const core = coreSym("__stdio_common_vsscanf");
            if (!core.has_value()) return false;   // reported; nothing emitted yet.
            begin(sym, vsig({pChar, pChar}, i32Ty));
            MirInstId const buf = builder.addArg(0, pChar);
            MirInstId const fmt = builder.addArg(1, pChar);
            MirInstId const ap  = vaStart(2);
            std::array<MirInstId, 7> ops{
                builder.addGlobalAddr(*core, interner.pointer(coreBufferedSig)),
                u64c(kOptNone),
                buf,
                u64c(kBufferCountUnbounded),
                fmt,
                nullP(),              // _Locale = NULL (the ambient locale)
                ap};
            builder.addReturn(builder.addInst(MirOpcode::Call, ops, i32Ty, /*payload=*/0));
            continue;
        }

        // The anti-silent-gap backstop. Those six ARE the whole shipped stdio vocabulary
        // (see the header): the loader's closed `kRecipes` table admits no other stdio id,
        // so reaching here means that table and this switch have drifted apart.
        emitErr(reporter,
                "synthesizeStdioShim: no synth arm for recipe id '" + recipe
                    + "' (D-FFI-PE-CRT-UCRT-MIGRATION vocab/switch drift)");
        return false;
    }

    opt::passes::cloneGlobalsVerbatim(mir, builder);
    mir = std::move(builder).finish();
    // Canonicalize StructCfMarkers module-wide from the CFG. NO LONGER MERELY DEFENSIVE:
    // the `snprintf` arm's clamp creates two blocks with DEFAULT markers, so this call is
    // what makes them canonical (IfThen/IfJoin) and the merge-path MirVerifier's
    // stored==derived check pass — the same role it plays for `thrd_join` in
    // synth_threads_shim. Idempotent for every other function (single-block recipes +
    // clones re-derive to their existing markers).
    rederiveStructCfMarkers(mir);
    return true;
}

} // namespace dss
