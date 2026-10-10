// THE MODULE'S SYMBOL-ID SPACE AND ITS ONE DOOR (P69 rounds 4 and 5, lane lm) —
// `Mir::symbolIdEnd`, `MirBuilder::mintSymbol`, mir/merge/synth_symbol_floor.hpp.
//
// ═══ WHAT IS PINNED ════════════════════════════════════════════════════════
//
// A symbol synthesized for a module after the front end numbered it — HIR→MIR's
// literal objects, the optimizer's rodata zero, the entry-shape init and its CRT
// imports, the SEH funclets and their personality, the <threads.h> helper imports —
// needs an id nothing else in the module's id space has. Most of that space is in the
// NAME TABLE the module was made from (the semantic model's records on the single-CU
// route, the merge's allocation on the whole-program one), not in the module: a local,
// a parameter, a typedef, a shipped constant, `__func__`, a block-scope extern. Whoever
// lowers the module names each of its ids through that table.
//
// Round 4 handed the three synthesis passes the table's end as an argument (✔MEASURED
// then: the stack-vector init of an x86_64 Linux image was published as `T __func__`).
// The optimizer's minter was a FIFTH site and was handed nothing: ✔MEASURED 2026-10-08,
// its zero constant took id 154 in a unit whose table ends at 159; where that id was a
// block-scope extern's, a single-unit release relocatable build was refused ("declared
// more than once") and a two-unit release image linked silently and died on its first
// call. Round 5: the MODULE carries the end and every minter asks the module. So:
//   * the door itself — what states, continues, raises and mints; that a symbol the
//     builder is handed raises the end live and the constructor counts a module made
//     without a builder; that a writer-reserved value is stepped over and an exhausted
//     space answers the invalid id; that a mint from a builder told nothing dies, and so
//     does a minter that cannot refuse at exhaustion; and the continuation a tier
//     lowering the frozen module mints from;
//   * a STATED end against a COUNTED one (P69, lane cs): the module carries whether its
//     end was ever stated — by the table it was made from, by its maker's word that it
//     is made from nothing but itself, or by the module it replaces — a rebuild copies
//     that fact and never makes it, a move carries it, and both leaves die rather than
//     mint past an end that was only counted, through a real pass too;
//   * a unit lowered with NO table stated still ends past its imports;
//   * each synthesis pass mints past an end the module's own symbols do not reach;
//   * the single-CU pipeline, at BOTH shipped configurations: an artifact publishes no
//     name its source does not declare, no synthesized object shares an id with an
//     import, and the module still carries its table's end after every pass ran;
//   * two scans of the source tree: no file under src/mir or src/opt counts a SymbolId
//     of its own, and every file that rebuilds a module continues or states its ids.
//
// (The MERGED route's statement — `mergeCuMirs` stating its allocator's end — is pinned
// beside the merge's other tests: `MirMerge.TheMergedModuleCarriesTheEndOfTheMergesAllocation`.)
//
// RED-ON-DISABLE (each alone): `lowerToMir` stating no table end (HIR→MIR's own minter
// and every later one then count from the unit's symbols); HIR→MIR not carrying the
// unit's symbols into the end; the one list of module facts a rebuild copies
// (`carryModuleFactsOf`) dropping the id space; `keepSymbolIdsClearOf` raising nothing;
// the constructor not counting; the reserved value not stepped over; and four minters
// each counting its own id again — mem2reg's zero, the entry-shape init, a SEH funclet,
// a threads helper import.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/entry_shape.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"   // LibrarySynthesis, SehPersonality
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"        // ProcessArgs
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "ffi/mangling/c_mangle.hpp"       // applyCMangling (a declared name as the format spells it)
#include "hir/attributes/ffi_metadata.hpp" // FfiMetadata (a hand-built unit's import)
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"               // HirFfiMap
#include "hir/hir_node.hpp"
#include "link/object_format_schema.hpp"
#include "mir/lowering/hir_to_mir.hpp"     // lowerToMir, MirLoweringConfig (a unit lowered with no table)
#include "mir/merge/synth_pe_startup.hpp"
#include "mir/merge/synth_seh_funclets.hpp"
#include "mir/merge/synth_symbol_floor.hpp"
#include "mir/merge/synth_threads_shim.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"            // detail::MirFunc / MirGlobal (a module made from raw arenas)
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"   // cloneGlobalsVerbatim (a rebuild carries the end)
#include "program/compile_pipeline.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace dss;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += '['; out += d.actual; out += ']'; }
    return out;
}

std::unordered_set<std::uint32_t> externIds(std::vector<ExternImport> const& ext) {
    std::unordered_set<std::uint32_t> ids;
    for (auto const& e : ext) ids.insert(e.symbol.v);
    return ids;
}

std::unordered_set<std::uint32_t> funcIds(Mir const& mir) {
    std::unordered_set<std::uint32_t> ids;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
        ids.insert(mir.funcSymbol(mir.funcAt(i)).v);
    return ids;
}

// What a fixture's module is made from: `kNoTable` builds it as a hand-built module is
// built — from nothing but itself, which it SAYS (its end is one past the symbols it
// defines; the door mints for no module that never said where its ids end); any other
// value is the end of the name table HIR→MIR would state for it.
constexpr std::uint32_t kNoTable = 0;
void stateTable(MirBuilder& mb, std::uint32_t tableEnd) {
    if (tableEnd != kNoTable) mb.stateSymbolIdEnd(tableEnd);
    else                      mb.stateSelfContainedSymbolIds();
}

// A one-function module: the entry `sig` bound to SymbolId{100}, body `return 0;`.
Mir buildEntryOnly(TypeInterner& in, TypeId sig, std::uint32_t tableEnd) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
    stateTable(mb, tableEnd);
    mb.addFunction(sig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

// The two shipped argument mechanisms, as the pe and elf exec formats declare them.
ProcessArgs ucrtAccessorPa() {
    ProcessArgs pa;
    pa.mechanism             = ArgsMechanism::CrtArgvAccessors;
    pa.configureNarrowArgvFn = "_configure_narrow_argv";
    pa.configureWideArgvFn   = "_configure_wide_argv";
    pa.argcAccessorFn        = "__p___argc";
    pa.narrowArgvAccessorFn  = "__p___argv";
    pa.wideArgvAccessorFn    = "__p___wargv";
    pa.argvMode              = 1;
    pa.argvUnavailableExitStatus = 127;
    pa.initializeNarrowEnvironmentFn = "_initialize_narrow_environment";
    pa.narrowEnvironmentAccessorFn   = "_get_initial_narrow_environment";
    pa.initializeWideEnvironmentFn   = "_initialize_wide_environment";
    pa.wideEnvironmentAccessorFn     = "_get_initial_wide_environment";
    pa.role                  = RuntimeLibraryRole::CLibrary;
    pa.crtLibraryPath        = "ucrtbase.dll";
    return pa;
}

ProcessArgs stackVectorPa() {
    ProcessArgs pa;
    pa.mechanism                 = ArgsMechanism::StackVector;
    pa.argcStackOffset           = 0;
    pa.argvStackOffset           = 8;
    pa.envpFollowsArgvTerminator = true;
    pa.vectorSlotBytes           = 8;
    return pa;
}

SehPersonality peSehPersonality() {
    SehPersonality p;
    p.role        = RuntimeLibraryRole::UnwindPersonality;
    p.libraryPath = "ucrtbase.dll";
    p.mangledName = "__C_specific_handler";
    return p;
}

// A single-`__try` parent (the c115 hir_to_mir CFG), bound to `sym`.
Mir buildSehParent(TypeInterner& in, SymbolId sym, std::uint32_t tableEnd) {
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const u32  = in.primitive(TypeKind::U32);
    TypeId const pI32 = in.pointer(i32);
    TypeId const sig  = in.fnSig({}, i32, CallConv::CcMS64);
    MirBuilder mb;
    stateTable(mb, tableEnd);
    mb.addFunction(sig, sym);
    MirBlockId const entry     = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tryBB     = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filterBB  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handlerBB = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const joinBB    = mb.createBlock(StructCfMarker::Linear);
    std::uint32_t const region = 0;

    mb.beginBlock(entry);
    mb.addSehTryBegin(tryBB, filterBB, region);

    mb.beginBlock(tryBB);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    (void)mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, region);
    mb.addBr(joinBB);

    mb.beginBlock(filterBB);
    MirInstId const code = mb.addInst(MirOpcode::SehExceptionCode, {}, u32);
    MirLiteralValue av; av.value = std::int64_t{0xC0000005}; av.core = TypeKind::U32;
    MirInstId const avc = mb.addConst(std::move(av), u32);
    MirInstId const cmp = mb.addInst(MirOpcode::ICmpEq,
                                     std::array<MirInstId, 2>{code, avc}, i32);
    mb.addSehFilterReturn(cmp, handlerBB, region);

    mb.beginBlock(handlerBB);
    mb.addBr(joinBB);

    mb.beginBlock(joinBB);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

// A module whose main (SymbolId{100}) calls mtx_lock — pre-minted SymbolId{10}, the
// recipe the semantic phase names — through a GlobalAddr, as CST→HIR lowers it.
Mir buildCallsMtxLock(TypeInterner& in, std::uint32_t tableEnd) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 1> const lockParams{pV};
    TypeId const lockSig = in.fnSig(lockParams, i32, CallConv::CcMS64);
    MirBuilder mb;
    stateTable(mb, tableEnd);
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot     = mb.addInst(MirOpcode::Alloca, {}, pV, 40);
    MirInstId const lockAddr = mb.addGlobalAddr(SymbolId{10}, in.pointer(lockSig));
    MirInstId const callOps[] = {lockAddr, slot};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    return std::move(mb).finish();
}

// The shipped C grammar + a shipped target + a shipped format, and the format's
// calling-convention index — the inputs every pipeline entry point takes.
struct Toolchain {
    std::shared_ptr<GrammarSchema>      grammar;
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
    std::uint16_t                       ccIndex = 0;
};

std::optional<Toolchain> loadToolchain(char const* targetName, char const* formatName) {
    auto grammarR = GrammarSchema::loadShipped("c");
    auto targetR  = TargetSchema::loadShipped(targetName);
    auto formatR  = ObjectFormatSchema::loadShipped(formatName);
    EXPECT_TRUE(grammarR.has_value()) << "c.lang.json must load";
    EXPECT_TRUE(targetR.has_value()) << targetName << " must load";
    EXPECT_TRUE(formatR.has_value()) << formatName << " must load";
    if (!grammarR || !targetR || !formatR) return std::nullopt;
    DiagnosticReporter rep;
    auto const abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
    EXPECT_TRUE(abi.has_value() && abi->cc != nullptr) << allDiagText(rep);
    if (!abi.has_value() || abi->cc == nullptr) return std::nullopt;
    auto const ccSpan = (*targetR)->callingConventions();
    Toolchain tc{*grammarR, *targetR, *formatR, 0};
    tc.ccIndex = static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
    return tc;
}

CompilationUnit unitOf(Toolchain const& tc, std::string source, std::string label) {
    UnitBuilder builder{tc.grammar, DiagnosticBudget::libraryDefault()};
    dss::applySystemDirs(builder, *tc.grammar);
    builder.addInMemory(std::move(source), std::move(label));
    return std::move(builder).finish();
}

CompileOptions optionsFor(CompileConfig config) {
    CompileOptions opts{DiagnosticBudget::libraryDefault()};
    opts.config = config;
    return opts;
}

char const* configLabel(CompileConfig c) { return c == CompileConfig::Release ? "release" : "debug"; }

// The reviewer's failing input: in release the optimizer makes a zero constant for the
// path that leaves `x` unassigned. No float or string literal — a literal's synthetic
// global is minted past the semantic table and would lift a module-only count clear of
// it, hiding the case.
constexpr char const* kPick =
    "double pick(int c, double v) { double x; if (c) x = v; return x; }\n";

// The same function where the count lands on an IMPORT: the module's highest-numbered
// symbol is the static local `tick`, and the very next record is the block-scope extern
// the body calls, whose import row carries that id.
constexpr char const* kPickCallsABlockScopeExtern =
    "double pick(int, double);\n"
    "int main(int argc, char **argv) { (void)argv; return (int)pick(argc, argc + 40); }\n"
    "double pick(int c, double v) {\n"
    "    static int tick;\n"
    "    extern double ext_in(double);\n"
    "    double x;\n"
    "    if (c) x = v;\n"
    "    ++tick;\n"
    "    return ext_in(x);\n"
    "}\n";

std::string readWhole(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Every `SymbolId{…}` / `SymbolId const name{…}` in `text` whose braces hold a `+`:
// the spelling of an id counted on the spot (`{maxV + 1}`, `{++next}`, `{next++}`).
std::vector<std::string> countedSymbolIds(std::string const& text) {
    std::vector<std::string> found;
    std::string_view const needle = "SymbolId";
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_'))
            continue;
        std::size_t i = at + needle.size();
        auto skipSpace = [&] { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
        auto skipWord  = [&] {
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
        };
        skipSpace();
        if (text.compare(i, 5, "const") == 0) { i += 5; skipSpace(); }
        skipWord();          // the declared name, when there is one
        skipSpace();
        if (i >= text.size() || text[i] != '{') continue;
        std::size_t const close = text.find_first_of("{}", i + 1);
        if (close == std::string::npos || text[close] != '}') continue;
        std::string const inside = text.substr(i + 1, close - i - 1);
        if (inside.find('+') != std::string::npos)
            found.push_back(text.substr(at, close - at + 1));
    }
    return found;
}

} // namespace

// ── The door ──────────────────────────────────────────────────────────────────

TEST(SynthSymbolFloor, TheModuleCarriesTheEndOfItsSymbolIdSpace) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    auto addFn = [&](MirBuilder& mb, std::uint32_t sym) {
        mb.addFunction(in.fnSig({}, i32, CallConv::CcSysV), SymbolId{sym});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32));
    };
    {
        MirBuilder mb;
        addFn(mb, 3);
        addFn(mb, 5);
        Mir const mir = std::move(mb).finish();
        EXPECT_EQ(mir.symbolIdEnd(), 6u)
            << "CONTROL: a module made from no table ends one past the symbols it defines";
    }
    {
        MirBuilder mb;
        mb.stateSymbolIdEnd(4);
        addFn(mb, 3);
        addFn(mb, 5);
        EXPECT_EQ(std::move(mb).finish().symbolIdEnd(), 6u)
            << "a table ending below the module's symbols changes nothing: a statement only raises";
    }
    {
        // A symbol the builder is HANDED raises the end past itself — an object and a
        // function alike — so a mint never returns an id the module already defines,
        // whatever table end was stated.
        MirBuilder handed;
        handed.stateSymbolIdEnd(4);
        (void)handed.addGlobal(i32, SymbolId{9}, UINT32_MAX, MirFuncId{}, SymbolBinding::Local,
                               SymbolVisibility::Default, /*isConst=*/false,
                               MirThreadStorage::Shared);
        EXPECT_EQ(handed.symbolIdEnd(), 10u) << "an object the builder is handed";
        addFn(handed, 12);
        EXPECT_EQ(handed.symbolIdEnd(), 13u) << "a function the builder is handed";
        EXPECT_EQ(handed.mintSymbol().v, 13u);
    }
    {
        // A value a format WRITER defines for itself is never a module's: the door
        // steps over it (the PE `_tls_index` singleton is the one such value today).
        ASSERT_TRUE(isWriterReservedSymbolIdValue(kTlsIndexReservedSymbolIdValue));
        MirBuilder reserved;
        reserved.stateSymbolIdEnd(kTlsIndexReservedSymbolIdValue - 1u);
        EXPECT_EQ(reserved.mintSymbol().v, kTlsIndexReservedSymbolIdValue - 1u)
            << "CONTROL: the value before the reserved one is an ordinary id";
        EXPECT_EQ(reserved.mintSymbol().v, kTlsIndexReservedSymbolIdValue + 1u)
            << "the reserved value itself is never handed out";
    }
    {
        // Exhaustion: the last value is never minted (it has no "one past"), the
        // mint is the invalid id — and stays so — and an id AT the last value
        // saturates the end instead of wrapping it onto 0.
        constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();
        MirBuilder full;
        full.stateSymbolIdEnd(kLast - 1u);
        EXPECT_EQ(full.mintSymbol().v, kLast - 1u) << "CONTROL: the last id that can be minted";
        EXPECT_FALSE(full.mintSymbol().valid());
        EXPECT_FALSE(full.mintSymbol().valid());
        MirBuilder saturated;
        saturated.stateSymbolIdEnd(7);
        saturated.keepSymbolIdsClearOf(SymbolId{kLast});
        EXPECT_EQ(saturated.symbolIdEnd(), kLast);
        EXPECT_FALSE(saturated.mintSymbol().valid());
    }
    MirBuilder mb;
    addFn(mb, 3);
    addFn(mb, 5);
    mb.stateSymbolIdEnd(20);
    EXPECT_EQ(mb.symbolIdEnd(), 20u) << "a table holding ids up to 19 takes them all";
    mb.keepSymbolIdsClearOf(SymbolId{29});
    EXPECT_EQ(mb.symbolIdEnd(), 30u) << "an id something beside the module holds raises the end past itself";
    mb.keepSymbolIdsClearOf(SymbolId{7});
    EXPECT_EQ(mb.symbolIdEnd(), 30u) << "an id already inside the space changes nothing";
    EXPECT_EQ(mb.mintSymbol().v, 30u);
    EXPECT_EQ(mb.mintSymbolOrAbort("test").v, 31u);
    Mir const mir = std::move(mb).finish();
    EXPECT_EQ(mir.symbolIdEnd(), 32u) << "the finished module carries the end past every id minted for it";

    // A rebuild continues the source's ids — and so does every clone-globals helper,
    // which is how a pass that never mints still hands the end on.
    MirBuilder direct;
    direct.continueSymbolIdsOf(mir);
    EXPECT_EQ(direct.mintSymbol().v, 32u);
    MirBuilder viaHelper;
    opt::passes::cloneGlobalsVerbatim(mir, viaHelper);
    EXPECT_EQ(std::move(viaHelper).finish().symbolIdEnd(), 32u)
        << "a rebuild that defines NOTHING still carries the source's end";
    // The one list of module facts a rebuild copies (`carryModuleFactsOf`) holds the
    // id space beside the two aliasing facts.
    {
        MirBuilder strict;
        strict.setAliasingMode(MirAliasingMode::StrictTBAA);
        strict.setCharTypesAliasAll(false);
        strict.stateSymbolIdEnd(77);
        Mir const source = std::move(strict).finish();
        MirBuilder rebuilt;
        rebuilt.carryModuleFactsOf(source);
        Mir const copy = std::move(rebuilt).finish();
        EXPECT_EQ(copy.aliasingMode(), MirAliasingMode::StrictTBAA);
        EXPECT_FALSE(copy.charTypesAliasAll());
        EXPECT_EQ(copy.symbolIdEnd(), 77u);
    }

    // A tier that lowers the FROZEN module and needs symbols of its own continues
    // the module's ids too, without a builder (MIR→LIR's block symbols).
    MirSymbolIdContinuation past{mir};
    EXPECT_EQ(past.mint().v, 32u) << "the first id past a frozen module is its end";
    EXPECT_EQ(past.mintOrAbort("test").v, 33u);
    EXPECT_EQ(past.end(), 34u);
    past.keepClearOf(SymbolId{12});
    EXPECT_EQ(past.end(), 34u) << "an id already inside the space changes nothing";
    past.keepClearOf(SymbolId{40});
    EXPECT_EQ(past.mint().v, 41u)
        << "an id something beside the module holds (an import row handed to the lowering) "
           "raises the end past itself";
    EXPECT_EQ(mir.symbolIdEnd(), 32u) << "a continuation never writes the module";
}

// A module assembled from raw arenas — no builder in the loop (a verifier fixture; the
// direct constructor is public) — ends past every symbol its arenas define all the same:
// the constructor counts them, so whatever continues such a module's ids is clear of
// them. (`MirBuilder::finish` adds what only a builder knows — the table's end and the
// ids minted — on top of that count.)
TEST(SynthSymbolFloor, AModuleMadeWithoutABuilderEndsPastTheSymbolsItDefines) {
    substrate::ArenaBuilder<detail::MirInst,   MirInstId,   MirModuleId> insts{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirBlock,  MirBlockId,  MirModuleId> blocks{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirFunc,   MirFuncId,   MirModuleId> funcs{MirModuleId{1}};
    substrate::ArenaBuilder<detail::MirGlobal, MirGlobalId, MirModuleId> globals{MirModuleId{1}};
    detail::MirFunc fn;
    fn.symbol = 41;
    (void)funcs.addNode(fn);
    detail::MirGlobal object;
    object.symbol = 57;
    (void)globals.addNode(object);
    std::vector<MirBlockId> instBlock{InvalidMirBlock};   // slot 0: as long as the inst arena
    Mir const mir(std::move(insts).finish(), std::move(blocks).finish(),
                  std::move(funcs).finish(), std::move(globals).finish(),
                  std::move(instBlock), {}, {}, {}, MirLiteralPool{}, MirAsmDescriptorPool{},
                  MirAliasingMode::Permissive, /*charTypesAliasAll=*/true);
    EXPECT_EQ(mir.symbolIdEnd(), 58u) << "one past the highest symbol either arena defines";
    EXPECT_EQ(MirSymbolIdContinuation{mir}.end(), 58u) << "and a continuation starts there";
    // …but that end is COUNTED — nobody stated it — so nothing is minted past it until
    // a rebuild that knows what the module was made from says so
    // (`SymbolIdEndIsStatedDeath.AMintPastAnEndThatWasOnlyCountedDies`).
    EXPECT_FALSE(mir.symbolIdEndIsStated());
    MirBuilder rebuilt;
    rebuilt.continueSymbolIdsOf(mir);
    rebuilt.stateSelfContainedSymbolIds();
    EXPECT_EQ(rebuilt.mintSymbol().v, 58u) << "the first id past it, once its maker's word is given";
}

// A caller that states NO table — a fixture that lowers hand-built HIR with the default
// `MirLoweringConfig` — still gets a module that ends past every symbol the UNIT holds,
// its imports included: an import's row lives beside the module, so the module never
// defines it and only HIR→MIR can carry its id into the end (once, after the collect
// pre-passes and before anything is minted). Here the import is the unit's ONLY symbol,
// so nothing else can raise the end: without the carry the first id minted for this
// module is the import's own — the round-5 defect, in a fixture. (With a table stated the
// carry changes nothing, which is why no pipeline arm can see it.)
TEST(SynthSymbolFloor, AUnitLoweredWithNoTableEndsPastItsImports) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32  = ti.primitive(TypeKind::I32);
    TypeId const fnTy = ti.fnSig(std::array{i32}, i32, CallConv::CcSysV);
    constexpr std::uint32_t kImportSymV = 17;
    HirBuilder b{"c"};
    HirNodeId const importNode = b.makeExternFunction(fnTy, /*symbol=*/kImportSymV, {});
    HirNodeId const root = b.makeModule(std::array{importNode});
    Hir hir = std::move(b).finish(root);
    HirFfiMap ffi{hir};
    FfiMetadata meta;
    meta.mangledName   = "printf";
    meta.importLibrary = "libc.so.6";
    ffi.set(importNode, meta);

    DiagnosticReporter rep;
    HirLiteralPool pool;
    auto result = lowerToMir(hir, pool, ti, rep, /*sourceMap=*/nullptr, MirLoweringConfig{}, &ffi);
    ASSERT_TRUE(result.ok) << allDiagText(rep);
    ASSERT_EQ(result.mir.moduleFuncCount(), 0u)
        << "CONTROL: the module defines nothing — the import is the unit's only symbol";
    ASSERT_EQ(result.mir.moduleGlobalCount(), 0u);
    EXPECT_EQ(result.mir.symbolIdEnd(), kImportSymV + 1u)
        << "the module's end must be one past the import's id";
    MirBuilder rebuilt;
    rebuilt.continueSymbolIdsOf(result.mir);
    EXPECT_GT(rebuilt.mintSymbol().v, kImportSymV)
        << "the first id minted for the module must not be the import's";
}

// An id counted from the symbols a builder happens to hold is the defect itself, so the
// door leaves no way to mint one: a builder told neither a table's end nor whose ids it
// continues refuses.
TEST(SynthSymbolFloorDeath, AMintFromABuilderToldNothingDies) {
    EXPECT_DEATH({
        MirBuilder mb;
        (void)mb.mintSymbol();
    }, "symbol-id space was never stated");
}

// A pass over a frozen module has no source position to put a diagnostic on, so at
// exhaustion it dies by name rather than hand a wrapped id downstream — from a builder
// and from a frozen module's continuation alike.
TEST(SynthSymbolFloorDeath, AnExhaustedSpaceKillsTheMinterThatCannotRefuse) {
    EXPECT_DEATH({
        MirBuilder mb;
        mb.stateSymbolIdEnd(std::numeric_limits<std::uint32_t>::max());
        (void)mb.mintSymbolOrAbort("thePassThatMints");
    }, "thePassThatMints fatal: the module's SymbolId space is exhausted");
    EXPECT_DEATH({
        MirBuilder mb;
        mb.stateSymbolIdEnd(std::numeric_limits<std::uint32_t>::max());
        Mir const full = std::move(mb).finish();
        MirSymbolIdContinuation past{full};
        (void)past.mintOrAbort("theLoweringThatMints");
    }, "theLoweringThatMints fatal: the module's SymbolId space is exhausted");
}

// ── A STATED END AGAINST A COUNTED ONE ────────────────────────────────────────
//
// Whether a module's end was ever STATED travels with the module — through `finish`,
// a move, every rebuild, the frozen module's continuation and the `.dssir` reader —
// and both leaves refuse to mint past an end that was only counted. Those pins are
// mir/test_symbol_id_end_is_stated, a binary of their own for the reason its header
// gives: the refusal is an abort, so a case that observes the FACT must never mint
// before it has asserted it, and this file's cases mint freely.

// ── Each pass mints past the module's end ────────────────────────────────────

// The round-4 review's finding: the helper imports. The table here ends at 500, far
// above every id the module holds (100) and every recipe (10) — as the semantic table
// of a TU including <threads.h> ends above its shipped constants.
TEST(SynthSymbolFloor, ThreadsHelperImportsMintPastTheNameTable) {
    LibrarySynthesis const win32{LibrarySynthVehicle::Win32,
                                 RuntimeLibraryRole::SystemPrimitives, "kernel32.dll"};
    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "mtx_lock"}};
    for (std::uint32_t const tableEnd : {kNoTable, 500u}) {
        SCOPED_TRACE(tableEnd);
        TypeInterner in{CompilationUnitId{1}};
        Mir mir = buildCallsMtxLock(in, tableEnd);
        std::vector<ExternImport> ext;
        DiagnosticReporter rep;
        ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, win32,
                                          CSymbolDecorationScheme::None, ext, rep))
            << allDiagText(rep);
        ASSERT_FALSE(ext.empty()) << "mtx_lock's body imports its kernel32 primitive";
        for (auto const& e : ext) {
            if (tableEnd == kNoTable) {
                EXPECT_GT(e.symbol.v, 100u)
                    << "CONTROL: " << e.mangledName << " clears the module's ids";
            } else {
                EXPECT_GE(e.symbol.v, tableEnd)
                    << e.mangledName << " was minted INSIDE the module's name table (id "
                    << e.symbol.v << " < " << tableEnd << "): the table names it as "
                       "something else";
            }
        }
        EXPECT_GT(mir.symbolIdEnd(), tableEnd)
            << "the rebuilt module carries the end past the helpers it was given";
    }
}

// What travels BESIDE a hand-built module — an import row, a recipe's reserved id — is
// tied to it by nothing, and a pass keeps clear of it all the same.
TEST(SynthSymbolFloor, AHandBuiltModulesImportRowsAndRecipeIdsAreKeptClearOf) {
    LibrarySynthesis const win32{LibrarySynthVehicle::Win32,
                                 RuntimeLibraryRole::SystemPrimitives, "kernel32.dll"};
    // The recipe map reserves 10 (referenced) and 400 (not referenced by the module).
    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "mtx_lock"},
                                                                 {400u, "mtx_unlock"}};
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCallsMtxLock(in, kNoTable);
    ASSERT_LT(mir.symbolIdEnd(), 300u) << "the fixture's module must not reach the row's id";
    std::vector<ExternImport> ext(1);
    ext[0].symbol      = SymbolId{300};
    ext[0].mangledName = "an_import_the_unit_already_had";
    ext[0].libraryPath = "some.dll";
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, win32,
                                      CSymbolDecorationScheme::None, ext, rep))
        << allDiagText(rep);
    ASSERT_GT(ext.size(), 1u);
    for (std::size_t i = 1; i < ext.size(); ++i)
        EXPECT_GT(ext[i].symbol.v, 400u)
            << ext[i].mangledName << " took id " << ext[i].symbol.v
            << ", at or below an id the import rows (300) or the recipe map (400) hold";
}

// The entry shape's init — a GLOBAL definition — and the CRT imports it calls.
TEST(SynthSymbolFloor, EntryInitAndItsImportsMintPastTheNameTable) {
    struct Arm {
        char const*          label;
        EntryMaterialization verb;
        CallConv             cc;
        ProcessArgs          pa;
        char const*          formatName;
    };
    std::array<Arm, 2> const arms{{
        {"crt-accessor (pe)", EntryMaterialization::ArgcArgv, CallConv::CcMS64,
         ucrtAccessorPa(), "pe64-x86_64-windows-exec"},
        {"stack-vector (elf)", EntryMaterialization::ArgcArgvEnvp, CallConv::CcSysV,
         stackVectorPa(), "elf64-x86_64-linux-exec"},
    }};
    for (std::uint32_t const tableEnd : {kNoTable, 700u}) {
        for (auto const& arm : arms) {
            SCOPED_TRACE(std::string{arm.label} + " table end " + std::to_string(tableEnd));
            TypeInterner in{CompilationUnitId{1}};
            TypeId const i32 = in.primitive(TypeKind::I32);
            TypeId const ppc = in.pointer(in.pointer(in.primitive(TypeKind::Char)));
            std::vector<TypeId> params{i32, ppc};
            if (arm.verb == EntryMaterialization::ArgcArgvEnvp) params.push_back(ppc);
            Mir mir = buildEntryOnly(in, in.fnSig(params, i32, arm.cc), tableEnd);
            std::optional<SymbolId>   entry = SymbolId{100};
            std::vector<ExternImport> ext;
            DiagnosticReporter        rep;
            ASSERT_TRUE(realizeEntryShape(mir, in, entry, ext, arm.verb, arm.pa,
                                          CSymbolDecorationScheme::None, arm.formatName, rep))
                << allDiagText(rep);
            ASSERT_TRUE(entry.has_value());
            ASSERT_NE(entry->v, 100u) << "the entry is retargeted to the synthesized init";
            if (tableEnd == kNoTable) {
                EXPECT_EQ(entry->v, 101u) << "CONTROL: with no table the init takes the module's next id";
                continue;
            }
            EXPECT_GE(entry->v, tableEnd)
                << "the init (a GLOBAL definition) was minted inside the module's name table";
            std::unordered_set<std::uint32_t> seen{entry->v};
            for (auto const& e : ext) {
                EXPECT_GE(e.symbol.v, tableEnd) << e.mangledName << " minted inside the table";
                EXPECT_TRUE(seen.insert(e.symbol.v).second)
                    << e.mangledName << " shares id " << e.symbol.v << " with another synthesized symbol";
            }
        }
    }
}

TEST(SynthSymbolFloor, SehFuncletAndPersonalityMintPastTheNameTable) {
    for (std::uint32_t const tableEnd : {kNoTable, 900u}) {
        SCOPED_TRACE(tableEnd);
        TypeInterner in{CompilationUnitId{1}};
        Mir mir = buildSehParent(in, SymbolId{100}, tableEnd);
        std::unordered_set<std::uint32_t> const before = funcIds(mir);
        std::vector<ExternImport> ext;
        std::vector<MirSehScope>  scopes;
        DiagnosticReporter        rep;
        ASSERT_TRUE(synthesizeSehFunclets(mir, in, ext, peSehPersonality(),
                                          CSymbolDecorationScheme::None,
                                          "pe64-x86_64-windows-exec", scopes, rep))
            << allDiagText(rep);
        ASSERT_EQ(scopes.size(), 1u);
        std::uint32_t const floor = tableEnd == kNoTable ? 101u : tableEnd;
        std::size_t funclets = 0;
        for (std::uint32_t const v : funcIds(mir)) {
            if (before.contains(v)) continue;
            ++funclets;
            EXPECT_GE(v, floor) << "a funclet was minted inside the module's id space";
        }
        EXPECT_EQ(funclets, 1u);
        ASSERT_EQ(ext.size(), 1u);
        EXPECT_GE(ext[0].symbol.v, floor) << "the personality import minted inside the module's id space";
    }
}

// ── The single-CU pipeline ────────────────────────────────────────────────────

// What the review's arms reach in a real build: the lower half names every defined
// symbol through the semantic model, so a synthesized definition minted inside the
// model's id space is PUBLISHED under the name the model gives that id. ✔MEASURED
// before the round-4 fix (the round-3 dsscp): the first arm's x86_64 Linux image
// carried `T __func__` — its stack-vector init, named after main's `__func__`.
// ✔MEASURED before the round-5 fix (2026-10-08): the RELEASE arms — the optimizer's
// zero constant took the id after the module's last symbol, which the table gives to a
// parameter, or to the import the body calls (the build of that arm was REFUSED: the
// import "is declared more than once").
// No string literal in any source: a literal's synthetic global is minted past the
// semantic table and would lift a module-only count clear of it, hiding the case.
TEST(SynthSymbolFloor, ASingleUnitPublishesNoNameItsSourceDoesNotDeclare) {
    struct Arm {
        char const*           target;
        char const*           format;
        CompileConfig         config;
        char const*           source;
        std::set<std::string> declared;
    };
    std::vector<Arm> const arms{
        {"x86_64", "elf64-x86_64-linux-exec", CompileConfig::Debug,
         "int main(int argc, char **argv, char **envp) { return envp[0] ? argc - 1 : 2; }\n",
         {"main"}},
        {"x86_64", "pe64-x86_64-windows-exec", CompileConfig::Debug,
         "int main(int argc, char **argv) { return argv[0] ? argc - 1 : 2; }\n",
         {"main"}},
        {"x86_64", "pe64-x86_64-windows-exec", CompileConfig::Debug,
         "int guarded(volatile int *p) {\n"
         "    int r = 0;\n"
         "    __try { r = *p; } __except (1) { r = -1; }\n"
         "    return r;\n"
         "}\n"
         "int main(void) { volatile int v = 41; return guarded(&v) + 1; }\n",
         {"main", "guarded"}},
        // The release schedule: the optimizer's zero constant.
        {"x86_64", "elf64-x86_64-linux", CompileConfig::Release, kPick, {"pick"}},
        {"x86_64", "pe64-x86_64-windows", CompileConfig::Release, kPick, {"pick"}},
        {"arm64", "macho64-arm64-darwin", CompileConfig::Release, kPick, {"pick"}},
        {"x86_64", "elf64-x86_64-linux", CompileConfig::Release, kPickCallsABlockScopeExtern,
         {"main", "pick", "tick"}},
        {"x86_64", "pe64-x86_64-windows", CompileConfig::Release, kPickCallsABlockScopeExtern,
         {"main", "pick", "tick"}},
        {"arm64", "macho64-arm64-darwin", CompileConfig::Release, kPickCallsABlockScopeExtern,
         {"main", "pick", "tick"}},
    };
    for (auto const& arm : arms) {
        SCOPED_TRACE(std::string{arm.format} + " (" + configLabel(arm.config) + "): " + arm.source);
        auto const tc = loadToolchain(arm.target, arm.format);
        ASSERT_TRUE(tc.has_value());
        CompilationUnit cu = unitOf(*tc, arm.source, "floor.c");
        DiagnosticReporter rep;
        auto const mod = assembleUnit(cu, *tc->grammar, *tc->target, *tc->format, tc->ccIndex,
                                      rep, optionsFor(arm.config));
        ASSERT_TRUE(mod.has_value()) << allDiagText(rep);
        ASSERT_EQ(rep.errorCount(), 0u) << allDiagText(rep);
        // The declared names as THIS format spells them on the binary (Mach-O puts
        // `_` before a C name): the format's own decoration rule, never a guess.
        std::set<std::string> published;
        for (std::string const& name : arm.declared)
            published.insert(
                dss::ffi::applyCMangling(name, tc->format->cSymbolDecoration().scheme));
        for (auto const& s : mod->symbols) {
            EXPECT_TRUE(published.contains(s.name))
                << "the module publishes '" << s.name << "' (symbol " << s.symbol.v
                << "), which the source does not declare: a synthesized symbol took an id "
                   "the semantic table names";
        }
    }
}

// The same two release inputs, read at the tier the id is minted in: after every pass
// of the unit schedule ran, the module still carries its table's end, nothing the
// optimizer synthesized sits inside the table, and no object shares an id with an
// import. The debug arm is the control: it mints no zero, and carries the end too.
TEST(SynthSymbolFloor, TheOptimizersConstantIsMintedPastTheSemanticTable) {
    struct Arm {
        char const*           source;
        std::set<std::string> staticObjects;   // the module globals the source declares
    };
    std::array<Arm, 2> const arms{{
        {kPick, {}},
        {kPickCallsABlockScopeExtern, {"tick"}},
    }};
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        for (auto const& arm : arms) {
            SCOPED_TRACE(std::string{configLabel(config)} + ": " + arm.source);
            auto const tc = loadToolchain("x86_64", "elf64-x86_64-linux");
            ASSERT_TRUE(tc.has_value());
            CompilationUnit cu = unitOf(*tc, arm.source, "zero.c");
            DiagnosticReporter rep;
            auto cuMir = buildCuMir(cu, *tc->grammar, *tc->target, *tc->format, tc->ccIndex, rep,
                                    optionsFor(config));
            ASSERT_TRUE(cuMir.has_value()) << allDiagText(rep);
            ASSERT_EQ(rep.errorCount(), 0u) << allDiagText(rep);
            auto const tableEnd = static_cast<std::uint32_t>(cuMir->model.symbols().size());
            Mir const& mir = cuMir->mir;
            EXPECT_GE(mir.symbolIdEnd(), tableEnd)
                << "a pass of the unit schedule rebuilt the module without its table's end";
            std::unordered_set<std::uint32_t> const imports = externIds(cuMir->externImports);
            for (std::uint32_t const v : imports)
                EXPECT_LT(v, mir.symbolIdEnd()) << "an import's id is outside the module's id space";
            std::size_t synthesized = 0;
            for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
                SymbolId const sym = mir.globalSymbol(mir.globalAt(i));
                EXPECT_FALSE(imports.contains(sym.v))
                    << "module global " << sym.v << " shares its id with an import";
                if (sym.v >= tableEnd) { ++synthesized; continue; }
                SymbolRecord const* const rec = cuMir->model.recordFor(sym);
                ASSERT_NE(rec, nullptr);
                EXPECT_TRUE(arm.staticObjects.contains(std::string{rec->name}))
                    << "module global " << sym.v << " sits inside the semantic table (end "
                    << tableEnd << "), which names that id '" << rec->name
                    << "' — not an object the source gives static storage";
            }
            if (config == CompileConfig::Release) {
                EXPECT_GE(synthesized, 1u)
                    << "the input must reach the minter: release makes a zero constant for the "
                       "path that leaves x unassigned";
            } else {
                EXPECT_EQ(synthesized, 0u) << "CONTROL: the debug schedule synthesizes no object here";
            }
        }
    }
}

// The round-4 review's arm itself, through the real front end: the helpers `mtx_init`
// and `mtx_destroy` call are minted past the CU's semantic table, which holds
// <threads.h>'s shipped constants (`thrd_success` …) above every id the module holds.
TEST(SynthSymbolFloor, ASingleUnitsThreadsHelpersMintAboveItsSemanticTable) {
    struct Arm { char const* target; char const* format; };
    std::array<Arm, 2> const arms{{
        {"x86_64", "pe64-x86_64-windows-exec"},
        {"arm64", "macho64-arm64-darwin-exec"},
    }};
    std::string const source =
        "#include <threads.h>\n"
        "int main(void) {\n"
        "    mtx_t m;\n"
        "    if (mtx_init(&m, mtx_plain) != thrd_success) return 1;\n"
        "    mtx_destroy(&m);\n"
        "    return 0;\n"
        "}\n";
    for (auto const& arm : arms) {
        SCOPED_TRACE(arm.format);
        auto const tc = loadToolchain(arm.target, arm.format);
        ASSERT_TRUE(tc.has_value());
        CompilationUnit cu = unitOf(*tc, source, "threads_floor.c");
        DiagnosticReporter rep;
        auto cuMir = buildCuMir(cu, *tc->grammar, *tc->target, *tc->format, tc->ccIndex, rep,
                                CompileOptions{DiagnosticBudget::libraryDefault()});
        ASSERT_TRUE(cuMir.has_value()) << allDiagText(rep);
        std::unordered_set<std::uint32_t> const before = externIds(cuMir->externImports);
        ASSERT_TRUE(synthesizeLibraryShims(*cuMir, rep)) << allDiagText(rep);
        ASSERT_EQ(rep.errorCount(), 0u) << allDiagText(rep);
        auto const tableEnd = static_cast<std::uint32_t>(cuMir->model.symbols().size());
        std::size_t minted = 0;
        for (auto const& e : cuMir->externImports) {
            if (before.contains(e.symbol.v)) continue;
            ++minted;
            EXPECT_GE(e.symbol.v, tableEnd)
                << e.mangledName << " minted as id " << e.symbol.v
                << " inside the semantic table (end " << tableEnd << "), which names it '"
                << (cuMir->model.recordFor(e.symbol) ? cuMir->model.recordFor(e.symbol)->name
                                                     : std::string_view{"?"})
                << "'";
        }
        EXPECT_GE(minted, 2u) << "mtx_init and mtx_destroy each import their vehicle's primitive";
    }
}

// ── The source tree ───────────────────────────────────────────────────────────

// SCOPE: every .cpp / .hpp under src/mir and src/opt, read as text, for a `SymbolId`
// whose braces hold arithmetic — the spelling all five minters had. `mir/mir.cpp` is
// the door's own body and the one file allowed it. ⚠ WHAT THE SCAN CANNOT SEE: a
// counter advanced OUTSIDE the braces (`SymbolId s{v}; ++v;`) — the pipeline arms above
// are what catch a minter by its result. NOT in scope, and not minters of a MIR
// module's ids: CST→HIR's lowering temporaries (locals, never a MIR symbol), the
// merge's allocator (it MAKES the merged module's id space and states its end), and
// the asm and link tiers (an assembled module's symbols carry their names; no table
// stands beside it — link/fresh_symbol_ids.hpp). MIR→LIR's block-symbol minter
// (src/lir/lowering/mir_to_lir.cpp) is outside these two tiers as well;
// `MirSymbolIdContinuation` is the door's leaf made for it.
TEST(SynthSymbolFloor, NoMirOrOptimizerSourceCountsASymbolIdOfItsOwn) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    std::size_t scanned = 0;
    bool        sawTheDoor = false;
    for (char const* const tier : {"mir", "opt"}) {
        std::filesystem::path const dir = *root / "src" / tier;
        for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            auto const ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp") continue;
            ++scanned;
            std::vector<std::string> const counted = countedSymbolIds(readWhole(entry.path()));
            bool const isTheDoor =
                std::filesystem::relative(entry.path(), *root / "src").generic_string() == "mir/mir.cpp";
            if (isTheDoor) {
                sawTheDoor = !counted.empty();
                EXPECT_EQ(counted.size(), 1u) << "the door mints in ONE place";
                continue;
            }
            for (std::string const& spelling : counted)
                ADD_FAILURE() << entry.path().string() << " counts a SymbolId of its own: `"
                              << spelling << "` — a module's fresh ids come from "
                                 "MirBuilder::mintSymbol alone";
        }
    }
    EXPECT_GT(scanned, 60u) << "the scan must have read both tiers";
    EXPECT_TRUE(sawTheDoor) << "CONTROL: the scan must see the door's own mint, or it sees nothing";
}

// SCOPE: every .cpp under src/, read as text. A file that declares a `MirBuilder` makes
// a module; unless it says where that module's ids end — it states a table's end, or it
// continues the ids of the module it rebuilds (directly, or through a clone-globals
// helper, each of which does) — the module it finishes is numbered from the symbols it
// happens to define, and the next minter counts inside the table again. NO FILE IS
// EXCUSED. `mir_text.cpp` was, on the sentence "a parsed module is made from no table";
// ✔MEASURED 2026-10-08 it is made from the text's own `symbols` table, and a name that
// table declared and the module did not define lost its id to a block symbol
// (tests/mir/test_mir_text_reader_never_aborts.cpp holds that text).
TEST(SynthSymbolFloor, EveryFileThatBuildsAModuleStatesOrContinuesItsSymbolIds) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    std::array<std::string_view, 7> const carriers{
        "stateSymbolIdEnd(", "continueSymbolIdsOf(", "carryModuleFactsOf(",
        "continueSymbolIdsPastImports(",
        "cloneGlobalsOrCarveOut(", "cloneGlobalsRemappingInitFunc(", "cloneGlobalsVerbatim("};
    std::size_t builders = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(*root / "src")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") continue;
        std::string const text = readWhole(entry.path());
        bool declares = false;
        std::string_view const needle = "MirBuilder";
        for (std::size_t at = text.find(needle); at != std::string::npos && !declares;
             at = text.find(needle, at + needle.size())) {
            std::size_t i = at + needle.size();
            if (i < text.size() && text[i] == '{') { declares = true; break; }   // MirBuilder{}
            std::size_t const afterType = i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            if (i == afterType) continue;                                        // MirBuilder& / ::
            std::size_t const nameAt = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
            if (i == nameAt) continue;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            declares = i < text.size() && text[i] == ';';                        // MirBuilder name;
        }
        if (!declares) continue;
        ++builders;
        std::string const rel =
            std::filesystem::relative(entry.path(), *root / "src").generic_string();
        bool carries = false;
        for (std::string_view const c : carriers) carries = carries || text.find(c) != std::string::npos;
        EXPECT_TRUE(carries) << rel << " builds a module and neither states nor continues its "
                                       "symbol-id space";
    }
    EXPECT_GE(builders, 15u) << "the scan must have found the module builders";

    // AND NO PRODUCT FILE SAYS ITS MODULE IS MADE FROM NOTHING BUT ITSELF. That
    // sentence (`stateSelfContainedSymbolIds`) is a hand-built fixture's: every module
    // the product makes is made from a name table, or replaces a module and carries
    // whatever that one said. A product entrance that reached for it would be stating
    // a counted end — the defect, with a signature. SCOPE: every .cpp and .hpp under
    // src/, read as text for a call or a declaration of that name; the door's own two
    // files declare and define it and are the only ones allowed to.
    std::size_t doorFiles = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(*root / "src")) {
        if (!entry.is_regular_file()) continue;
        auto const ext = entry.path().extension();
        if (ext != ".cpp" && ext != ".hpp") continue;
        if (readWhole(entry.path()).find("stateSelfContainedSymbolIds(") == std::string::npos) continue;
        std::string const rel =
            std::filesystem::relative(entry.path(), *root / "src").generic_string();
        if (rel == "mir/mir.hpp" || rel == "mir/mir.cpp") { ++doorFiles; continue; }
        ADD_FAILURE() << rel << " says a module is made from nothing but itself: state the "
                                "table it is made from, or continue the module it replaces";
    }
    EXPECT_EQ(doorFiles, 2u) << "the scan must have found the door's declaration and its definition";
}
