// ONLY THE LAST OPTIMIZATION STAGE A MODULE RECEIVES REPORTS A TRUNCATED FIXPOINT -- the closure of
// [[D-OPT-RELEASE-UNIT-FIXPOINT-CAP-WARNS-THOUGH-THE-PROGRAM-STAGE-RESUMES]].
//
// ✔MEASURED 2026-10-20 (the orchestrator; compile-and-keep, every 90th single-file example, baseline and
// release): 4 of 12 RELEASE compiles printed `X_OptFixpointTruncated` naming `release-unit` -- the UNIT stage's
// schedule, capped at 2 iterations by design -- and told the user to raise that cap, though the PROGRAM stage runs
// the full `release` schedule over the same functions before anything is lowered. A warning that cries wolf trains
// its reader to ignore the real one.
//
// What is pinned here:
//   * `opt::optimize` reports a truncation only when NO later stage resumes the module (`laterStageResumes`), and
//     a resumed truncation is silent -- the fixture truncates in both runs, so the silence is the flag's;
//   * the driver decides it from the STAGE (`optimizeModule`): a fixpoint capped low by an override says nothing
//     at the Unit stage and warns once at the Program stage -- never a list of pipeline names;
//   * the four examples the measurement named compile under `release`, through both stages, with no
//     `X_OptFixpointTruncated` -- on two targets (the Program stage still reports its own, so this also measures
//     that the LAST stage converges for them);
//   * ★ THE INVARIANT THE DECISION RESTS ON: a module is lowered only after a PROGRAM-stage optimize (the Unit
//     stage is never its last; `PipelineStage`'s contract). Every production route that lowers a module is driven
//     below through the stage seam (`pipeline_testing::setStageObserver`), and a route that lowers a module whose
//     last optimize was not Program-stage is red -- as is a route that records no Unit or no lowering at all. A
//     NEW lowering route is added to `kRoutes`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "diagnostic_count.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/object_format_schema.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/optimizer.hpp"
#include "program/compile_pipeline.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::test_support::countCode;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// ONE caller (`SymbolId{100}`) making five direct calls to ONE single-block leaf (`SymbolId{50}`) of six
// instructions: an `Inlining` traversal splices every call, so ONE traversal ends with the module still changing
// -- a fixpoint capped at 1 truncates by construction (test_inline_growth_budget.cpp's fixture, its numbers).
[[nodiscard]] Mir repeatedCallModule(TypeInterner& interner) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder   mb;
    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Global, SymbolVisibility::Default, /*noInline=*/false,
                   /*alwaysInline=*/false);
    MirBlockId const cEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(cEntry);
    MirInstId last{};
    for (std::int64_t i = 0; i < 5; ++i) last = mb.addConst(i32Lit(i + 1), i32);
    mb.addReturn(last);
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId lastCall{};
    for (int i = 0; i < 5; ++i) {
        MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
        MirInstId const ops[]      = {calleeAddr};
        lastCall                   = mb.addInst(MirOpcode::Call, ops, i32);
    }
    mb.addReturn(lastCall);
    return std::move(mb).finish();
}

// A pipeline of one `Inlining` traversal: it truncates on the fixture above.
[[nodiscard]] opt::OptPipeline capOnePipeline() {
    opt::OptPipeline pipe;
    pipe.name                      = "cap1";
    pipe.inlineThreshold           = 6;
    pipe.inlineCallerGrowthPercent = opt::kMaxInlineCallerGrowthPercent;
    pipe.schedule = opt::OptPipelineNode::fixpoint(1u, {opt::OptPipelineNode::leaf(opt::PassId::Inlining)});
    return pipe;
}

}  // namespace

// THE ENGINE: a truncation the caller says a later stage resumes is not reported; the same truncation in a run
// that is the last stage is (the control: same fixture, same pipeline, the flag alone differs).
TEST(FixpointReportStage, ATruncationALaterStageResumesIsNotReported) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    opt::OptPipeline const pipe = capOnePipeline();
    {
        TypeInterner       interner{CompilationUnitId{1}};
        Mir                mir = repeatedCallModule(interner);
        DiagnosticReporter rep;
        auto const         res = opt::optimize(mir, **targetR, interner, pipe, rep);
        ASSERT_TRUE(res.ok);
        ASSERT_FALSE(res.fixedPointReached) << "the fixture no longer truncates: the case is vacuous";
        EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptFixpointTruncated), 1u)
            << "the LAST stage must report its truncation";
    }
    {
        TypeInterner       interner{CompilationUnitId{1}};
        Mir                mir = repeatedCallModule(interner);
        DiagnosticReporter rep;
        auto const res = opt::optimize(mir, **targetR, interner, pipe, rep, {}, std::nullopt, {},
                                       opt::ModuleExtent::WholeImage, {}, /*laterStageResumes=*/true);
        ASSERT_TRUE(res.ok);
        ASSERT_FALSE(res.fixedPointReached) << "the resumed run truncated too, or the silence below proves nothing";
        EXPECT_EQ(countCode(rep, DiagnosticCode::X_OptFixpointTruncated), 0u)
            << "a truncation a later stage resumes told the user to raise a cap the topology fixes on purpose";
    }
}

// THE DRIVER: the decision is the STAGE's. One override capped low runs at both sites (an override runs at every
// site): the Unit stage says nothing, the Program stage -- the last -- warns once.
TEST(FixpointReportStage, AProgramStageFixpointCappedLowStillWarnsAndTheUnitStageDoesNot) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    opt::OptPipeline const pipe = capOnePipeline();
    CompileOptions         opts{DiagnosticBudget::libraryDefault()};
    opts.pipelineOverride = &pipe;
    std::map<PipelineStage, std::size_t> warned;
    for (PipelineStage const stage : {PipelineStage::Unit, PipelineStage::Program}) {
        TypeInterner       interner{CompilationUnitId{1}};
        Mir                mir = repeatedCallModule(interner);
        DiagnosticReporter rep;
        ASSERT_TRUE(optimizeModule(mir, **targetR, interner, opts, stage, opt::ModuleExtent::WholeImage, {}, rep))
            << "errorCount=" << rep.errorCount();
        warned[stage] = countCode(rep, DiagnosticCode::X_OptFixpointTruncated);
    }
    EXPECT_EQ(warned[PipelineStage::Unit], 0u) << "the Unit stage, which the Program stage resumes, warned";
    EXPECT_EQ(warned[PipelineStage::Program], 1u) << "the Program stage, the last, did not report its truncation";
}

// THE MEASUREMENT'S FOUR EXAMPLES, through both stages under `release`, on two targets: no truncation reported.
TEST(FixpointReportStage, TheFourExamplesCompileUnderReleaseWithNoTruncationReported) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    auto const grammar = *grammarR;
    struct Leg {
        char const* target;
        char const* format;
    };
    for (Leg const leg : {Leg{"x86_64", "pe64-x86_64-windows-exec"}, Leg{"arm64", "macho64-arm64-darwin-exec"}}) {
        auto targetR = TargetSchema::loadShipped(leg.target);
        ASSERT_TRUE(targetR.has_value()) << leg.target;
        auto formatR = ObjectFormatSchema::loadShipped(leg.format);
        ASSERT_TRUE(formatR.has_value()) << leg.format;
        DiagnosticReporter abiRep;
        auto const         abi = dss::ffi::resolveAbi(**targetR, **formatR, abiRep);
        ASSERT_TRUE(abi.has_value() && abi->cc != nullptr) << leg.format;
        auto const ccSpan  = (*targetR)->callingConventions();
        auto const ccIndex = static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
        CompileOptions opts{DiagnosticBudget::libraryDefault()};
        opts.config = CompileConfig::Release;
        for (char const* name :
             {"extern_fn_definition", "long_primitive_smoke", "sizeof_const_context", "switch_fallthrough_attribute"}) {
            fs::path const source = dss::test::repoRoot() / "examples" / "c" / name / "main.c";
            std::ifstream  in{source, std::ios::binary};
            ASSERT_TRUE(in.good()) << "cannot read " << source.generic_string();
            std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            UnitBuilder builder{grammar, DiagnosticBudget::libraryDefault()};
            builder.addInMemory(std::move(text), "main.c");
            CompilationUnit const cu = std::move(builder).finish();
            DiagnosticReporter    rep;
            auto const assembled = assembleUnit(cu, *grammar, **targetR, **formatR, ccIndex, rep, opts);
            ASSERT_TRUE(assembled.has_value()) << name << " on " << leg.format << ": errorCount=" << rep.errorCount();
            std::string said;
            for (auto const& d : rep.all())
                if (d.code == DiagnosticCode::X_OptFixpointTruncated) said += "\n  " + d.actual;
            EXPECT_TRUE(said.empty()) << name << " on " << leg.format << " reported a truncation:" << said;
        }
    }
}

// ★ THE INVARIANT: every production route lowers a module only after a PROGRAM-stage optimize ran on it.
namespace {

// What one route recorded, judged: empty when every lowered module's last optimize at its address was
// Program-stage and the route recorded a Unit-stage optimize and a lowering (not vacuous).
[[nodiscard]] std::string judged(std::vector<StageEvent> const& events) {
    std::map<void const*, PipelineStage> lastAt;
    bool                                 sawUnit = false;
    std::size_t                          lowered = 0;
    std::string                          problems;
    for (auto const& e : events) {
        if (e.kind == StageEvent::Kind::Optimized) {
            lastAt[e.module] = e.stage;
            sawUnit          = sawUnit || e.stage == PipelineStage::Unit;
            continue;
        }
        ++lowered;
        auto const it = lastAt.find(e.module);
        if (it == lastAt.end()) problems += " a module was lowered with no optimize run on it;";
        else if (it->second != PipelineStage::Program)
            problems += " a module was lowered after a Unit-stage optimize, with no Program stage after it;";
        if (it != lastAt.end()) lastAt.erase(it);
    }
    if (!sawUnit) problems += " no Unit-stage optimize was recorded (the route proves nothing);";
    if (lowered == 0) problems += " no module was lowered (the route proves nothing);";
    return problems;
}

struct Route {
    char const*                                  name;
    std::function<bool(fs::path const& scratch)> run;   // true when the compile succeeded
};

[[nodiscard]] fs::path writeSource(fs::path const& dir, char const* name, char const* text) {
    fs::path const p = dir / name;
    std::ofstream{p, std::ios::binary} << text;
    return p;
}

char const* const kHelper = "int helper(int x) { return x + 1; }\n";
char const* const kMain   = "int helper(int x);\nint main(void) { return helper(41); }\n";

// Every route that lowers a module in production: the N==1 sole unit, the N>1 merged module, each static-archive
// member, the thin-LTO import stage (then the merge), and `assembleUnit`. A NEW lowering route is added here.
std::vector<Route> const kRoutes = {
    {"compileFiles: the N==1 sole unit",
     [](fs::path const& s) {
         Program prog;
         return prog.compileFiles({writeSource(s, "one.c", kHelper).generic_string()}, "c",
                                  {"x86_64:elf64-x86_64-linux"}) == 0;
     }},
    {"compileUnits: the N>1 merged module",
     [](fs::path const& s) {
         Program prog;
         return prog.compileUnits({writeSource(s, "helper.c", kHelper).generic_string(),
                                   writeSource(s, "main.c", kMain).generic_string()},
                                  "c", {"x86_64:elf64-x86_64-linux"}) == 0;
     }},
    {"compileUnits: each static-archive member",
     [](fs::path const& s) {
         Program prog;
         return prog.compileUnits({writeSource(s, "helper.c", kHelper).generic_string(),
                                   writeSource(s, "main.c", kMain).generic_string()},
                                  "c", {"x86_64:elf64-x86_64-linux-staticlib"}) == 0;
     }},
    {"compileUnits --lto=thin: the import stage, then the merged module",
     [](fs::path const& s) {
         Program prog;
         prog.setLtoMode(LtoModeArg::Thin);
         return prog.compileUnits({writeSource(s, "helper.c", kHelper).generic_string(),
                                   writeSource(s, "main.c", kMain).generic_string()},
                                  "c", {"x86_64:elf64-x86_64-linux"}) == 0;
     }},
    {"assembleUnit: one unit to its assembled module",
     [](fs::path const&) {
         auto grammarR = GrammarSchema::loadShipped("c");
         auto targetR  = TargetSchema::loadShipped("x86_64");
         auto formatR  = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
         if (!grammarR || !targetR || !formatR) return false;
         DiagnosticReporter rep;
         auto const         abi = dss::ffi::resolveAbi(**targetR, **formatR, rep);
         if (!abi || abi->cc == nullptr) return false;
         auto const  ccSpan  = (*targetR)->callingConventions();
         auto const  ccIndex = static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
         UnitBuilder builder{*grammarR, DiagnosticBudget::libraryDefault()};
         builder.addInMemory(kHelper, "helper.c");
         CompilationUnit const cu = std::move(builder).finish();
         return assembleUnit(cu, **grammarR, **targetR, **formatR, ccIndex, rep,
                             CompileOptions{DiagnosticBudget::libraryDefault()})
             .has_value();
     }},
};

}  // namespace

// The seam's events come from a CU pool's workers, and the seam hands them over one at a time: the observer
// below takes no lock, and counts the calls that found another still inside it -- such a call records nothing
// (so a seam that stopped serializing reddens here by name rather than racing on `events`). NO MUTATION ARM holds
// the seam's lock: without it, this case reddens only when two workers' calls happen to overlap, which no case
// can force without holding the observer open on a timer that every green run would pay -- this count is what
// names an overlap when one happens.
TEST(FixpointReportStage, EveryRouteLowersAModuleOnlyAfterItsProgramStage) {
    for (auto const& route : kRoutes) {
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "fixpoint-report-stage"};
        scratch.useAsCwd();
        std::vector<StageEvent> events;
        std::atomic<int>        inside{0};
        std::atomic<int>        overlapped{0};
        pipeline_testing::setStageObserver([&](StageEvent const& e) {
            if (inside.fetch_add(1) == 0) events.push_back(e);
            else overlapped.fetch_add(1);
            inside.fetch_sub(1);
        });
        bool const compiled = route.run(scratch.path());
        pipeline_testing::setStageObserver({});
        EXPECT_TRUE(compiled) << route.name << ": the compile failed, so its stages were not all recorded";
        EXPECT_EQ(overlapped.load(), 0) << route.name << ": the seam handed its observer two events at once";
        std::string const problems = judged(events);
        EXPECT_TRUE(problems.empty()) << route.name << " (" << events.size() << " stage event(s)):" << problems;
    }
}

// THE JUDGE ITSELF, on synthesized event lists: a lowering after a Unit-only optimize, a lowering of a module
// nothing optimized, and an empty route are each red; the production shape is green.
TEST(FixpointReportStage, TheJudgeRefusesALoweringAfterAUnitOnlyOptimize) {
    int a = 0, b = 0;
    using K = StageEvent::Kind;
    EXPECT_EQ(judged({{K::Optimized, PipelineStage::Unit, &a}, {K::Optimized, PipelineStage::Program, &a},
                      {K::Lowered, PipelineStage::Program, &a}}),
              "");
    EXPECT_NE(judged({{K::Optimized, PipelineStage::Unit, &a}, {K::Lowered, PipelineStage::Program, &a}}), "");
    EXPECT_NE(judged({{K::Optimized, PipelineStage::Unit, &a}, {K::Lowered, PipelineStage::Program, &b}}), "");
    EXPECT_NE(judged({{K::Optimized, PipelineStage::Program, &a}, {K::Lowered, PipelineStage::Program, &a}}), "")
        << "a route that records no Unit stage proves nothing";
    EXPECT_NE(judged({}), "");
}
