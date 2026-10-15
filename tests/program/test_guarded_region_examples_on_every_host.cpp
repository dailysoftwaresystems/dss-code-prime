// THE pe64 `__try` EXAMPLES, BUILT BY THE DRIVER ON EVERY HOST.
//
// An example that runs a `__try` is `runOn: windows`. On any other host the
// in-process examples runner builds its BASELINE arm and runs nothing, and the
// CLI runner builds nothing at all — so the RELEASE arm of every one of them,
// which for several is the subject (a filter that reads a value the optimizer
// shared with the code before the region; a handler that reads one its filter
// computed), was built on a Windows host only, one leg of five.
//
// This binary builds those sources for pe64 through the driver, in the pipeline
// each case names, on whatever host it runs on, and reads what the image says
// about its guarded regions. It cannot run them — that stays the Windows legs' —
// but it holds on every host that:
//   * the release pipeline BUILDS each source (a refusal or an abort of the
//     compiler in release is exactly what three of these examples exist for);
//   * the image has one scope record for every `__try` the source writes, each
//     with its own filter function, a handler inside its function and outside
//     its own range, and — where two ranges share an address — the inner one
//     first in the table;
//   * the three callees of the handler example save EVERY vector register a
//     call preserves, in both pipelines, so whichever of them a caller chose
//     for the `double` its handler reads, the callee overwrote it and said
//     where it saved it;
//   * no handler of any of these images, in either pipeline, opens by taking a
//     value from a register a call does not preserve.
//
// ★ ONE BUILD PER (SOURCE, PIPELINE), READ BY EVERY CASE THAT NEEDS IT
// (`imageOf`): an image build is this tier's whole cost.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "pe_image_readers.hpp"
#include "program/cli_args.hpp"   // CompileConfig
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using dss::CompileConfig;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::HandlerFirstRead;
using dss::test_support::HandlerTakes;
using dss::test_support::Location;
using dss::test_support::PeFunctionUnwind;
using dss::test_support::PeGuardedFunction;
using dss::test_support::PeScopeRecord;
using dss::test_support::PeSection;
using dss::test_support::ScratchDir;
using dss::test_support::firstValueAHandlerTakes;
using dss::test_support::kPeUwopSaveXmm128;
using dss::test_support::kPeUwopSaveXmm128Far;
using dss::test_support::peFunctionUnwinds;
using dss::test_support::peGuardedFunctions;
using dss::test_support::peRvaToOffset;
using dss::test_support::peSections;
using dss::test_support::rdU32;
using dss::test_support::readAllBytes;

namespace {

constexpr std::string_view kPeExecSpec = "x86_64:pe64-x86_64-windows-exec";

// The sources. Each is an example of the corpus that guards a region and runs on
// Windows alone; the first five have a subject only the built image or its run
// shows, the sixth is here because it guards regions too.
constexpr std::array<char const*, 6> kExamples{
    "seh_filter_reads_a_value_computed_before_the_region",
    "seh_handler_reads_values_born_before_the_region",
    "seh_nested_regions_reach_their_own_handler",
    "seh_filter_with_a_branch",
    "seh_handler_names_what_its_filter_named",
    "windows_exception_status_family",
};

[[nodiscard]] std::string pipelineName(CompileConfig c) {
    return c == CompileConfig::Release ? "release" : "baseline";
}

struct Built {
    int                       rc = -1;
    std::string               diagnostics;
    std::vector<std::uint8_t> image;
    std::string               source;   // the example's `main.c`, as built
};

// `examples/c/<example>/main.c` built for pe64 in `config` — once per process.
[[nodiscard]] Built const& imageOf(std::string const& example, CompileConfig config) {
    static std::map<std::pair<std::string, int>, Built> cache;
    auto const key = std::make_pair(example, static_cast<int>(config));
    if (auto const it = cache.find(key); it != cache.end()) return it->second;

    Built b;
    fs::path const src = dss::test::repoRoot() / "examples" / "c" / example / "main.c";
    {
        std::ifstream in(src, std::ios::binary);
        b.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ScratchDir scratch{Location::InsideRepo,
                       "guarded-example-" + example + "-" + pipelineName(config)};
    scratch.useAsCwd();
    auto const out = scratch.path() / "out";
    Program prog;
    prog.setOutputDir(out);
    prog.setCompileConfig(config);
    DiagnosticReporter rep;
    b.rc = prog.compileFiles({src.generic_string()}, "c", {std::string{kPeExecSpec}}, rep);
    for (auto const& d : rep.all()) {
        b.diagnostics += d.contextPrefix;
        b.diagnostics += ' ';
        b.diagnostics += d.actual;
        b.diagnostics += '\n';
    }
    b.image = readAllBytes(out / "main.exe");
    return cache.emplace(key, std::move(b)).first->second;
}

// How many `__try` the source WRITES: the keyword outside comments and outside
// string and character literals.
[[nodiscard]] std::size_t regionsWritten(std::string_view s) {
    constexpr std::string_view kKeyword = "__try";
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "/*") == 0) {
            std::size_t const end = s.find("*/", i + 2);
            i = end == std::string_view::npos ? s.size() : end + 2;
        } else if (s.compare(i, 2, "//") == 0) {
            std::size_t const end = s.find('\n', i);
            i = end == std::string_view::npos ? s.size() : end;
        } else if (s[i] == '"' || s[i] == '\'') {
            char const quote = s[i++];
            while (i < s.size() && s[i] != quote) i += s[i] == '\\' ? 2 : 1;
            ++i;
        } else if (s.compare(i, kKeyword.size(), kKeyword) == 0) {
            ++n;
            i += kKeyword.size();
        } else {
            ++i;
        }
    }
    return n;
}

// The begin RVA of every function the image's function table lists.
[[nodiscard]] std::set<std::uint32_t> functionBegins(std::vector<std::uint8_t> const& img) {
    std::set<std::uint32_t> out;
    for (PeSection const& s : peSections(img)) {
        if (s.name != ".pdata") continue;
        std::size_t const sz = std::min<std::size_t>(s.vsize, s.rawSize);
        for (std::size_t e = 0; e + 12 <= sz; e += 12) out.insert(rdU32(img, s.rawPtr + e));
    }
    return out;
}

// What an image must say about its guarded regions, whichever source it is.
void expectARecordPerRegion(std::string const& example, CompileConfig config) {
    SCOPED_TRACE(example + " [" + pipelineName(config) + "]");
    Built const& b = imageOf(example, config);
    ASSERT_EQ(b.rc, 0) << "the " << pipelineName(config) << " pipeline must build this "
                          "source for pe64 on every host\n" << b.diagnostics;
    ASSERT_FALSE(b.image.empty());
    ASSERT_FALSE(b.source.empty());

    std::size_t const written = regionsWritten(b.source);
    ASSERT_GE(written, 1u) << "the premise: the source writes a `__try`";
    auto const guarded = peGuardedFunctions(b.image);
    auto const begins  = functionBegins(b.image);
    std::size_t             records = 0;
    std::set<std::uint32_t> filters;
    for (PeGuardedFunction const& f : guarded) {
        for (std::size_t i = 0; i < f.records.size(); ++i) {
            PeScopeRecord const& r = f.records[i];
            ++records;
            filters.insert(r.filter);
            EXPECT_LT(r.begin, r.end) << "a record guards a non-empty range";
            EXPECT_TRUE(r.begin >= f.begin && r.end <= f.end)
                << "a record's range lies in its own function: [" << r.begin << ", "
                << r.end << ") in [" << f.begin << ", " << f.end << ")";
            EXPECT_TRUE(begins.contains(r.filter))
                << "the filter of a record is a function of the image — the "
                   "function table lists " << r.filter << " as the start of none";
            EXPECT_TRUE(r.target >= f.begin && r.target < f.end)
                << "a handler is code of the function that guards the region";
            EXPECT_TRUE(r.target < r.begin || r.target >= r.end)
                << "a handler lies outside the range it handles: a fault in it "
                   "would re-enter it";
            for (std::size_t j = i + 1; j < f.records.size(); ++j) {
                PeScopeRecord const& later = f.records[j];
                bool const apart = r.end <= later.begin || later.end <= r.begin;
                EXPECT_TRUE(apart || (later.begin <= r.begin && r.end <= later.end))
                    << "of two records that share an address the one listed FIRST "
                       "is the inner one — the handler routine takes the first "
                       "record that holds the faulting address. Got [" << r.begin
                    << ", " << r.end << ") then [" << later.begin << ", " << later.end
                    << ")";
            }
        }
    }
    EXPECT_EQ(records, written)
        << "one scope record for every `__try` the source writes";
    EXPECT_EQ(filters.size(), written)
        << "and every region has a filter function of its own";
}

} // namespace

// The release pipeline builds every one of these sources on every host, and the
// image says of each region what the dispatcher needs.
//   * a filter that reads what the function computed before the region — the
//     release arm was REFUSED before its fix;
//   * a handler that reads values born before the region — the arm whose
//     register allocation differs most from the baseline's;
//   * nested regions — the order of the records is the whole subject;
//   * a filter whose expression has a branch — refused in both pipelines before
//     its fix;
//   * a handler that names what its filter named — the release arm ABORTED THE
//     COMPILER before its fix;
//   * the status names of the system header, where two regions catch a fault by
//     its name.
TEST(GuardedRegionExamples, TheReleasePipelineBuildsEachWithARecordPerRegion) {
    for (char const* example : kExamples) {
        expectARecordPerRegion(example, CompileConfig::Release);
    }
}

// The baseline of the one example whose image the case below also reads — so
// that case's two images are both held to the same statement.
TEST(GuardedRegionExamples, TheBaselineOfTheHandlerExampleHasARecordPerRegionToo) {
    expectARecordPerRegion("seh_handler_reads_values_born_before_the_region",
                           CompileConfig::Debug);
}

// ── THE CALLEES A FAULT UNWINDS THROUGH SAVE EVERY REGISTER A CALLER COULD HAVE
//    CHOSEN ──────────────────────────────────────────────────────────────────
//
// Cases 29, 31 and 32 of the handler example fault INSIDE A CALLEE: the guarding
// function keeps a `double` across the call, the callee overwrites vector
// registers and faults, and the handler reads the `double` — right only if the
// unwinder put the caller's register back from the slot the callee's unwind
// information names. The case tells a wrong save code from a right one ONLY IF
// THE CALLEE OVERWROTE THE REGISTER THE CALLER CHOSE. ✔MEASURED 2026-10-10 by the
// review of that example: its callees, written with ten NAMED doubles, saved
// three registers at baseline (a named object is memory there), and the cases
// answered right because caller and callee happened to choose the same one.
//
// So the example now keeps ten UNNAMED values across the callee's call, and this
// case holds the property the example leans on, in BOTH pipelines, read off the
// image: at least three functions save all ten vector registers a call preserves
// (xmm6..xmm15), at least one of them guarding a region — and every vector
// register that any OTHER guarding function saves (a caller's `double`) is one
// each of those three saved. A change of the allocator's order that moves a
// caller's choice, or a callee that stops needing all ten, fails here by name
// instead of quietly blunting the example.
TEST(GuardedRegionExamples, TheCalleesAFaultUnwindsThroughSaveEveryRegisterACallerChose) {
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        SCOPED_TRACE(pipelineName(config));
        Built const& b = imageOf("seh_handler_reads_values_born_before_the_region", config);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        auto const fns = peFunctionUnwinds(b.image);
        ASSERT_GE(fns.size(), 20u) << "the premise: the image's function table was read";

        auto const vectorSaves = [](PeFunctionUnwind const& f) {
            std::set<unsigned> regs;
            for (auto const& c : f.codes) {
                if (c.op == kPeUwopSaveXmm128 || c.op == kPeUwopSaveXmm128Far) {
                    regs.insert(unsigned{c.info});
                }
            }
            return regs;
        };
        std::set<unsigned> const everyPreserved{6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        std::size_t        savesAll = 0, savesAllAndGuards = 0;
        std::set<unsigned> chosenByACaller;
        for (PeFunctionUnwind const& f : fns) {
            std::set<unsigned> const regs = vectorSaves(f);
            if (regs == everyPreserved) {
                ++savesAll;
                if (f.guards) ++savesAllAndGuards;
            } else if (f.guards) {
                chosenByACaller.insert(regs.begin(), regs.end());
            }
        }
        EXPECT_GE(savesAll, 3u)
            << "the three callees each keep ten values across a call and save all "
               "ten of xmm6..xmm15";
        EXPECT_GE(savesAllAndGuards, 1u)
            << "one of them guards a region of its own (case 32)";
        EXPECT_FALSE(chosenByACaller.empty())
            << "the premise: a guarding function keeps a `double` for its handler "
               "in a call-preserved vector register";
        for (unsigned const reg : chosenByACaller) {
            EXPECT_TRUE(everyPreserved.contains(reg))
                << "a guarding function saves xmm" << reg << ", which the callees "
                   "that save every call-preserved vector register do not save";
        }
    }
}

// ── NO HANDLER TAKES A VALUE FROM A REGISTER A CALL DOES NOT PRESERVE ─────────
//
// A handler is entered by the unwinder, which hands it the frame and the
// registers a call preserves and nothing else. So the first register a handler
// reads without having written it must be one of those — the question
// `firstValueAHandlerTakes` asks of the image. It is asked here of EVERY handler
// of every one of these images, in BOTH pipelines: the block a filter leaves
// behind in its function once led into the handler, and what a handler read was
// then live to the end of the function — a wrong answer to "what does the
// handler's entry hold" that no run on one host would have shown.
//
// The walk decodes the few forms a handler opens with and answers `NotDecoded`
// for the rest, so "none takes a volatile register" could be held by a walk
// that decoded nothing. It is not: in each pipeline at least one handler of
// these images must be seen TAKING a value — from a call-preserved register or
// out of the frame.
TEST(GuardedRegionExamples, NoHandlerTakesAValueACallDoesNotPreserve) {
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        std::size_t handlers = 0, takeAValue = 0, takeNothing = 0, notDecoded = 0;
        for (char const* example : kExamples) {
            SCOPED_TRACE(std::string{example} + " [" + pipelineName(config) + "]");
            Built const& b = imageOf(example, config);
            ASSERT_EQ(b.rc, 0) << b.diagnostics;
            auto const secs = peSections(b.image);
            for (PeGuardedFunction const& f : peGuardedFunctions(b.image)) {
                for (PeScopeRecord const& r : f.records) {
                    std::size_t const at = peRvaToOffset(secs, r.target);
                    ASSERT_NE(at, 0u) << "a handler's address lies in no section";
                    HandlerFirstRead const first = firstValueAHandlerTakes(b.image, at);
                    ++handlers;
                    switch (first.takes) {
                        case HandlerTakes::APreservedRegister:
                        case HandlerTakes::AValueOfTheFrame:   ++takeAValue;  break;
                        case HandlerTakes::NothingFromOutside: ++takeNothing; break;
                        case HandlerTakes::NotDecoded:         ++notDecoded;  break;
                        case HandlerTakes::AVolatileRegister:  break;
                    }
                    EXPECT_NE(first.takes, HandlerTakes::AVolatileRegister)
                        << "the handler at " << r.target << " of the function at "
                        << f.begin << " reads register " << first.reg
                        << " before writing it — a register which a call does not "
                           "preserve, so the unwinder hands the handler no value "
                           "in it";
                }
            }
        }
        EXPECT_GE(takeAValue, 1u)
            << pipelineName(config) << ": of " << handlers << " handler(s), "
            << takeNothing << " open by taking nothing from outside and "
            << notDecoded << " in a form the walk does not decode — none was seen "
               "taking a value, so the statement above was held by nothing";
    }
}
