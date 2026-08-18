#include "mir/merge/synth_pe_startup.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"       // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/mangling/c_mangle.hpp"   // applyCMangling (per-format CRT import names)
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"   // rederiveStructCfMarkers (multi-block synth body)
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>   // std::max
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>     // std::move

namespace dss {

namespace {

// A verbatim clone policy: keep EVERY block (no drop, no rewrite) so each
// existing function is re-added to the builder unchanged. Everything else takes
// the MirRebuildPolicy base defaults (accept all phi incomings, no substitution).
class IdentityClonePolicy final : public opt::passes::MirRebuildPolicy {
public:
    // The rebuild DRIVER this policy belongs to — printed by every
    // `MirFunctionRebuilder` fatal.
    // See D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS (kept on ONE line: a
    // wrapped anchor name mints a SECOND, unregistered anchor).
    // Deliberately NOT a `kPassNameTable` pass name: this is a MIR merge step,
    // so a fatal saying `[pass=SynthPeStartup]` tells the reader the abort did
    // not come from the optimizer pipeline at all.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SynthPeStartup";
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> blocks;
        std::uint32_t const n = src.funcBlockCount(fn);
        blocks.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            blocks.push_back(src.funcBlockAt(fn, i));
        }
        return blocks;
    }
};

void emitErr(DiagnosticReporter& rep, DiagnosticCode code, std::string msg) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// Max SymbolId.v across every defined function, every module GLOBAL, AND every
// extern import — the floor for minting fresh synthetic symbols (mirrors the
// entry-trampoline's maxExistingSymbolIdV, but at the MIR tier where there is no
// AssembledModule).
//
// The globals scan is LOAD-BEARING, not defensive: the merged SymbolId space is
// unified + monotonic, and synthetic string-literal globals are minted ABOVE every
// function/extern id (compile_pipeline's `syntheticSymbolFloor`). So in a real
// program (sqlite) the single HIGHEST SymbolId is almost always a global, not a
// function. Omitting globals here (as the sibling entry_trampoline's maxExisting…
// pointedly does NOT — it scans dataItems for exactly this reason) would let
// synthetic symbols duplicate a real global's id, and the linker would silently
// mis-bind the entry onto that DATA symbol — an entry that "runs" a string literal.
[[nodiscard]] std::uint32_t
maxSymbolIdV(Mir const& mir, std::vector<ExternImport> const& externs) {
    std::uint32_t maxV = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        maxV = std::max(maxV, mir.funcSymbol(mir.funcAt(i)).v);
    }
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        maxV = std::max(maxV, mir.globalSymbol(mir.globalAt(i)).v);
    }
    for (auto const& e : externs) maxV = std::max(maxV, e.symbol.v);
    return maxV;
}

// ⓘ THE SIGNATURE CLASSIFIER AND THE DECLARED-SET RENDERER USED TO LIVE HERE
// AND ARE DELIBERATELY GONE. This pass classified the resolved entry's MIR
// signature into `EntryParamShape`/`EntryReturnShape` and refused anything the
// FORMAT did not declare. Both the classification and the refusal moved out:
//
//   * classification → the SEMANTIC tier, which has the declarator and therefore
//     a real source span (`S_EntryShapeNotDeclared`), and which compares against
//     the SOURCE LANGUAGE's declared entry rows — the one owner of an entry
//     signature. Asking a linker format whether `fn(i32, ptr-ptr-u16) -> i32` is
//     an entry signature was asking the wrong party: that is how C spells an
//     entry, not something a loader has an opinion about.
//   * candidacy → ENTRY RESOLUTION, which intersects those language rows with the
//     format's declared `entryVerbs` and so decides the verb before this pass
//     runs.
//
// ⚠ A CLASSIFIER HERE WOULD BE A SECOND OWNER. It would re-derive the
// signature→verb mapping the language config already declares, and the two would
// drift with nothing to catch it. The verb is an INPUT to this function.

// The entry's own declared calling convention, read out of its interned
// signature (`fnSig` encodes `scalars = [(int)cc, isVariadic]`).
//
// ★ WHY NOT A LITERAL. c111 spelled `CallConv::CcMS64` here, which was correct
// for the one format that used the pass and became a latent
// format-identity assumption the moment the pass went format-agnostic. The
// synthesized init CALLS the user entry and is CALLED by the entry trampoline,
// so its convention must be the entry's — that is a fact about the program being
// compiled, available right here, and never a guess.
[[nodiscard]] std::optional<CallConv>
entryCallConv(TypeInterner const& interner, TypeId entrySig) {
    auto const sc = interner.scalars(entrySig);
    if (sc.empty()) return std::nullopt;
    auto const raw = sc[0];
    if (raw < 0 || raw > static_cast<std::int64_t>(CallConv::CcSpirv)) {
        return std::nullopt;
    }
    return static_cast<CallConv>(raw);
}

} // namespace

bool realizeEntryShape(Mir&                              mir,
                       TypeInterner&                     interner,
                       std::optional<SymbolId>&          userEntrySymbol,
                       std::vector<ExternImport>&        externImports,
                       EntryMaterialization              verb,
                       std::optional<ProcessArgs> const& processArgs,
                       CSymbolDecorationScheme           scheme,
                       std::string_view                  formatName,
                       DiagnosticReporter&               reporter) {
    // No resolved entry (a library TU with no `main`) — nothing to gate and
    // nothing to materialize.
    if (!userEntrySymbol.has_value()) return true;

    // Locate the entry function + its signature in the module.
    MirFuncId entryFn{};
    TypeId    entrySig = InvalidType;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == userEntrySymbol->v) {
            entryFn  = f;
            entrySig = mir.funcSignature(f);
            break;
        }
    }
    // The entry symbol isn't a defined function here (e.g. resolved to an extern)
    // — leave it; the trampoline's own resolution reports that case with
    // `K_EntryPointResolvesToExtern`, which is the accurate diagnostic.
    if (!entryFn.valid() || !entrySig.valid()) return true;

    // ── MATERIALIZE: dispatch on the DECIDED verb × the declared MECHANISM ──
    //
    // The verb says WHICH arguments the entry needs; the mechanism says HOW this
    // format obtains them. Both closed enums, both config-declared, neither
    // derived here.
    //
    // ⓘ THE "EMPTY DECLARED SET" EARLY RETURN THAT USED TO GUARD THIS IS GONE
    // BECAUSE ITS JOB MOVED, NOT BECAUSE IT WAS WRONG. A relocatable `.o` build
    // DOES arrive with a resolved `main` — the entry-name scan knows nothing about
    // object formats — and the old gate had to skip such a build explicitly or it
    // refused every object-file build in the tree (MEASURED while that gate was
    // written). Entry resolution now reads the SAME predicate, the format's
    // `entryVerbs` being empty, and a `.o` build's entry is resolved with no verb
    // at all → `EntryMaterialization::None` → the no-op return just below. The
    // predicate is still the DECLARED SET being empty, never `isExecFlavor()` and
    // never a format name.
    if (verb == EntryMaterialization::None) {
        return true;   // a no-arg entry needs no setup at all.
    }
    if (!processArgs.has_value()) {
        // Mach-O: dyld CALLS an LC_MAIN entry with argc/argv already in the
        // argument registers, so "no mechanism" is a real ANSWER and the
        // trampoline's pass-through is correct. DOCUMENTED, and pinned in-tree
        // by `ProcessArgsSubstrate.ShippedMachoExecsDeclareNoneAndPe…`.
        return true;
    }
    switch (processArgs->mechanism) {
    case ArgsMechanism::StackVector:
        // The entry trampoline materializes argc/argv from the untouched
        // process-entry stack (SysV AMD64 psABI §3.4.1 / AAPCS64 Linux). Nothing
        // to synthesize at the MIR tier.
        return true;
    case ArgsMechanism::CrtArgvAccessors:
        break;   // the one arm this pass emits — falls through below.
    case ArgsMechanism::None:
        // The loader rejects `mechanism: "none"`, and `optional` empty is how
        // "no mechanism" is spelled, so a `None` reaching here is a hand-built
        // schema. Refuse rather than silently skip the setup a matched verb asked
        // for — a skipped setup calls the entry on uninitialized registers.
        emitErr(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                std::format(
                    "realizeEntryShape: format '{}' declares a `processArgs` "
                    "block whose mechanism is the invalid `none` sentinel while "
                    "the resolved program entry needs the '{}' materialization "
                    "verb — argument setup would be silently skipped, calling "
                    "the entry on UNINITIALIZED registers. "
                    "(D-RUNTIME-MAIN-ARGC-ARGV.)",
                    formatName, entryMaterializationName(verb)));
        return false;
    }

    ProcessArgs const& pa = *processArgs;

    // WIDE vs NARROW comes from the VERB — i.e. from the resolved entry's own
    // signature, exactly as c111 required, but now via the language's declared
    // signature→verb mapping instead of an inline TypeKind inspection here. argc
    // is SHARED between the two worlds (MEASURED, PROBE-0), so one accessor name
    // serves both.
    bool const wide = (verb == EntryMaterialization::ArgcWargv);
    std::string const configureName =
        wide ? pa.configureWideArgvFn : pa.configureNarrowArgvFn;
    std::string const argvAccessorName =
        wide ? pa.wideArgvAccessorFn : pa.narrowArgvAccessorFn;

    // ── THE ARITY BACKSTOP, AND IT IS NOT THE RETIRED GATE ────────────────
    //
    // Every argc/argv verb materializes into the entry's FIRST TWO parameters, so
    // an entry reaching here with fewer than two is an internal inconsistency
    // between the verb entry resolution decided and the signature actually in the
    // merged MIR. On the normal path that CANNOT happen: the semantic tier already
    // matched this definition against the language row the verb came from. So this
    // fires only for a signature that never passed through the semantic tier —
    // a hand-built `Mir`, or an entry arriving from an object DSS did not compile.
    //
    // ★ WHY IT IS NOT A SECOND SIGNATURE OWNER, which is the thing to check
    // before touching it: it does not ask "is this a legal entry signature" (the
    // language owns that, with a span). It asks "can the arguments I am about to
    // materialize physically land in this function", which is a fact about the
    // MIR in hand and about nothing declared anywhere. Without it, `params[0]` /
    // `params[1]` below are an out-of-bounds read — a silent one, on the program
    // entry.
    //
    // ⚠ AND IT CANNOT COVER THE FOREIGN-OBJECT CASE, so do not read it as
    // closing that gap. `AssembledFunction` carries no TypeId and no signature;
    // a pre-built `.obj`/`.lib` member's entry has no C signature IN THE INPUT at
    // all, so there is nothing here to check its arity against. The recorded
    // long-term closure is a SUPERSET MATERIALIZATION — declaring a 3-parameter
    // row with an `argc-argv-envp` verb on the formats whose loader ALREADY
    // supplies envp in the argument register (glibc's `__libc_start_main` passes
    // three; Darwin's LC_MAIN is called with four), which makes the undetectable
    // case harmless at every tier instead of faulting. It is gated on a RUN
    // WITNESS and is NOT implemented (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE).
    auto const params = interner.fnParams(entrySig);
    if (params.size() < 2) {
        emitErr(reporter, DiagnosticCode::K_EntryVerbUnmaterializable,
                std::format(
                    "realizeEntryShape: the resolved program entry needs the "
                    "'{}' materialization verb, which materializes two arguments "
                    "into the entry's first two parameters, but the entry's "
                    "signature in the merged module declares {} parameter(s). "
                    "The verb and the signature disagree, so the arguments have "
                    "nowhere to land. On any source DSS compiled this is "
                    "impossible — the semantic tier matched the definition "
                    "against the very language row this verb came from — so this "
                    "signature did not pass through it (a hand-built module, or "
                    "an entry from a pre-built object). "
                    "(D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE.)",
                    entryMaterializationName(verb), params.size()));
        return false;
    }

    TypeId const argcTy = params[0];   // int      (language-row-matched: I32)
    TypeId const argvTy = params[1];   // char**   (language-row-matched: ptr→ptr→…)

    auto const ccOpt = entryCallConv(interner, entrySig);
    if (!ccOpt.has_value()) {
        emitErr(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                std::format(
                    "realizeEntryShape: the resolved program entry of format "
                    "'{}' carries no readable calling convention in its interned "
                    "signature, so the synthesized pre-main init cannot be given "
                    "the SAME convention the entry trampoline will call it with. "
                    "Refusing rather than assuming one — a convention mismatch on "
                    "the program entry is a silent ABI break.", formatName));
        return false;
    }
    CallConv const cc = *ccOpt;

    // Types for the synth body.
    TypeId const i32Ty     = interner.primitive(TypeKind::I32);
    TypeId const i64Ty     = interner.primitive(TypeKind::I64);
    TypeId const boolTy    = interner.primitive(TypeKind::Bool);
    TypeId const pArgcTy   = interner.pointer(argcTy);   // int*    (__p___argc)
    TypeId const pArgvTy   = interner.pointer(argvTy);   // char*** (__p___argv)
    // errno_t _configure_{narrow,wide}_argv(int mode)
    std::array<TypeId, 1> const cfgParams{i32Ty};
    TypeId const cfgSig    = interner.fnSig(cfgParams, i32Ty, cc);
    // int* __p___argc(void) / char*** __p___argv(void)
    TypeId const argcAccSig = interner.fnSig({}, pArgcTy, cc);
    TypeId const argvAccSig = interner.fnSig({}, pArgvTy, cc);
    TypeId const synthSig   = interner.fnSig({}, i32Ty, cc);
    TypeId const pCfgSig    = interner.pointer(cfgSig);
    TypeId const pArgcAccSig = interner.pointer(argcAccSig);
    TypeId const pArgvAccSig = interner.pointer(argvAccSig);
    TypeId const pEntrySig   = interner.pointer(entrySig);

    // Mint fresh symbols: the synth function + the three CRT imports.
    std::uint32_t const maxV = maxSymbolIdV(mir, externImports);
    SymbolId const synthSym{maxV + 1};
    SymbolId const cfgSym{maxV + 2};
    SymbolId const argcAccSym{maxV + 3};
    SymbolId const argvAccSym{maxV + 4};

    // Register the CRT imports (all FUNCTION imports, not data). Their library is
    // the ROLE-resolved image — see `RuntimeLibraryRole`; nothing here spells a
    // DLL name.
    //
    // ★★ THE NAMES ARE C-MANGLED FOR THE ACTIVE FORMAT, and this is a REAL
    // DIFFERENCE from c111, not tidying. c111 wrote `crtName` into the import
    // VERBATIM, which was harmless only because the one format using the pass
    // declares `cSymbolDecoration: {"scheme": "none"}` — an assumption invisible
    // in the code and untrue the moment a decorating format declares the
    // mechanism. Routing through the SAME `applyCMangling` the FFI ingest uses
    // means a `leading-underscore` format would request `__p___argc` correctly
    // instead of silently asking for an undecorated name that does not exist.
    // Getting this wrong is the `_exit`-vs-`exit` class of mis-bind, which is not
    // hypothetical: MEASURED 2026-08-06, ucrtbase exports `exit`, `_exit` AND
    // `_Exit` as three DISTINCT functions, so an off-by-one-underscore request
    // binds to a DIFFERENT function rather than failing to resolve.
    auto addImport = [&](SymbolId sym, std::string const& name) {
        ExternImport imp;
        imp.symbol      = sym;
        imp.mangledName = dss::ffi::applyCMangling(name, scheme);
        imp.libraryPath = pa.crtLibraryPath;
        imp.isData      = false;
        externImports.push_back(std::move(imp));
    };
    addImport(cfgSym,     configureName);
    addImport(argcAccSym, pa.argcAccessorFn);
    addImport(argvAccSym, argvAccessorName);

    // Rebuild the module (Mir is frozen): clone every existing function verbatim,
    // then APPEND the synth function, then clone globals — the prune_unreachable
    // rebuild idiom.
    MirBuilder builder;
    IdentityClonePolicy policy;
    for (std::uint32_t i = 0; i < nf; ++i) {
        opt::passes::MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(mir.funcAt(i));
    }

    // _dss_pe_start(): the pre-main init. Global-bound so DCE (which runs AFTER
    // this synthesis on the single-CU seam) keeps it — it is also the retargeted
    // program entry.
    (void)builder.addFunction(synthSig, synthSym, SymbolBinding::Global,
                              SymbolVisibility::Default);
    MirBlockId const entryBlk = builder.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const failBlk  = builder.createBlock();
    MirBlockId const callBlk  = builder.createBlock();

    auto konst = [&](std::int64_t v, TypeKind core, TypeId ty) {
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = core;
        return builder.addConst(std::move(lit), ty);
    };

    builder.beginBlock(entryBlk);
    // _configure_{narrow,wide}_argv(<argvMode>). The RESULT IS DELIBERATELY
    // IGNORED: MEASURED 2026-08-10, every valid `_crt_argv_mode` returns
    // `errno_t` 0 — including mode 0, which returns 0 while yielding
    // `argv == NULL`. Branching on it would be a guard that asserts nothing. The
    // real check is the argv test below.
    {
        MirInstId const cfgAddr = builder.addGlobalAddr(cfgSym, pCfgSig);
        MirInstId const mode    = konst(static_cast<std::int64_t>(pa.argvMode),
                                        TypeKind::I32, i32Ty);
        std::array<MirInstId, 2> call{cfgAddr, mode};
        (void)builder.addInst(MirOpcode::Call, call, i32Ty, /*payload=*/0);
    }
    // argc = *__p___argc();  argv = *__p___argv();
    // EXACTLY ONE dereference each — the accessors return the ADDRESS of the
    // CRT's state (`ucrt/stdlib.h:1144-1145`: `int*` and `char***`), which is
    // why a second load would read the first element instead of the vector.
    MirInstId argc{};
    MirInstId argv{};
    {
        MirInstId const accAddr = builder.addGlobalAddr(argcAccSym, pArgcAccSig);
        std::array<MirInstId, 1> call{accAddr};
        MirInstId const slot =
            builder.addInst(MirOpcode::Call, call, pArgcTy, /*payload=*/0);
        argc = builder.addInst(MirOpcode::Load,
                               std::array<MirInstId, 1>{slot}, argcTy);
    }
    {
        MirInstId const accAddr = builder.addGlobalAddr(argvAccSym, pArgvAccSig);
        std::array<MirInstId, 1> call{accAddr};
        MirInstId const slot =
            builder.addInst(MirOpcode::Call, call, pArgvTy, /*payload=*/0);
        argv = builder.addInst(MirOpcode::Load,
                               std::array<MirInstId, 1>{slot}, argvTy);
    }
    // if (argv == NULL) → the failure arm. Compared through PtrToInt against a
    // machine-word zero (the shape `synth_threads_shim` already uses for a
    // handle-is-null test), so no null-pointer literal encoding is needed.
    {
        MirInstId const argvInt = builder.addInst(
            MirOpcode::PtrToInt, std::array<MirInstId, 1>{argv}, i64Ty);
        std::array<MirInstId, 2> cmp{argvInt,
                                     konst(0, TypeKind::I64, i64Ty)};
        MirInstId const isNull =
            builder.addInst(MirOpcode::ICmpEq, cmp, boolTy);
        builder.addCondBr(isNull, failBlk, callBlk);
    }

    // The failure arm RETURNS the format's declared status rather than calling
    // anything: the value leaves through the format's already-wired `processExit`
    // path (the trampoline moves this return value into the exit mechanism's
    // status register), so this needs no second exit import — and it is not an
    // `Unreachable`, which in a REACHABLE position would invite an optimizer to
    // treat the guarded branch as dead and delete the check.
    builder.beginBlock(failBlk);
    builder.addReturn(konst(
        static_cast<std::int64_t>(pa.argvUnavailableExitStatus),
        TypeKind::I32, i32Ty));

    // return entry(argc, argv);
    builder.beginBlock(callBlk);
    {
        MirInstId const entryAddr =
            builder.addGlobalAddr(*userEntrySymbol, pEntrySig);
        MirInstId const ret = builder.addInst(
            MirOpcode::Call,
            std::array<MirInstId, 3>{entryAddr, argc, argv},
            i32Ty, /*payload=*/0);
        builder.addReturn(ret);
    }

    opt::passes::cloneGlobalsVerbatim(mir, builder);
    mir = std::move(builder).finish();

    // Canonicalize StructCfMarkers module-wide from the CFG. REQUIRED because the
    // synth body is MULTI-BLOCK now (the argv gate) and the single-CU seam runs
    // POST-optimize, so no later pass re-derives them — and `MirVerifier`'s
    // marker-equality check rejects a module whose raw markers are not already
    // canonical. Same call, same reason, as `synthesizeThreadsShim`'s
    // multi-block bodies.
    rederiveStructCfMarkers(mir);

    // Retarget the program entry to the synth function.
    userEntrySymbol = synthSym;
    return true;
}

} // namespace dss
