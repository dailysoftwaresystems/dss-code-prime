#include "mir/merge/synth_pe_startup.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/entry_param_types.hpp"   // typeOfEntryParamShape -- the ONE shape <-> type correspondence
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"       // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // the pointer width of the module's data model (envp's slot)
#include "ffi/mangling/c_mangle.hpp"   // applyCMangling (per-format CRT import names)
#include "mir/merge/synth_symbol_floor.hpp"  // continueSymbolIdsPastImports (the rebuild continues the module's ids)
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
#include <vector>

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

// ⓘ THE INIT'S SYMBOL AND ITS IMPORTS' COME FROM THE MODULE (`MirBuilder::mintSymbolOrAbort`,
// the one door — see mir/merge/synth_symbol_floor.hpp), as the SEH-funclet and
// threads-shim passes' do. The module's end is past every id it holds — its GLOBALS
// matter: synthetic string-literal globals are minted ABOVE every function/extern id, so
// an id that cleared only those would let the init duplicate a real global's, and the
// linker would silently mis-bind the entry onto that DATA symbol, an entry that "runs" a
// string literal — and past every id the NAME TABLE the module was made from holds, which
// a scan of the module cannot see: the init is a GLOBAL definition the lower half
// publishes under whatever name the table gives its id — ✔MEASURED P69 round 4, an x86_64
// Linux image of `int main(int argc, char **argv, char **envp)` carried this init as
// `T __func__`.

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

// The type a parameter of an entry verb's slot has is `typeOfEntryParamShape`
// (core/types/entry_param_types.hpp): the inverse of the classifier the semantic
// tier matches a definition with, so the two can never learn different shapes.


// The one body of the three doors below. `userEntrySymbol` set: the entry is a
// function of `mir`, called with its own signature, or -- `callWithTheVerbs-
// Signature` -- with the verb's. Unset with `foreignEntryName` named: the entry is
// a function ANOTHER input of the link defines under that name, and the init calls
// it through an import row of the module, by that name, with the verb's signature.
[[nodiscard]] bool materializeEntryShape(Mir&                              mir,
                                         TypeInterner&                     interner,
                                         std::optional<SymbolId>&          userEntrySymbol,
                                         std::string_view                  foreignEntryName,
                                         bool                              callWithTheVerbsSignature,
                                         std::vector<ExternImport>&        externImports,
                                         EntryMaterialization              verb,
                                         std::optional<ProcessArgs> const& processArgs,
                                         CSymbolDecorationScheme           scheme,
                                         std::string_view                  formatName,
                                         DiagnosticReporter&               reporter) {
    bool const foreign = !userEntrySymbol.has_value() && !foreignEntryName.empty();
    // No resolved entry (a library TU with no `main`) — nothing to gate and
    // nothing to materialize.
    if (!userEntrySymbol.has_value() && !foreign) return true;

    // ★ THE VERB'S OWN SIGNATURE: the language row's parameter shapes and `int`,
    // the row's result. Its convention is the one the semantic tier gives every
    // function it types, the canonical MIR-tier placeholder the calling-convention
    // pass replaces with the target's at materialize time (`functionTypeOfParams`),
    // so the init and the call it makes are given the target's C convention exactly
    // as a source entry's are.
    auto const verbsSignature = [&] {
        std::vector<TypeId> params;
        for (EntryParamShape const shape : entryVerbParams(verb)) {
            params.push_back(typeOfEntryParamShape(interner, shape));
        }
        return interner.fnSig(params, interner.primitive(TypeKind::I32), CallConv::CcSysV);
    };

    // Locate the entry function + its signature in the module — or, for an
    // entry another input defines, its signature from the verb (below).
    MirFuncId entryFn{};
    TypeId    entrySig = InvalidType;
    std::size_t const nf = mir.moduleFuncCount();
    if (!foreign) {
        for (std::uint32_t i = 0; i < nf; ++i) {
            MirFuncId const f = mir.funcAt(i);
            if (mir.funcSymbol(f).v == userEntrySymbol->v) {
                entryFn  = f;
                entrySig = mir.funcSignature(f);
                break;
            }
        }
        // The entry symbol isn't a defined function here (e.g. resolved to an
        // extern) — leave it; the trampoline's own resolution reports that case
        // with `K_EntryPointResolvesToExtern`, which is the accurate diagnostic.
        if (!entryFn.valid() || !entrySig.valid()) return true;
        // ★ A DEFINITION WHOSE SIGNATURE IS NONE OF ITS NAME'S ROWS (P69 fold 3e,
        // class C's N3: the semantic tier's `entryCalledWithTheVerbsSignature`) is
        // called as every reference's startup calls any `main` -- with the verb's
        // signature, its own parameters receiving those arguments. ✔MEASURED
        // before: gcc 13.3 and cl 19.44 link `int main(int, double)` and run it.
        if (callWithTheVerbsSignature) entrySig = verbsSignature();
    } else {
        // ★ AN ENTRY FROM AN OBJECT HAS NO SIGNATURE IN THE INPUT — an assembled
        // function carries no type — so the init calls it with the signature of
        // the verb it was elected with (P69 fold 3d,
        // D-PROGRAM-AN-OBJECTS-ENTRY-IS-CALLED-WITHOUT-ITS-ARGUMENTS; the verb is
        // the richest the format realizes, `collectObjectEntryCandidates`).
        entrySig = verbsSignature();
    }

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
        // Mach-O: dyld CALLS an LC_MAIN entry with (argc, argv, envp, apple)
        // already in the argument registers, so "no mechanism" is a real ANSWER
        // and the trampoline's pass-through is correct for EVERY verb the format
        // lists — the environment verbs included. DOCUMENTED, and pinned in-tree
        // by `ProcessArgsSubstrate.ShippedMachoExecsDeclareNoneAndPe…`.
        return true;
    }
    bool const passesEnvironment = entryVerbPassesEnvironment(verb);
    switch (processArgs->mechanism) {
    case ArgsMechanism::StackVector:        // the entry-stack arm below
    case ArgsMechanism::CrtArgvAccessors:   // the CRT init below
        break;
    case ArgsMechanism::None:
        // The loader rejects `mechanism: "none"`, and `optional` empty is how
        // "no mechanism" is spelled, so a `None` reaching here is a hand-built
        // schema. Refuse rather than silently skip the setup a matched verb asked
        // for — a skipped setup calls the entry on uninitialized registers.
        // Anchored: D-RUNTIME-MAIN-ARGC-ARGV.
        emitErr(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                std::format(
                    "realizeEntryShape: format '{}' declares a `processArgs` "
                    "block whose mechanism is the invalid `none` sentinel while "
                    "the resolved program entry needs the '{}' materialization "
                    "verb — argument setup would be silently skipped, calling "
                    "the entry on UNINITIALIZED registers. Declare the format's "
                    "argument mechanism.",
                    formatName, entryMaterializationName(verb)));
        return false;
    }

    ProcessArgs const& pa = *processArgs;
    bool const stackVector = pa.mechanism == ArgsMechanism::StackVector;

    // WHAT the verb materializes comes from the ONE verb → slots table
    // (`entryVerbSlots`): each slot's parameter shape and the VALUE it holds --
    // wide vs narrow, an environment, the GUI startups' four -- read per slot,
    // never off a position or a count. That is still the RESOLVED ENTRY'S
    // SIGNATURE deciding, exactly as c111 required — the language row that matched
    // the entry carries the verb — and never a format flag or a second
    // classification here. argc is SHARED between the narrow and wide worlds
    // (MEASURED, PROBE-0), so one accessor name serves both.
    auto const verbParams = entryVerbParams(verb);
    auto const verbSlots  = entryVerbSlots(verb);
    bool const wide = entryVerbIsWide(verb);

    // ── THE MECHANISM BACKSTOP ─────────────────────────────────────────────
    //
    // `ObjectFormatData::validate()` refuses every one of these pairings AT LOAD
    // for a format document, so a loaded format cannot reach them. A hand-built
    // `ProcessArgs` can, and the alternative to refusing here is materializing
    // values the mechanism never produced — the entry would read them from the
    // argument registers. Both tiers ask the ONE statement of what a mechanism
    // produces, per value (`processArgsCannotProduce`, target_schema.cpp).
    for (EntryVerbSlot const& slot : verbSlots) {
        std::string const why = processArgsCannotProduce(pa, slot.value);
        if (why.empty()) continue;
        emitErr(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                std::format(
                    "realizeEntryShape: format '{}' declares the `{}` "
                    "`processArgs` mechanism, which cannot realize the resolved "
                    "program entry's '{}' verb -- its {}: {}. Refusing rather "
                    "than handing the entry values nothing produced.",
                    formatName, argsMechanismName(pa.mechanism),
                    entryMaterializationName(verb),
                    entryArgumentValueName(slot.value), why));
        return false;
    }

    // The entry trampoline materializes (argc, argv) from the untouched
    // process-entry stack (SysV AMD64 psABI §3.4.1 / AAPCS64 Linux). An environment
    // verb needs the vector AFTER argv, whose place depends on the RUNTIME argc —
    // that is the one value synthesized below; every other verb needs nothing at the
    // MIR tier. (Checked AFTER the backstop above, so a hand-built entry-stack
    // mechanism handed a wide verb is refused rather than passed through.)
    if (stackVector && !passesEnvironment) return true;

    // ── THE ARITY BACKSTOP, AND IT IS NOT THE RETIRED GATE ────────────────
    //
    // Every verb materializes into the entry's FIRST N parameters, N being its
    // row in the verb table, so an entry reaching here with fewer is an internal
    // inconsistency between the verb entry resolution decided and the signature
    // actually in the merged MIR. On the normal path that CANNOT happen: the
    // semantic tier already matched this definition against the language row the
    // verb came from. So this fires only for a signature that never passed through
    // the semantic tier — a hand-built `Mir`, or an entry arriving from an object
    // DSS did not compile.
    //
    // ★ WHY IT IS NOT A SECOND SIGNATURE OWNER, which is the thing to check
    // before touching it: it does not ask "is this a legal entry signature" (the
    // language owns that, with a span). It asks "can the arguments I am about to
    // materialize physically land in this function", which is a fact about the
    // MIR in hand and about nothing declared anywhere. Without it, `params[k]`
    // below is an out-of-bounds read — a silent one, on the program entry.
    //
    // ⓘ IT HAS NOTHING TO CHECK FOR AN ENTRY ANOTHER INPUT DEFINES. An assembled
    // function carries no TypeId and no signature, so a pre-built object's entry
    // has no C signature IN THE INPUT; its signature above is BUILT from the verb,
    // and agrees with it by construction. Whether the object's `main` declares
    // fewer parameters is the case every reference startup also passes through
    // (it passes every value to every `main`). The same holds for a definition
    // whose signature is none of its name's rows (`callWithTheVerbsSignature`,
    // P69 fold 3e): it is called with the verb's signature, built the same way.
    auto const params = interner.fnParams(entrySig);
    if (params.size() < verbParams.size()) {
        // Anchored: D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE.
        emitErr(reporter, DiagnosticCode::K_EntryVerbUnmaterializable,
                std::format(
                    "realizeEntryShape: the resolved program entry needs the "
                    "'{}' materialization verb, which materializes {} arguments "
                    "into the entry's first {} parameters, but the entry's "
                    "signature in the merged module declares {} parameter(s). "
                    "The verb and the signature disagree, so the arguments have "
                    "nowhere to land. On any source DSS compiled this is "
                    "impossible — the semantic tier matched the definition "
                    "against the very language row this verb came from — so this "
                    "signature did not pass through it (a hand-built module, or "
                    "an entry from a pre-built object): give the entry the "
                    "signature its language row declares.",
                    entryMaterializationName(verb), verbParams.size(),
                    verbParams.size(), params.size()));
        return false;
    }

    // Each slot's parameter type, copied out of the interner's pool BEFORE anything
    // below interns a type (a view into the pool does not survive its growth): the
    // entry's own parameter there, or the verb's for an entry called with the
    // verb's signature.
    std::vector<TypeId> slotTypes;
    slotTypes.reserve(verbParams.size());
    for (std::size_t i = 0; i < verbParams.size(); ++i) slotTypes.push_back(params[i]);
    // The type of the slot holding `value`; InvalidType where the verb hands none.
    auto const slotType = [&](EntryArgumentValue value) -> TypeId {
        for (std::size_t i = 0; i < verbSlots.size(); ++i) {
            if (verbSlots[i].value == value) return slotTypes[i];
        }
        return InvalidType;
    };

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

    TypeId const i32Ty     = interner.primitive(TypeKind::I32);
    TypeId const i64Ty     = interner.primitive(TypeKind::I64);
    TypeId const pEntrySig = interner.pointer(entrySig);

    // Rebuild the module (Mir is frozen): clone every existing function verbatim,
    // then APPEND the synth function, then clone globals — the prune_unreachable
    // rebuild idiom. The rebuilt module continues the source's symbol ids, and the
    // new symbols are minted from it.
    constexpr char const* kMinter = "realizeEntryShape";
    MirBuilder builder;
    continueSymbolIdsPastImports(builder, mir, externImports);
    SymbolId const synthSym = builder.mintSymbolOrAbort(kMinter);
    // What the init calls: the module's entry, or — for an entry another input
    // defines — an import row of the module naming it, which the link's merge
    // binds to that input's definition as it binds any reference between units.
    SymbolId calleeSym{};
    if (foreign) {
        calleeSym = builder.mintSymbolOrAbort(kMinter);
        ExternImport entryImport;
        entryImport.symbol      = calleeSym;
        entryImport.mangledName = std::string{foreignEntryName};   // already the format's spelling
        entryImport.isData      = false;
        externImports.push_back(std::move(entryImport));
    } else {
        calleeSym = *userEntrySymbol;
    }
    IdentityClonePolicy policy;
    for (std::uint32_t i = 0; i < nf; ++i) {
        opt::passes::MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(mir.funcAt(i));
    }

    auto konst = [&](std::int64_t v, TypeKind core, TypeId ty) {
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = core;
        return builder.addConst(std::move(lit), ty);
    };

    if (stackVector) {
        // ── THE ENTRY-STACK ENVIRONMENT (mechanism `stack-vector`) ─────────────
        //
        //   int _dss_stack_start(int argc, char **argv) {   // argc/argv from the
        //       char **envp = argv + (argc + 1);            // trampoline, as before
        //       return main(argc, argv, envp);
        //   }
        //
        // The environment vector starts one slot past argv's NULL terminator in the
        // SAME entry-stack layout (SysV AMD64 psABI §3.4.1; AAPCS64 Linux), so its
        // address depends on the RUNTIME argc — which is why no static offset in
        // `processArgs` can name it, and why it is ordinary pointer arithmetic here
        // rather than per-architecture trampoline code. ✔MEASURED 2026-09-24, gcc
        // 13.3.0 and clang 18.1.3 on x86_64 Linux: `envp - argv == argc + 1` under
        // glibc's own startup. Each slot is a POINTER, so its width is the module's
        // data model's (`MirModuleFacts::dataModel`, the one home of the pointer width;
        // P69 fold 3f, F3 P16 -- it was a document key, `vectorSlotBytes`, a second
        // statement of it). The synth takes exactly the two values the trampoline
        // materializes, so the trampoline and its before-entry park are unchanged.
        std::optional<std::uint64_t> const slotBytes =
            scalarByteSize(TypeKind::Ptr, builder.moduleFacts().dataModel);
        if (!slotBytes.has_value()) {
            emitErr(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                    std::format("realizeEntryShape: the module states no data model with a pointer "
                                "size ({}), so the environment vector's slot past argv has no width "
                                "-- refusing rather than placing envp by a guessed one",
                                dataModelSpelled(builder.moduleFacts().dataModel)));
            return false;
        }
        TypeId const argcTy = slotType(EntryArgumentValue::ArgumentCount);         // int
        TypeId const argvTy = slotType(EntryArgumentValue::NarrowArgumentVector);  // char**
        TypeId const envpTy = slotType(EntryArgumentValue::NarrowEnvironment);     // char**
        TypeId const synthSig = interner.fnSig(
            std::array<TypeId, 2>{argcTy, argvTy}, i32Ty, cc);
        (void)builder.addFunction(synthSig, synthSym, SymbolBinding::Global,
                                  SymbolVisibility::Default);
        MirBlockId const entryBlk = builder.createBlock(StructCfMarker::EntryBlock);
        builder.beginBlock(entryBlk);
        MirInstId const argc = builder.addArg(0, argcTy);
        MirInstId const argv = builder.addArg(1, argvTy);
        MirInstId const argc64 = builder.addInst(
            MirOpcode::SExt, std::array<MirInstId, 1>{argc}, i64Ty);
        MirInstId const slots = builder.addInst(
            MirOpcode::Add,
            std::array<MirInstId, 2>{argc64, konst(1, TypeKind::I64, i64Ty)},
            i64Ty);
        MirInstId const bytes = builder.addInst(
            MirOpcode::Mul,
            std::array<MirInstId, 2>{
                slots,
                konst(static_cast<std::int64_t>(*slotBytes),
                      TypeKind::I64, i64Ty)},
            i64Ty);
        // A BYTE-offset Gep — the MIR Gep index contract (the lowering emits
        // `lea [base + index*1]`), which is why the slot width is multiplied in.
        MirInstId const envp = builder.addInst(
            MirOpcode::Gep, std::array<MirInstId, 2>{argv, bytes}, envpTy);
        MirInstId const entryAddr =
            builder.addGlobalAddr(calleeSym, pEntrySig);
        MirInstId const ret = builder.addInst(
            MirOpcode::Call,
            std::array<MirInstId, 4>{entryAddr, argc, argv, envp},
            i32Ty, /*payload=*/0);
        builder.addReturn(ret);
    } else {
        // ── THE CRT INIT (mechanism `crt-argv-accessors`) ──────────────────────
        //
        // ★ EVERY VERB CONFIGURES THE ARGUMENT VECTOR AND PASSES ITS GATE, THE GUI
        // ONES TOO (P69 fold 3f, lane `xa`). ✔MEASURED 2026-11-17 (probe-reference-cc runs 20261117-082204-1f9ca84b, 20261117-082259-45497930):
        // inside WinMain and wWinMain `__argc` is 1 under cl 19.52 + link.exe and
        // MinGW-w64 gcc 13.2 alike, and MSVC's GUI startup imports the configure
        // call and the accessors -- so a GUI program that reads `__argc` / `__argv`
        // reads the configured vector under DSS as under the references. Then each
        // SLOT of the verb is fetched by the VALUE it holds:
        //   argument count / vector  -> one dereference of its accessor
        //   environment              -> the width's accessor; its result IS the vector
        //   command line             -> the width's `{narrow,wide}WinMainCommandLineFn`
        //   module instance, show    -> the `winMainStartupRoutines` (the format's
        //                               C runtime's static startup half)
        //   previous instance        -> a null pointer (the Win32 contract)
        bool const handsCount   = entryVerbHands(verb, EntryArgumentValue::ArgumentCount);
        bool const handsCommand = entryVerbHands(verb, EntryArgumentValue::NarrowCommandLine)
                               || entryVerbHands(verb, EntryArgumentValue::WideCommandLine);
        bool const handsInstance = entryVerbHands(verb, EntryArgumentValue::ModuleInstance);
        bool const handsShow     = entryVerbHands(verb, EntryArgumentValue::ShowWindow);
        // The argument vector the gate reads: the entry's own where the verb hands
        // one, else the width's vector shape (a GUI verb reads it for the gate only).
        TypeId const argvTy = entryVerbPassesArgumentVector(verb)
            ? slotType(wide ? EntryArgumentValue::WideArgumentVector
                            : EntryArgumentValue::NarrowArgumentVector)
            : typeOfEntryParamShape(interner, wide ? EntryParamShape::PtrPtrU16
                                                   : EntryParamShape::PtrPtrChar);
        TypeId const argcTy  = handsCount ? slotType(EntryArgumentValue::ArgumentCount)
                                          : InvalidType;
        // Interned in the order they always were, so a verb that hands argc and
        // argv mints the same types in the same order as before the slots.
        TypeId const boolTy  = interner.primitive(TypeKind::Bool);
        TypeId const pArgcTy = handsCount ? interner.pointer(argcTy)   // int*    (__p___argc)
                                          : InvalidType;
        TypeId const pArgvTy = interner.pointer(argvTy);   // char*** (__p___argv)
        // errno_t _configure_{narrow,wide}_argv(int mode)
        std::array<TypeId, 1> const cfgParams{i32Ty};
        TypeId const cfgSig      = interner.fnSig(cfgParams, i32Ty, cc);
        // int* __p___argc(void) / char*** __p___argv(void)
        TypeId const argcAccSig  = handsCount ? interner.fnSig({}, pArgcTy, cc)
                                              : InvalidType;
        TypeId const argvAccSig  = interner.fnSig({}, pArgvTy, cc);
        TypeId const synthSig    = interner.fnSig({}, i32Ty, cc);
        TypeId const pCfgSig     = interner.pointer(cfgSig);
        TypeId const pArgcAccSig = handsCount ? interner.pointer(argcAccSig)
                                              : InvalidType;
        TypeId const pArgvAccSig = interner.pointer(argvAccSig);

        std::string const configureName =
            wide ? pa.configureWideArgvFn : pa.configureNarrowArgvFn;
        std::string const argvAccessorName =
            wide ? pa.wideArgvAccessorFn : pa.narrowArgvAccessorFn;

        // Mint the CRT import symbols after the synth function's — each only when
        // the verb reads what it fetches.
        SymbolId const cfgSym     = builder.mintSymbolOrAbort(kMinter);
        SymbolId const argcAccSym =
            handsCount ? builder.mintSymbolOrAbort(kMinter) : SymbolId{};
        SymbolId const argvAccSym = builder.mintSymbolOrAbort(kMinter);
        SymbolId const envInitSym =
            passesEnvironment ? builder.mintSymbolOrAbort(kMinter) : SymbolId{};
        SymbolId const envAccSym =
            passesEnvironment ? builder.mintSymbolOrAbort(kMinter) : SymbolId{};
        SymbolId const cmdSym =
            handsCommand ? builder.mintSymbolOrAbort(kMinter) : SymbolId{};
        SymbolId const instSym =
            handsInstance ? builder.mintSymbolOrAbort(kMinter) : SymbolId{};
        SymbolId const showSym =
            handsShow ? builder.mintSymbolOrAbort(kMinter) : SymbolId{};

        // Register the CRT imports (all FUNCTION imports, not data). Their library
        // is the ROLE-resolved image — see `RuntimeLibraryRole`; nothing here spells
        // a DLL name.
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
        auto addImport = [&](SymbolId sym, std::string const& name,
                             std::string const& libraryPath) {
            ExternImport imp;
            imp.symbol      = sym;
            imp.mangledName = dss::ffi::applyCMangling(name, scheme);
            imp.libraryPath = libraryPath;
            imp.isData      = false;
            externImports.push_back(std::move(imp));
        };
        addImport(cfgSym, configureName, pa.crtLibraryPath);
        if (handsCount) addImport(argcAccSym, pa.argcAccessorFn, pa.crtLibraryPath);
        addImport(argvAccSym, argvAccessorName, pa.crtLibraryPath);
        if (passesEnvironment) {
            addImport(envInitSym, wide ? pa.initializeWideEnvironmentFn
                                       : pa.initializeNarrowEnvironmentFn,
                      pa.crtLibraryPath);
            addImport(envAccSym,  wide ? pa.wideEnvironmentAccessorFn
                                       : pa.narrowEnvironmentAccessorFn,
                      pa.crtLibraryPath);
        }
        if (handsCommand) {
            addImport(cmdSym, wide ? pa.wideWinMainCommandLineFn
                                   : pa.narrowWinMainCommandLineFn,
                      pa.crtLibraryPath);
        }
        // The backstop above refused a GUI verb on a block with no routines.
        if (handsInstance) {
            addImport(instSym, pa.winMainStartupRoutines->moduleInstanceFn,
                      pa.winMainStartupRoutines->libraryPath);
        }
        if (handsShow) {
            addImport(showSym, pa.winMainStartupRoutines->showWindowFn,
                      pa.winMainStartupRoutines->libraryPath);
        }

        // _dss_pe_start(): the pre-main init. Global-bound so DCE (which runs AFTER
        // this synthesis on the single-CU seam) keeps it — it is also the retargeted
        // program entry.
        (void)builder.addFunction(synthSig, synthSym, SymbolBinding::Global,
                                  SymbolVisibility::Default);
        MirBlockId const entryBlk = builder.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const failBlk  = builder.createBlock();
        MirBlockId const callBlk  = builder.createBlock();

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
        // _initialize_{narrow,wide}_environment() — MSVC's OWN startup order and its
        // own treatment of the result (DOCUMENTED, the MSVC toolset's
        // `crt/src/vcruntime/exe_common.inl`: `pre_c_initialization` calls
        // `environment_policy::initialize_environment()` AFTER `configure_argv` and
        // IGNORES what it returns, where it fast-fails on the argv call's). ✔MEASURED
        // 2026-09-24 against ucrtbase 10.0.26100.9444 with no CRT startup — DSS's
        // situation: the DLL has already built both vectors at load, and the call
        // returns 0 leaving the same pointer. So it is not what makes envp valid on
        // the dynamic UCRT; it is what the reference does, and matching it keeps the
        // environment's state the reference's rather than a DLL-attach side effect's.
        if (passesEnvironment) {
            TypeId const initSig  = interner.fnSig({}, i32Ty, cc);
            MirInstId const initAddr =
                builder.addGlobalAddr(envInitSym, interner.pointer(initSig));
            (void)builder.addInst(MirOpcode::Call,
                                  std::array<MirInstId, 1>{initAddr}, i32Ty,
                                  /*payload=*/0);
        }
        // argc = *__p___argc();  argv = *__p___argv();
        // EXACTLY ONE dereference each — the accessors return the ADDRESS of the
        // CRT's state (`ucrt/stdlib.h`'s `__p___argc` / `__p___argv`: `int*` and
        // `char***`), which is why a second load would read the first element
        // instead of the vector. argc only where the verb hands it.
        MirInstId argc{};
        MirInstId argv{};
        if (handsCount) {
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
        // anything: the value leaves through the format's already-wired
        // `processExit` path (the trampoline moves this return value into the exit
        // mechanism's status register), so this needs no second exit import — and
        // it is not an `Unreachable`, which in a REACHABLE position would invite an
        // optimizer to treat the guarded branch as dead and delete the check.
        builder.beginBlock(failBlk);
        builder.addReturn(konst(
            static_cast<std::int64_t>(pa.argvUnavailableExitStatus),
            TypeKind::I32, i32Ty));

        // return entry(<each slot's value, in the verb's order>);
        //
        // envp is `_get_initial_{narrow,wide}_environment()` — the accessor's result
        // IS the vector (no dereference), passed exactly as MSVC's own `invoke_main`
        // passes it (`main(__argc, __argv, _get_initial_narrow_environment())`,
        // DOCUMENTED in the same file), unchecked there too. The GUI values are
        // fetched as MSVC's GUI `invoke_main` fetches them (`WinMain(&__ImageBase,
        // nullptr, _get_narrow_winmain_command_line(), __scrt_get_show_window_mode())`,
        // DOCUMENTED, exe_common.inl / exe_winmain.cpp), each through the routine the
        // format names for it.
        builder.beginBlock(callBlk);
        {
            MirInstId const entryAddr =
                builder.addGlobalAddr(calleeSym, pEntrySig);
            // A zero-argument routine whose result is the slot's value.
            auto const fetch = [&](SymbolId sym, TypeId resultTy) {
                TypeId const sig = interner.fnSig({}, resultTy, cc);
                MirInstId const addr =
                    builder.addGlobalAddr(sym, interner.pointer(sig));
                return builder.addInst(MirOpcode::Call,
                                       std::array<MirInstId, 1>{addr}, resultTy,
                                       /*payload=*/0);
            };
            std::vector<MirInstId> callOps{entryAddr};
            for (std::size_t i = 0; i < verbSlots.size(); ++i) {
                TypeId const ty = slotTypes[i];
                switch (verbSlots[i].value) {
                case EntryArgumentValue::ArgumentCount:
                    callOps.push_back(argc);
                    break;
                case EntryArgumentValue::NarrowArgumentVector:
                case EntryArgumentValue::WideArgumentVector:
                    callOps.push_back(argv);
                    break;
                case EntryArgumentValue::NarrowEnvironment:
                case EntryArgumentValue::WideEnvironment:
                    callOps.push_back(fetch(envAccSym, ty));
                    break;
                case EntryArgumentValue::NarrowCommandLine:
                case EntryArgumentValue::WideCommandLine:
                    callOps.push_back(fetch(cmdSym, ty));
                    break;
                case EntryArgumentValue::ModuleInstance:
                    callOps.push_back(fetch(instSym, ty));
                    break;
                case EntryArgumentValue::ShowWindow:
                    callOps.push_back(fetch(showSym, ty));
                    break;
                case EntryArgumentValue::PreviousInstance:
                    callOps.push_back(konst(0, TypeKind::Ptr, ty));
                    break;
                case EntryArgumentValue::AppleVector:
                    // The backstop refused it: no `processArgs` mechanism
                    // produces it. Reaching here is a backstop that stopped
                    // asking, so refuse rather than pass the entry a value
                    // nothing produced.
                    emitErr(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
                            std::format(
                                "realizeEntryShape: format '{}': the '{}' verb's "
                                "{} reached the CRT init, which produces none",
                                formatName, entryMaterializationName(verb),
                                entryArgumentValueName(verbSlots[i].value)));
                    return false;
                }
            }
            MirInstId const ret = builder.addInst(
                MirOpcode::Call, std::span<MirInstId const>{callOps}, i32Ty,
                /*payload=*/0);
            builder.addReturn(ret);
        }
    }

    opt::passes::cloneGlobalsVerbatim(mir, builder);
    mir = std::move(builder).finish();

    // Canonicalize StructCfMarkers module-wide from the CFG. REQUIRED because the
    // CRT synth body is MULTI-BLOCK (the argv gate) and the single-CU seam runs
    // POST-optimize, so no later pass re-derives them — and `MirVerifier`'s
    // marker-equality check rejects a module whose raw markers are not already
    // canonical. Same call, same reason, as `synthesizeThreadsShim`'s
    // multi-block bodies.
    rederiveStructCfMarkers(mir);

    // Retarget the program entry to the synth function.
    userEntrySymbol = synthSym;
    return true;
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
    return materializeEntryShape(mir, interner, userEntrySymbol, {}, /*callWithTheVerbsSignature=*/false,
                                 externImports, verb, processArgs, scheme, formatName, reporter);
}

bool realizeEntryShapeWithTheVerbsSignature(Mir&                              mir,
                                            TypeInterner&                     interner,
                                            std::optional<SymbolId>&          userEntrySymbol,
                                            std::vector<ExternImport>&        externImports,
                                            EntryMaterialization              verb,
                                            std::optional<ProcessArgs> const& processArgs,
                                            CSymbolDecorationScheme           scheme,
                                            std::string_view                  formatName,
                                            DiagnosticReporter&               reporter) {
    return materializeEntryShape(mir, interner, userEntrySymbol, {}, /*callWithTheVerbsSignature=*/true,
                                 externImports, verb, processArgs, scheme, formatName, reporter);
}

bool realizeForeignEntryShape(Mir&                              mir,
                              TypeInterner&                     interner,
                              std::string_view                  entryLinkName,
                              std::optional<SymbolId>&          init,
                              std::vector<ExternImport>&        externImports,
                              EntryMaterialization              verb,
                              std::optional<ProcessArgs> const& processArgs,
                              CSymbolDecorationScheme           scheme,
                              std::string_view                  formatName,
                              DiagnosticReporter&               reporter) {
    init.reset();
    if (entryLinkName.empty()) return true;   // no entry: nothing to materialize
    return materializeEntryShape(mir, interner, init, entryLinkName, /*callWithTheVerbsSignature=*/true,
                                 externImports, verb, processArgs, scheme, formatName, reporter);
}

} // namespace dss
