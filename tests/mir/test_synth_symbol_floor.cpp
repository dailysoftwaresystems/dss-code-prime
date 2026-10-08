// THE NAME-TABLE FLOOR of the MIR-tier synthesis passes (P69 round 4, lane lm) —
// mir/merge/synth_symbol_floor.hpp.
//
// ═══ WHAT IS PINNED ════════════════════════════════════════════════════════
//
// `realizeEntryShape`, `synthesizeSehFunclets` and `synthesizeThreadsShim` append
// symbols to a module whose ids the CALLER later names through its own table — the
// semantic model's records on the single-CU route, the merge's `symbolNames` on the
// whole-program one. Each pass minted from a scan of the MODULE alone (functions,
// globals, externs), which cannot see the ids the table gives to things the module
// never holds: a local, a parameter, a typedef, a shipped constant, `__func__`. The
// review's finding named the threads pass (its helper-import floor; the call_once
// adapter it used to define was named `_thrd_success` that way); the same scan in the
// entry-shape pass was MEASURED publishing the stack-vector init of an x86_64 Linux
// image as `T __func__`. So:
//   * the floor itself (the module's ids + the table's end);
//   * each pass mints above a table end the module does not reach;
//   * the single-CU pipeline: an image publishes no name its source does not declare,
//     and the threads helpers mint above the semantic table;
//   * no production source passes the hand-built-module end, `kNoNameTable`.
//
// RED-ON-DISABLE: `highestTakenSymbolIdV` ignoring `nameTableEnd` (the old module-only
// scan) reds the pass arms and both pipeline arms; the control arms (a module without
// a name table) stay green under it.

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
#include "link/object_format_schema.hpp"
#include "mir/merge/synth_pe_startup.hpp"
#include "mir/merge/synth_seh_funclets.hpp"
#include "mir/merge/synth_symbol_floor.hpp"
#include "mir/merge/synth_threads_shim.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "program/compile_pipeline.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// A one-function module: the entry `sig` bound to SymbolId{100}, body `return 0;`.
Mir buildEntryOnly(TypeInterner& in, TypeId sig) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
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
Mir buildSehParent(TypeInterner& in, SymbolId sym) {
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const u32  = in.primitive(TypeKind::U32);
    TypeId const pI32 = in.pointer(i32);
    TypeId const sig  = in.fnSig({}, i32, CallConv::CcMS64);
    MirBuilder mb;
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
Mir buildCallsMtxLock(TypeInterner& in) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 1> const lockParams{pV};
    TypeId const lockSig = in.fnSig(lockParams, i32, CallConv::CcMS64);
    MirBuilder mb;
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

} // namespace

// ── The floor ─────────────────────────────────────────────────────────────────

TEST(SynthSymbolFloor, HighestTakenReadsTheModuleAndTheNameTable) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
    for (std::uint32_t const sym : {3u, 5u}) {
        mb.addFunction(in.fnSig({}, i32, CallConv::CcSysV), SymbolId{sym});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32));
    }
    Mir const mir = std::move(mb).finish();
    std::vector<ExternImport> ext(1);
    ext[0].symbol      = SymbolId{9};
    ext[0].mangledName = "puts";

    EXPECT_EQ(highestTakenSymbolIdV(mir, ext, kNoNameTable), 9u)
        << "CONTROL: with no name table the module's own highest id (an extern's) is the floor";
    EXPECT_EQ(highestTakenSymbolIdV(mir, ext, 6u), 9u)
        << "a table ending below the module's ids changes nothing";
    EXPECT_EQ(highestTakenSymbolIdV(mir, ext, 20u), 19u)
        << "a table holding ids up to 19 takes them all — the floor is its last id";
}

TEST(SynthSymbolFloor, NameTableEndOfIsOnePastTheLargestKey) {
    EXPECT_EQ(nameTableEndOf({}), 0u);
    EXPECT_EQ(nameTableEndOf({{4u, "a"}, {11u, "b"}, {2u, "c"}}), 12u);
}

// ── Each pass mints above the caller's table ─────────────────────────────────

// The review's finding: the helper imports. The table here ends at 500, far above
// every id the module holds (100) and every recipe (10) — as the semantic table of a
// TU including <threads.h> ends above its shipped constants.
TEST(SynthSymbolFloor, ThreadsHelperImportsMintAboveTheNameTable) {
    LibrarySynthesis const win32{LibrarySynthVehicle::Win32,
                                 RuntimeLibraryRole::SystemPrimitives, "kernel32.dll"};
    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "mtx_lock"}};
    for (std::uint32_t const tableEnd : {kNoNameTable, 500u}) {
        SCOPED_TRACE(tableEnd);
        TypeInterner in{CompilationUnitId{1}};
        Mir mir = buildCallsMtxLock(in);
        std::vector<ExternImport> ext;
        DiagnosticReporter rep;
        ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, win32,
                                          CSymbolDecorationScheme::None, ext, tableEnd, rep))
            << allDiagText(rep);
        ASSERT_FALSE(ext.empty()) << "mtx_lock's body imports its kernel32 primitive";
        for (auto const& e : ext) {
            if (tableEnd == kNoNameTable) {
                EXPECT_GT(e.symbol.v, 100u)
                    << "CONTROL: " << e.mangledName << " clears the module's ids";
            } else {
                EXPECT_GE(e.symbol.v, tableEnd)
                    << e.mangledName << " was minted INSIDE the caller's name table (id "
                    << e.symbol.v << " < " << tableEnd << "): the table names it as "
                       "something else";
            }
        }
    }
}

// The entry shape's init — a GLOBAL definition — and the CRT imports it calls.
TEST(SynthSymbolFloor, EntryInitAndItsImportsMintAboveTheNameTable) {
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
    std::uint32_t const tableEnd = 700u;
    for (auto const& arm : arms) {
        SCOPED_TRACE(arm.label);
        TypeInterner in{CompilationUnitId{1}};
        TypeId const i32 = in.primitive(TypeKind::I32);
        TypeId const ppc = in.pointer(in.pointer(in.primitive(TypeKind::Char)));
        std::vector<TypeId> params{i32, ppc};
        if (arm.verb == EntryMaterialization::ArgcArgvEnvp) params.push_back(ppc);
        Mir mir = buildEntryOnly(in, in.fnSig(params, i32, arm.cc));
        std::optional<SymbolId>   entry = SymbolId{100};
        std::vector<ExternImport> ext;
        DiagnosticReporter        rep;
        ASSERT_TRUE(realizeEntryShape(mir, in, entry, ext, arm.verb, arm.pa,
                                      CSymbolDecorationScheme::None, arm.formatName,
                                      tableEnd, rep))
            << allDiagText(rep);
        ASSERT_TRUE(entry.has_value());
        ASSERT_NE(entry->v, 100u) << "the entry is retargeted to the synthesized init";
        EXPECT_GE(entry->v, tableEnd)
            << "the init (a GLOBAL definition) was minted inside the caller's name table";
        for (auto const& e : ext)
            EXPECT_GE(e.symbol.v, tableEnd) << e.mangledName << " minted inside the table";
    }
}

TEST(SynthSymbolFloor, SehFuncletAndPersonalityMintAboveTheNameTable) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSehParent(in, SymbolId{100});
    std::unordered_set<std::uint32_t> const before = funcIds(mir);
    std::vector<ExternImport> ext;
    std::vector<MirSehScope>  scopes;
    DiagnosticReporter        rep;
    std::uint32_t const tableEnd = 900u;
    ASSERT_TRUE(synthesizeSehFunclets(mir, in, ext, peSehPersonality(),
                                      CSymbolDecorationScheme::None,
                                      "pe64-x86_64-windows-exec", scopes, tableEnd, rep))
        << allDiagText(rep);
    ASSERT_EQ(scopes.size(), 1u);
    std::size_t funclets = 0;
    for (std::uint32_t const v : funcIds(mir)) {
        if (before.contains(v)) continue;
        ++funclets;
        EXPECT_GE(v, tableEnd) << "a funclet was minted inside the caller's name table";
    }
    EXPECT_EQ(funclets, 1u);
    ASSERT_EQ(ext.size(), 1u);
    EXPECT_GE(ext[0].symbol.v, tableEnd) << "the personality import minted inside the table";
}

// ── The single-CU pipeline ────────────────────────────────────────────────────

// What the review's arms reach in a real build: the lower half names every defined
// symbol through the semantic model, so a synthesized definition minted inside the
// model's id space is PUBLISHED under the name the model gives that id. ✔MEASURED
// before the fix (P69 round 4, the round-3 dsscp): the first arm's x86_64 Linux image
// carried `T __func__` — its stack-vector init, named after main's `__func__`.
// No string literal in any source: a literal's synthetic global is minted above the
// semantic table and would lift the module's highest id clear of it, hiding the case.
TEST(SynthSymbolFloor, ASingleUnitPublishesNoNameItsSourceDoesNotDeclare) {
    struct Arm {
        char const*           target;
        char const*           format;
        char const*           source;
        std::set<std::string> declared;
    };
    std::vector<Arm> const arms{
        {"x86_64", "elf64-x86_64-linux-exec",
         "int main(int argc, char **argv, char **envp) { return envp[0] ? argc - 1 : 2; }\n",
         {"main"}},
        {"x86_64", "pe64-x86_64-windows-exec",
         "int main(int argc, char **argv) { return argv[0] ? argc - 1 : 2; }\n",
         {"main"}},
        {"x86_64", "pe64-x86_64-windows-exec",
         "int guarded(volatile int *p) {\n"
         "    int r = 0;\n"
         "    __try { r = *p; } __except (1) { r = -1; }\n"
         "    return r;\n"
         "}\n"
         "int main(void) { volatile int v = 41; return guarded(&v) + 1; }\n",
         {"main", "guarded"}},
    };
    for (auto const& arm : arms) {
        SCOPED_TRACE(std::string{arm.format} + ": " + arm.source);
        auto const tc = loadToolchain(arm.target, arm.format);
        ASSERT_TRUE(tc.has_value());
        CompilationUnit cu = unitOf(*tc, arm.source, "floor.c");
        DiagnosticReporter rep;
        auto const mod = assembleUnit(cu, *tc->grammar, *tc->target, *tc->format, tc->ccIndex,
                                      rep, CompileOptions{DiagnosticBudget::libraryDefault()});
        ASSERT_TRUE(mod.has_value()) << allDiagText(rep);
        ASSERT_EQ(rep.errorCount(), 0u) << allDiagText(rep);
        for (auto const& s : mod->symbols) {
            EXPECT_TRUE(arm.declared.contains(s.name))
                << "the module publishes '" << s.name << "' (symbol " << s.symbol.v
                << "), which the source does not declare: a synthesized symbol took an id "
                   "the semantic table names";
        }
    }
}

// The review's arm itself, through the real front end: the helpers `mtx_init` and
// `mtx_destroy` call are minted above the CU's semantic table, which holds
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

// ── The seams ─────────────────────────────────────────────────────────────────

// `kNoNameTable` is the end of a HAND-BUILT module's table. Every production seam
// has a real table and passes its end; a seam passing this constant would mint blind
// again and compile. So no file under src/ but the floor's own header names it.
TEST(SynthSymbolFloor, NoProductionSourcePassesTheHandBuiltModuleEnd) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    std::filesystem::path const src = *root / "src";
    std::size_t scanned = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        auto const ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
        ++scanned;
        if (entry.path().filename() == "synth_symbol_floor.hpp") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        EXPECT_EQ(buf.str().find("kNoNameTable"), std::string::npos)
            << entry.path().string() << " names kNoNameTable — a production seam has a name "
               "table and must pass its end";
    }
    EXPECT_GT(scanned, 100u) << "the scan must have read the source tree";
}
