// <math.h>'s float constants are MACROS of their own type on every real pair — P69 round 4,
// D-FFI-DESCRIPTOR-FLOAT-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// A descriptor's `floatConstants` reached the SEMANTIC tier only, so `#include <math.h>` then
// `#ifdef INFINITY` was FALSE on every pair (✔MEASURED: a probe returning 42 under the macro and 7
// otherwise exited 7 on pe64), while C 7.12p3-5 make INFINITY, NAN and HUGE_VAL macros and every
// reference defines them; math.json also typed INFINITY `f64` where 7.12p4 says `float`, and
// shipped no NAN. ✔MEASURED, the references after `#include <math.h>` alone (probes/mathinf.c,
// `_Generic` over each, exit 42): gcc 13.3.0 and clang 18.1.3 over glibc 2.39 (run
// 20261001-043814-1a4bac88), MSVC 19.51 and mingw-w64 13.2.0 over the UCRT (run
// 20261001-043838-0f23e0c9), Apple clang 21 on both Mach-O arches (run 20261001-045943-1ce3d762):
// INFINITY and NAN are `float`, HUGE_VAL is `double`, all three macros.
//
// The splice now defines each row as the C front end's own constant of the declared type
// (`spellFloatConstant`): a finite value as a hex-float literal whose suffix the language's
// float-literal typing gives that type, an infinity or a quiet NaN as the language's builtin of
// that type whose lowering is the value's. Pinned here: (1) on EVERY real pair the three names
// are macros of the references' types, usable as constant expressions; (2) the user-fallback
// shape compiled both ways; (3) `#undef` removes one exactly as it removes any macro; (4) each
// spelling form, over a scratch descriptor; (5) a type no form can spell is refused on a live
// include. The run-time half (values, bit patterns, NaN != NaN) is
// `examples/c/math_h_float_constants`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir, std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e : std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(suffix);
        if (at == std::string::npos) continue;
        out.push_back(fn.substr(0, at));
    }
    return out;
}

[[nodiscard]] std::string allErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "  ";
        out += d.actual;
        out += '\n';
    }
    return out;
}

struct PairRun {
    std::string preprocessErrors;
    std::string semanticErrors;
    bool        undeclared = false;   // an S_UndeclaredIdentifier among the semantic diagnostics
};

// Compile `source` for one pair through the driver's two halves.
[[nodiscard]] PairRun compileForPair(TargetSchema const& target, ObjectFormatSchema const& format,
                                     std::string source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    PairRun r;
    r.preprocessErrors = allErrors(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) r.preprocessErrors += allErrors(tree.diagnostics());
    auto const analysis =
        analyzeForTargetFormat(cu, DiagnosticBudget::libraryDefault(), target, format, nullptr);
    r.semanticErrors = allErrors(analysis.model.diagnostics());
    for (auto const& d : analysis.model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) r.undeclared = true;
    }
    return r;
}

// The three macros, their types, and that each is a constant expression (a static initializer).
constexpr std::string_view kSurfaceProbe =
    "#include <math.h>\n"
    "#ifndef INFINITY\n#error \"INFINITY is not a macro after <math.h>\"\n#endif\n"
    "#ifndef NAN\n#error \"NAN is not a macro after <math.h>\"\n#endif\n"
    "#ifndef HUGE_VAL\n#error \"HUGE_VAL is not a macro after <math.h>\"\n#endif\n"
    "_Static_assert(_Generic(INFINITY, float: 1, default: 0), \"INFINITY is a float (7.12p4)\");\n"
    "_Static_assert(_Generic(NAN, float: 1, default: 0), \"NAN is a float (7.12p5)\");\n"
    "_Static_assert(_Generic(HUGE_VAL, double: 1, default: 0), \"HUGE_VAL is a double (7.12p3)\");\n"
    "_Static_assert(sizeof INFINITY == sizeof(float), \"sizeof INFINITY\");\n"
    "_Static_assert(sizeof NAN == sizeof(float), \"sizeof NAN\");\n"
    "_Static_assert(sizeof HUGE_VAL == sizeof(double), \"sizeof HUGE_VAL\");\n"
    "_Static_assert(_Generic(-INFINITY, float: 1, default: 0), \"-INFINITY stays a float\");\n"
    "static const float  kInf    = INFINITY;\n"
    "static const float  kNegInf = -INFINITY;\n"
    "static const float  kNan    = NAN;\n"
    "static const double kHuge   = HUGE_VAL;\n"
    "double probe_use(void) { return (double)kInf + kNegInf + kNan + kHuge; }\n";

}  // namespace

// ── (1) THE THREE MACROS, OF THE REFERENCES' TYPES, ON EVERY REAL PAIR ─────────
TEST(MathHFloatConstants, InfinityNanAndHugeValAreMacrosOfTheirOwnTypeOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const targetNames = shippedNames("targets", ".target.json");
    auto const formatNames = shippedNames("object-formats", ".format.json");
    std::size_t pairs = 0;
    for (std::string const& targetName : targetNames) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : formatNames) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;   // not a real pair
            SCOPED_TRACE(targetName + ":" + formatName);
            ++pairs;
            PairRun const r = compileForPair(**targetR, **formatR, std::string{kSurfaceProbe});
            EXPECT_EQ(r.preprocessErrors, "");
            EXPECT_EQ(r.semanticErrors, "");
        }
    }
    // FLOOR: the real-pair enumeration of the limits.h pin (P68 round 9: 22 real pairs).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── (2) A PROGRAM'S OWN FALLBACK, COMPILED BOTH WAYS ────────────────────────
// The shape that made the missing macro worse than a missing branch: `#ifndef INFINITY` /
// `#define INFINITY (1.0/0.0)` silently redefined it as a DOUBLE. With the header it is the
// header's float; without the header the program's own double — as on every reference.
TEST(MathHFloatConstants, AProgramsOwnFallbackIsTakenOnlyWithoutTheHeader) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("x86_64");
    auto formatR = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(targetR.has_value() && formatR.has_value());
    PairRun const with = compileForPair(**targetR, **formatR,
        "#include <math.h>\n#ifndef INFINITY\n#define INFINITY (1.0/0.0)\n#endif\n"
        "_Static_assert(_Generic(INFINITY, float: 1, default: 0), \"the header's float\");\n"
        "int probe;\n");
    EXPECT_EQ(with.preprocessErrors + with.semanticErrors, "");
    PairRun const without = compileForPair(**targetR, **formatR,
        "#ifndef INFINITY\n#define INFINITY (1.0/0.0)\n#endif\n"
        "_Static_assert(_Generic(INFINITY, double: 1, default: 0), \"the program's double\");\n"
        "int probe;\n");
    EXPECT_EQ(without.preprocessErrors + without.semanticErrors, "");
}

// ── (3) `#undef` REMOVES ONE, LIKE ANY MACRO ─────────────────────────────────
// The integer constants' rule (P68 round 9, `test_limits_h_lattice`): a macro the program
// `#undef`s is gone, and every reference then refuses the use as an undeclared identifier. The
// semantic tier used to inject every float constant as a binding too, so `INFINITY` would have
// survived its own `#undef`.
TEST(MathHFloatConstants, UndefRemovesAFloatConstantLikeAnyMacro) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("arm64");
    auto formatR = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(targetR.has_value() && formatR.has_value());
    PairRun const gone = compileForPair(**targetR, **formatR,
        "#include <math.h>\n#undef INFINITY\nfloat f(void) { return INFINITY; }\n");
    EXPECT_TRUE(gone.undeclared) << "an #undef'd INFINITY must be an undeclared identifier:\n"
                                 << gone.semanticErrors;
    EXPECT_NE(gone.semanticErrors.find("INFINITY"), std::string::npos) << gone.semanticErrors;
    EXPECT_EQ(gone.preprocessErrors, "");
    PairRun const redefined = compileForPair(**targetR, **formatR,
        "#include <math.h>\n#undef NAN\n#define NAN 5\n"
        "_Static_assert(NAN == 5, \"the program's NAN\");\nint f(void) { return NAN; }\n");
    EXPECT_EQ(redefined.preprocessErrors + redefined.semanticErrors, "");
}

// ── (4) EACH SPELLING FORM, OVER A SCRATCH DESCRIPTOR ───────────────────────
// What the splice writes, read off the synth buffer: a finite value as a hex-float literal with
// the type's suffix (exact, and read back), a negative one parenthesized, an infinity and a quiet
// NaN as the language's builtin of that type (the C document's first `infinity` / `quiet_nan`
// row of the declared result type), negated for a negative one.
TEST(MathHFloatConstants, EachValueIsSpelledAsTheLanguagesOwnConstantOfItsType) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "float-splice"};
    std::filesystem::create_directories(dir.path() / "sys");
    std::ofstream(dir.path() / "sys" / "fk.json", std::ios::binary) << R"({
  "header": "fk.h",
  "floatConstants": [
    { "name": "K_INF_F",   "value": "inf",   "type": "f32" },
    { "name": "K_NINF_D",  "value": "-inf",  "type": "f64" },
    { "name": "K_NAN_F",   "value": "nan",   "type": "f32" },
    { "name": "K_NNAN_D",  "value": "-nan",  "type": "f64" },
    { "name": "K_HALF_F",  "value": "0.5",   "type": "f32" },
    { "name": "K_TENTH_F", "value": "0.1",   "type": "f32" },
    { "name": "K_NEG_D",   "value": "-2.25", "type": "f64" }
  ]
})";
    std::vector<std::filesystem::path> const sys{dir.path() / "sys"};
    auto buf = SourceBuffer::fromString("#include <fk.h>\nint probe;\n", "main.c");
    PreprocessResult const r =
        preprocess(buf, cLanguage(), {}, kDefaultHeaderNameMatching,
                   DiagnosticBudget::libraryDefault(), sys, ObjectFormatKind::Elf, {}, {}, {},
                   std::nullopt, nullptr, nullptr);
    std::string const synth = r.synthBuffer ? std::string{r.synthBuffer->text()} : std::string{};
    EXPECT_FALSE(r.diagnostics->hasErrors()) << allErrors(*r.diagnostics);
    for (std::string_view const want : {
             "#define K_INF_F (__builtin_inff())\n",
             "#define K_NINF_D (-__builtin_inf())\n",
             "#define K_NAN_F (__builtin_nanf(\"\"))\n",
             "#define K_NNAN_D (-__builtin_nan(\"\"))\n",
             "#define K_HALF_F 0x1p-1f\n",
             // 0.1 narrowed to the float nearest it, 0x3dcccccd: the FLOAT's digits, not the double's.
             "#define K_TENTH_F 0x1.99999ap-4f\n",
             "#define K_NEG_D (-0x1.2p+1)\n"}) {
        EXPECT_NE(synth.find(want), std::string::npos) << "missing `" << want << "` in:\n" << synth;
    }
}

// ── (5) A TYPE NO FORM CAN SPELL IS REFUSED ON A LIVE INCLUDE ────────────────
// C has no `_Float16` literal suffix in DSS's document and no half-precision builtin, so an
// `f16` row has no spelling: the splice refuses the descriptor by name rather than splice a
// constant of another type, and a DEAD include stays silent (the integer splice's rule).
TEST(MathHFloatConstants, ATypeNoFormSpellsIsRefusedOnALiveIncludeOnly) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "float-splice-refused"};
    std::filesystem::create_directories(dir.path() / "sys");
    std::ofstream(dir.path() / "sys" / "fh.json", std::ios::binary) << R"({
  "header": "fh.h",
  "floatConstants": [ { "name": "K_HALF_H", "value": "0.5", "type": "f16" } ]
})";
    std::vector<std::filesystem::path> const sys{dir.path() / "sys"};
    auto const run = [&](std::string source) {
        auto buf = SourceBuffer::fromString(std::move(source), "main.c");
        return preprocess(buf, cLanguage(), {}, kDefaultHeaderNameMatching,
                          DiagnosticBudget::libraryDefault(), sys, ObjectFormatKind::Elf, {}, {}, {},
                          std::nullopt, nullptr, nullptr);
    };
    PreprocessResult const live = run("#include <fh.h>\nint probe;\n");
    std::string const errs = allErrors(*live.diagnostics);
    EXPECT_NE(errs.find("float constant 'K_HALF_H'"), std::string::npos) << errs;
    std::string const synth = live.synthBuffer ? std::string{live.synthBuffer->text()} : std::string{};
    EXPECT_EQ(synth.find("#define K_HALF_H"), std::string::npos) << synth;
    PreprocessResult const dead = run("#if 0\n#include <fh.h>\n#endif\nint probe;\n");
    EXPECT_FALSE(dead.diagnostics->hasErrors()) << allErrors(*dead.diagnostics);
}
