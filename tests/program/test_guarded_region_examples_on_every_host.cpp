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
//   * no handler of any of these images, in either pipeline, reads a register
//     a call does not preserve — general or vector, on any path to the
//     function's return — before writing it.
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
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using dss::CompileConfig;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::HandlerRead;
using dss::test_support::HandlerWalk;
using dss::test_support::Location;
using dss::test_support::PeFunctionUnwind;
using dss::test_support::PeGuardedFunction;
using dss::test_support::PeScopeRecord;
using dss::test_support::PeSection;
using dss::test_support::ScratchDir;
using dss::test_support::everyValueAHandlerTakes;
using dss::test_support::kPeUwopSaveXmm128;
using dss::test_support::kPeUwopSaveXmm128Far;
using dss::test_support::kX64Xmm;
using dss::test_support::peFunctionUnwinds;
using dss::test_support::peGuardedFunctions;
using dss::test_support::peRvaToOffset;
using dss::test_support::peSections;
using dss::test_support::rdU32;
using dss::test_support::readAllBytes;
using dss::test_support::x64RegisterName;

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

// `n` bytes of the image from `at`, in hex, for a failure's message.
[[nodiscard]] std::string bytesAt(std::vector<std::uint8_t> const& img, std::size_t at,
                                  std::size_t n) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string    out;
    for (std::size_t k = 0; k < n && at + k < img.size(); ++k) {
        if (k != 0) out += ' ';
        out += kHex[img[at + k] >> 4];
        out += kHex[img[at + k] & 0xFu];
    }
    return out;
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
// registers a call preserves and nothing else. So every register a handler
// reads before writing it must be one of those — the question
// `everyValueAHandlerTakes` asks of the image, on EVERY PATH from the handler's
// address to the function's return (the code after the handler, which reads
// what the handler and the code before the region left, included) and in BOTH
// register files: rax, rcx, rdx, r8..r11 and xmm0..xmm5 are the registers it
// must not find. It is asked of every handler of every one of these images, in
// both pipelines: the block a filter leaves behind in its function once led into
// the handler, and what a handler read was then live to the end of the function
// — a wrong answer to "what does the handler's entry hold" that no run on one
// host would have shown.
//
// The statement is as wide as the walk. So EVERY path of every handler must be
// decoded to its end — a form the decoder does not know fails here, with its
// bytes — and in each pipeline the handlers must be seen taking a value out of
// EACH register file: a call-preserved general register, and a call-preserved
// vector register (the handler example's `double` cases), so neither half of the
// decoder holds the statement by reading nothing. (The walk itself is held to
// what it must find by the case after this one.)
TEST(GuardedRegionExamples, NoHandlerTakesAValueACallDoesNotPreserve) {
    for (CompileConfig const config : {CompileConfig::Debug, CompileConfig::Release}) {
        std::size_t handlers = 0, takeGeneral = 0, takeVector = 0, takeFromTheFrame = 0;
        for (char const* example : kExamples) {
            SCOPED_TRACE(std::string{example} + " [" + pipelineName(config) + "]");
            Built const& b = imageOf(example, config);
            ASSERT_EQ(b.rc, 0) << b.diagnostics;
            auto const secs = peSections(b.image);
            for (PeGuardedFunction const& f : peGuardedFunctions(b.image)) {
                std::size_t const begin = peRvaToOffset(secs, f.begin);
                ASSERT_NE(begin, 0u) << "a guarded function's address lies in no section";
                std::size_t const end = begin + (f.end - f.begin);
                for (PeScopeRecord const& r : f.records) {
                    std::size_t const at = peRvaToOffset(secs, r.target);
                    ASSERT_NE(at, 0u) << "a handler's address lies in no section";
                    HandlerWalk const w = everyValueAHandlerTakes(b.image, at, begin, end);
                    ++handlers;
                    EXPECT_FALSE(w.overBudget)
                        << "the walk from the handler at " << r.target << " of the function at "
                        << f.begin << " decoded " << w.instructions << " instructions and did "
                           "not reach the end of every path";
                    if (w.undecodedAt.has_value()) {
                        ADD_FAILURE() << "the walk from the handler at " << r.target
                                      << " of the function at " << f.begin << " met a form it "
                                         "does not decode, at function offset +"
                                      << (*w.undecodedAt - begin) << ": "
                                      << bytesAt(b.image, *w.undecodedAt, 12)
                                      << " — every path past it is unread, so the statement "
                                         "below would hold over less than the handler";
                    }
                    for (HandlerRead const& v : w.volatileReads) {
                        ADD_FAILURE() << "the handler at " << r.target << " of the function at "
                                      << f.begin << " reads " << x64RegisterName(v.reg)
                                      << " before writing it, at function offset +"
                                      << (v.at - begin) << " (" << bytesAt(b.image, v.at, 8)
                                      << ") — a register which a call does not preserve, so "
                                         "the unwinder hands the handler no value in it";
                    }
                    if (w.preservedGeneral > 0u) ++takeGeneral;
                    if (w.preservedVector > 0u) ++takeVector;
                    if (w.frameLoads > 0u) ++takeFromTheFrame;
                }
            }
        }
        EXPECT_GE(takeGeneral, 1u)
            << pipelineName(config) << ": of " << handlers << " handler(s), none was seen "
               "taking a call-preserved general register (" << takeFromTheFrame
            << " load a value out of the frame) — the general half of the statement above "
               "was held by reading nothing";
        EXPECT_GE(takeVector, 1u)
            << pipelineName(config) << ": of " << handlers << " handler(s), none was seen "
               "taking a call-preserved vector register — the vector half of the statement "
               "above was held by reading nothing, though the handler example's `double` "
               "cases read one";
    }
}

// ── THE WALK FINDS WHAT IT LOOKS FOR ─────────────────────────────────────────
//
// The case above is green when the walk finds nothing, so the walk is held here
// to FINDING each thing it looks for, on bytes written by hand: a register a
// call does not preserve read AFTER a first read of one it does (the walk does
// not stop at the first read), in the vector file, on the far arm of a
// conditional jump and past an unconditional one; and to NOT finding what is no
// such read — a zero idiom, a register a call wrote, an address of the frame
// formed in a register — and to saying where it met a form it does not know.
TEST(GuardedRegionExamples, TheHandlerWalkFindsWhatItLooksFor) {
    struct Shape {
        char const*               what;
        std::vector<std::uint8_t> bytes;
        std::vector<unsigned>     volatileReads;   // the registers it must find, in order
        std::size_t               preservedGeneral, preservedVector, frameLoads;
        bool                      decodedToItsEnd;
    };
    constexpr unsigned kRcx = 1, kRdx = 2, kR8 = 8, kXmm0 = kX64Xmm + 0;
    std::vector<Shape> const shapes{
        // mov eax, r15d ; add eax, ecx ; ret
        {"past the first read", {0x44, 0x89, 0xF8, 0x01, 0xC8, 0xC3}, {kRcx}, 1, 0, 0, true},
        // cvttsd2si eax, xmm0 ; ret
        {"a vector register", {0xF2, 0x0F, 0x2C, 0xC0, 0xC3}, {kXmm0}, 0, 0, 0, true},
        // cvttsd2si eax, xmm15 ; ret
        {"a preserved vector register", {0xF2, 0x41, 0x0F, 0x2C, 0xC7, 0xC3}, {}, 0, 1, 0, true},
        // xor ecx, ecx ; mov eax, ecx ; xorps xmm0, xmm0 ; addsd xmm0, xmm0 ; ret
        {"zero idioms", {0x31, 0xC9, 0x89, 0xC8, 0x0F, 0x57, 0xC0, 0xF2, 0x0F, 0x58, 0xC0, 0xC3},
         {}, 0, 0, 0, true},
        // test r15d, r15d ; jne +6 ; mov eax, 1 ; ret ; mov eax, edx ; ret
        {"the far arm of a conditional jump",
         {0x45, 0x85, 0xFF, 0x75, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x89, 0xD0, 0xC3},
         {kRdx}, 1, 0, 0, true},
        // call +0 ; mov eax, ecx ; ret
        {"a register a call wrote", {0xE8, 0x00, 0x00, 0x00, 0x00, 0x89, 0xC8, 0xC3}, {}, 0, 0, 0,
         true},
        // lea r9, [rsp+0x10] ; movsd xmm1, [r9] ; ret
        {"an address of the frame in a register",
         {0x4C, 0x8D, 0x4C, 0x24, 0x10, 0xF2, 0x41, 0x0F, 0x10, 0x09, 0xC3}, {}, 0, 0, 1, true},
        // jmp +2 ; (cpuid, jumped over) ; mov eax, r8d ; ret
        {"past an unconditional jump", {0xEB, 0x02, 0x0F, 0xA2, 0x44, 0x89, 0xC0, 0xC3}, {kR8}, 0, 0,
         0, true},
        // cpuid ; ret
        {"a form it does not decode", {0x0F, 0xA2, 0xC3}, {}, 0, 0, 0, false},
    };
    for (Shape const& s : shapes) {
        SCOPED_TRACE(s.what);
        HandlerWalk const w = everyValueAHandlerTakes(s.bytes, 0, 0, s.bytes.size());
        std::vector<unsigned> found;
        for (HandlerRead const& v : w.volatileReads) found.push_back(v.reg);
        EXPECT_EQ(found, s.volatileReads);
        EXPECT_EQ(w.preservedGeneral, s.preservedGeneral);
        EXPECT_EQ(w.preservedVector, s.preservedVector);
        EXPECT_EQ(w.frameLoads, s.frameLoads);
        EXPECT_EQ(w.decodedToItsEnd(), s.decodedToItsEnd);
        if (!s.decodedToItsEnd) {
            ASSERT_TRUE(w.undecodedAt.has_value());
            EXPECT_EQ(*w.undecodedAt, 0u) << "where the walk met the form";
        }
    }
}
