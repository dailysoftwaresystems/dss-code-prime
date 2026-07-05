#include "mir/merge/synth_pe_startup.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"       // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>   // std::max
#include <array>
#include <cstdint>
#include <string>
#include <utility>     // std::move

namespace dss {

namespace {

// A verbatim clone policy: keep EVERY block (no drop, no rewrite) so each
// existing function is re-added to the builder unchanged. Everything else takes
// the MirRebuildPolicy base defaults (accept all phi incomings, no substitution).
class IdentityClonePolicy final : public opt::passes::MirRebuildPolicy {
public:
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

void emitErr(DiagnosticReporter& rep, std::string msg) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::K_NoMatchingObjectFormat;
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
// synthSym / crtSym duplicate a real global's id, and the linker would silently
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

} // namespace

bool synthesizePeStartup(Mir&                        mir,
                         TypeInterner&               interner,
                         std::optional<SymbolId>&    userEntrySymbol,
                         std::vector<ExternImport>&  externImports,
                         ProcessArgs const&          processArgs,
                         DiagnosticReporter&         reporter) {
    // Only the Windows CRT out-parameter mechanism synthesizes; every other
    // format (StackVector elf, none for macho, absent) is a clean no-op.
    if (processArgs.mechanism != ArgsMechanism::CrtOutParam) return true;
    // No resolved entry (a library TU with no main) — nothing to wrap.
    if (!userEntrySymbol.has_value()) return true;

    // Locate the entry function + its signature in the merged module.
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
    // — leave it; the trampoline's own resolution reports that case.
    if (!entryFn.valid() || !entrySig.valid()) return true;

    // A no-arg entry (`int main(void)`) needs no argument setup — the trampoline
    // calls it directly and it ignores the argument registers. Synthesizing would
    // run __wgetmainargs pointlessly, so skip.
    auto const params = interner.fnParams(entrySig);
    if (params.size() < 2) return true;

    TypeId const argcTy = params[0];   // int
    TypeId const argvTy = params[1];   // char** / wchar_t** (ptr<ptr<elem>>)

    // argc must be int (i32) — the synthesized argc stack slot is sized 4 bytes and
    // the arg-forward reads it as i32. `main`/`wmain`'s argc is always int; fail loud
    // rather than silently under/over-read a non-standard width.
    if (interner.kind(argcTy) != TypeKind::I32) {
        emitErr(reporter, "synthesizePeStartup: entry's 1st parameter (argc) is not "
                          "int (i32) — the CRT arg-fetch synthesis requires it");
        return false;
    }

    // WIDE vs NARROW is derived from the entry's argv ELEMENT width — never a
    // format flag. wchar_t** on pe is ptr<ptr<u16>>; char** is ptr<ptr<i8/char>>.
    if (interner.kind(argvTy) != TypeKind::Ptr) {
        emitErr(reporter, "synthesizePeStartup: entry's 2nd parameter (argv) is "
                          "not a pointer — cannot form the CRT arg-fetch call");
        return false;
    }
    TypeId const argvPointee = interner.operands(argvTy)[0];   // ptr<elem>
    if (interner.kind(argvPointee) != TypeKind::Ptr) {
        emitErr(reporter, "synthesizePeStartup: entry's argv is not a pointer-to-"
                          "pointer (char**/wchar_t**)");
        return false;
    }
    TypeKind const elem = interner.kind(interner.operands(argvPointee)[0]);
    std::string crtName;
    if (elem == TypeKind::U16) {
        crtName = processArgs.crtWideArgvFn;     // __wgetmainargs
    } else if (elem == TypeKind::Char || elem == TypeKind::I8) {
        crtName = processArgs.crtNarrowArgvFn;   // __getmainargs
    } else {
        emitErr(reporter, "synthesizePeStartup: entry's argv element is neither "
                          "char nor the pe wide-char (u16) — cannot pick the CRT "
                          "arg-fetch export");
        return false;
    }

    // Types for the synth body.
    TypeId const i32Ty    = interner.primitive(TypeKind::I32);
    TypeId const pArgcTy  = interner.pointer(argcTy);   // int*
    TypeId const pArgvTy  = interner.pointer(argvTy);   // char*** / wchar_t***
    TypeId const pI32Ty   = interner.pointer(i32Ty);    // _startupinfo* (as int*)
    // __wgetmainargs(int*, argv***, argv***, int, _startupinfo*) -> int (Win64 cc).
    std::array<TypeId, 5> const crtParams{pArgcTy, pArgvTy, pArgvTy, i32Ty, pI32Ty};
    TypeId const crtSig    = interner.fnSig(crtParams, i32Ty, CallConv::CcMS64);
    TypeId const synthSig  = interner.fnSig({}, i32Ty, CallConv::CcMS64);
    TypeId const pCrtSig   = interner.pointer(crtSig);
    TypeId const pEntrySig = interner.pointer(entrySig);

    // Mint fresh symbols: the synth function + the CRT extern.
    std::uint32_t const maxV = maxSymbolIdV(mir, externImports);
    SymbolId const synthSym{maxV + 1};
    SymbolId const crtSym{maxV + 2};

    // Register the CRT arg-fetch export (a FUNCTION import, not data).
    ExternImport crtExtern;
    crtExtern.symbol      = crtSym;
    crtExtern.mangledName = crtName;
    crtExtern.libraryPath = processArgs.crtLibraryPath;
    crtExtern.isData      = false;   // a FUNCTION import, not a data object
    externImports.push_back(std::move(crtExtern));

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
    // this synthesis, pre-optimization) keeps it — it is also the retargeted entry.
    (void)builder.addFunction(synthSig, synthSym, SymbolBinding::Global,
                              SymbolVisibility::Default);
    // The synth function's sole block IS its entry block — mark it so directly.
    // The merge path would re-derive this marker in the optimizer, but the single-CU
    // path synthesizes POST-optimize (no rederive pass follows), so the raw marker
    // must already be canonical (else MirVerifier's checkEntryBlocks / StructCfMarker
    // equality rejects the module).
    MirBlockId const entryBlk = builder.createBlock(StructCfMarker::EntryBlock);
    builder.beginBlock(entryBlk);

    // Stack locals (Alloca yields a pointer to the storage; payload = byte size).
    MirInstId const argcSlot = builder.addInst(MirOpcode::Alloca, {}, pArgcTy, 4);
    MirInstId const argvSlot = builder.addInst(MirOpcode::Alloca, {}, pArgvTy, 8);
    MirInstId const envSlot  = builder.addInst(MirOpcode::Alloca, {}, pArgvTy, 8);
    MirInstId const siSlot   = builder.addInst(MirOpcode::Alloca, {}, pI32Ty, 4);

    // startupinfo.newmode = 0 (msvcrt dereferences it; must be zeroed non-NULL).
    MirLiteralValue zeroLit;
    zeroLit.value = std::int64_t{0};
    zeroLit.core  = TypeKind::I32;
    MirInstId const zero = builder.addConst(std::move(zeroLit), i32Ty);
    {
        std::array<MirInstId, 2> st{zero, siSlot};   // Store {value, ptr}
        builder.addInst(MirOpcode::Store, st, InvalidType);
    }
    MirLiteralValue wildLit;
    wildLit.value = std::int64_t{0};
    wildLit.core  = TypeKind::I32;
    MirInstId const doWild = builder.addConst(std::move(wildLit), i32Ty);

    // __wgetmainargs(&argc, &argv, &env, 0, &startupinfo).
    MirInstId const crtAddr = builder.addGlobalAddr(crtSym, pCrtSig);
    {
        std::array<MirInstId, 6> call{crtAddr, argcSlot, argvSlot, envSlot,
                                      doWild, siSlot};
        (void)builder.addInst(MirOpcode::Call, call, i32Ty, /*payload=*/0);
    }

    // argc = *argcSlot; argv = *argvSlot.
    MirInstId const argc = builder.addInst(MirOpcode::Load,
                                           std::array<MirInstId, 1>{argcSlot},
                                           argcTy);
    MirInstId const argv = builder.addInst(MirOpcode::Load,
                                           std::array<MirInstId, 1>{argvSlot},
                                           argvTy);

    // return entry(argc, argv).
    MirInstId const entryAddr = builder.addGlobalAddr(*userEntrySymbol, pEntrySig);
    MirInstId const ret = builder.addInst(MirOpcode::Call,
                                          std::array<MirInstId, 3>{entryAddr, argc, argv},
                                          i32Ty, /*payload=*/0);
    (void)builder.addReturn(ret);

    opt::passes::cloneGlobalsVerbatim(mir, builder);
    mir = std::move(builder).finish();

    // Retarget the program entry to the synth function.
    userEntrySymbol = synthSym;
    return true;
}

} // namespace dss
