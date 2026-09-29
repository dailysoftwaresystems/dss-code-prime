// `<limits.h>` realized per (target, format) pair from the lattice, and the typed
// shipped-constant splice it rests on — P68 round 9,
// D-C-LIMITS-H-DEFINES-NINE-OF-THE-STANDARD-MACROS.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// DSS's `<limits.h>` defined NINE of the 32 names gcc, clang and mingw define
// (✔MEASURED at HEAD 4d9a24c4: `LONG_MAX` was an undeclared identifier on every
// pair). The names cannot be FIXED rows: C 5.2.5.3.2 gives each `_MIN`/`_MAX` its
// type's PROMOTED type and value, `long` is 32 bits on pe and 64 on ELF/Mach-O,
// and plain `char` is unsigned on aarch64 Linux alone. So each row now names a
// type and a limit and is realized from the pair; and the splice that turns a
// shipped constant into the `#define` every use expands to now spells the literal
// whose PHASE-7 TYPE is the constant's own (it used to match signedness alone,
// which left `<stdint.h>`'s `INT64_MAX` a `long` on Mach-O, where `int64_t` is
// `long long`).
//
// ★ THE PIN IS THE REFERENCES' ANSWER, COMPILED BY DSS ON EVERY REAL PAIR. The
// table below is ✔MEASURED 2026-09-23 per target by `_Generic` type + printed
// value: gcc 13.3.0 `-std=c2x` and clang 18.1.3 `-std=c23` on x86_64-linux and on
// aarch64-linux (cross, under qemu); mingw-w64 gcc 13.2.0 `-std=c2x` and MSVC
// 14.51 `/std:clatest` on pe; Apple clang 21.0.0 `-std=c23` on arm64-apple-darwin
// and under `-arch x86_64` (plain `char` signed on both, `MB_LEN_MAX` 6). Each real
// pair compiles a translation unit of `_Static_assert`s — the type through
// `_Generic`, the value through `==` — and of `#if` arms, through the driver's
// own two halves (`applyTargetFormatPair`, `analyzeForTargetFormat`), and must be
// diagnostic-free. A pair the table does not know FAILS: a new target or format
// is measured deliberately, never inherited.
//
// Also pinned here: `<stdint.h>`'s `INT64_MAX`/`INT64_MIN` are `int64_t` and the
// `*_C` macros their `*_least_t`/`*max_t` on every pair (Mach-O included);
// `NULL` is `void *`; `clock_t` has its platform identity; and `#undef` removes
// a shipped constant exactly as it removes any macro (the coordinator's three
// cells). The reader-level pins are `ffi/test_shipped_derived_constants`; the
// run-time half is `examples/c/limits_h_lattice`.

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
#include "tokenizer/tokenizer.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// ── THE REFERENCES' FACTS, PER (processor, platform) ─────────────────────────
// Everything the 32 names depend on. Written out whole rather than derived from
// DSS's own documents: a pin that read the target to predict the target would
// agree with any mistake in it.
struct PlatformFacts {
    std::string_view target;      // `.target.json` name
    ObjectFormatKind format;
    int              longBits;    // LONG_* / ULONG_* / *LONG_WIDTH
    bool             charSigned;  // CHAR_MIN / CHAR_MAX
    int              mbLenMax;    // the C runtime's MB_LEN_MAX
    std::string_view clockT;      // clock_t's identity
    std::string_view source;      // where the numbers were read
};
constexpr PlatformFacts kPlatforms[] = {
    {"x86_64", ObjectFormatKind::Elf,   64, true,  16, "long",
     "gcc 13.3.0 + clang 18.1.3 x86_64-linux, MEASURED"},
    {"arm64",  ObjectFormatKind::Elf,   64, false, 16, "long",
     "aarch64-linux-gnu-gcc 13.3.0 + clang 18.1.3 --target=aarch64-linux-gnu, MEASURED"},
    {"x86_64", ObjectFormatKind::Pe,    32, true,  5,  "long",
     "mingw-w64 gcc 13.2.0 + MSVC 14.51, MEASURED"},
    {"x86_64", ObjectFormatKind::MachO, 64, true,  6,  "unsigned long",
     "Apple clang 21.0.0 -arch x86_64, MEASURED"},
    {"arm64",  ObjectFormatKind::MachO, 64, true,  6,  "unsigned long",
     "Apple clang 21.0.0 arm64-apple-darwin25.6.0, MEASURED"},
};

[[nodiscard]] PlatformFacts const* factsFor(std::string_view target, ObjectFormatKind f) {
    for (auto const& p : kPlatforms) {
        if (p.target == target && p.format == f) return &p;
    }
    return nullptr;
}

[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir,
                                                    std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e :
         std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
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

// The translation unit one pair compiles: every `<limits.h>` name's TYPE and
// VALUE as the references give them on this platform, its `#if` arms, and the
// `<stdint.h>`/`<stddef.h>`/`<time.h>` identities the same splice decides.
[[nodiscard]] std::string limitsProbe(PlatformFacts const& p) {
    std::string const longMax =
        p.longBits == 64 ? "9223372036854775807LL" : "2147483647LL";
    std::string const ulongMax =
        p.longBits == 64 ? "18446744073709551615ULL" : "4294967295ULL";
    std::string s =
        "#include <limits.h>\n#include <stdint.h>\n#include <stddef.h>\n#include <time.h>\n"
        "#define IS(T, M) _Static_assert(_Generic((M), T: 1, default: 0), "
        "#M \" is not \" #T);\n"
        "#define EQ(M, V) _Static_assert((M) == (V), #M \" is not \" #V);\n";
    auto const both = [&](std::string_view type, std::string_view name,
                          std::string const& value) {
        s += std::format("#ifndef {0}\n#error \"{0} is not defined\"\n#endif\n", name);
        s += std::format("IS({}, {})\nEQ({}, {})\n", type, name, name, value);
    };
    both("int", "CHAR_BIT", "8");
    both("int", "SCHAR_MIN", "-128");
    both("int", "SCHAR_MAX", "127");
    both("int", "UCHAR_MAX", "255");
    both("int", "CHAR_MIN", p.charSigned ? "-128" : "0");
    both("int", "CHAR_MAX", p.charSigned ? "127" : "255");
    both("int", "MB_LEN_MAX", std::to_string(p.mbLenMax));
    both("int", "SHRT_MIN", "-32768");
    both("int", "SHRT_MAX", "32767");
    both("int", "USHRT_MAX", "65535");
    both("int", "INT_MIN", "(-2147483647 - 1)");
    both("int", "INT_MAX", "2147483647");
    both("unsigned int", "UINT_MAX", "4294967295U");
    both("long", "LONG_MIN", "(-" + longMax + " - 1)");
    both("long", "LONG_MAX", longMax);
    both("unsigned long", "ULONG_MAX", ulongMax);
    both("long long", "LLONG_MIN", "(-9223372036854775807LL - 1)");
    both("long long", "LLONG_MAX", "9223372036854775807LL");
    both("unsigned long long", "ULLONG_MAX", "18446744073709551615ULL");
    both("int", "BOOL_MAX", "1");
    // C23 5.2.5.3.2 fn 15: "exact" — gcc and mingw say 1; clang 18.1.3 says 8
    // (a meaning fork, decided by the standard, ruled 2026-09-23).
    both("int", "BOOL_WIDTH", "1");
    both("int", "CHAR_WIDTH", "8");
    both("int", "SCHAR_WIDTH", "8");
    both("int", "UCHAR_WIDTH", "8");
    both("int", "SHRT_WIDTH", "16");
    both("int", "USHRT_WIDTH", "16");
    both("int", "INT_WIDTH", "32");
    both("int", "UINT_WIDTH", "32");
    both("int", "LONG_WIDTH", std::to_string(p.longBits));
    both("int", "ULONG_WIDTH", std::to_string(p.longBits));
    both("int", "LLONG_WIDTH", "64");
    both("int", "ULLONG_WIDTH", "64");
    // BITINT_MAXWIDTH is DSS's own model bound (C23: at least ULLONG_WIDTH); its
    // one owner is the `model-limit` predefine, which the header names.
    s += "IS(int, BITINT_MAXWIDTH)\nEQ(BITINT_MAXWIDTH, __BITINT_MAXWIDTH__)\n"
         "_Static_assert(BITINT_MAXWIDTH >= ULLONG_WIDTH, \"C23 floor\");\n";
    both("long", "__STDC_VERSION_LIMITS_H__", "202311L");
    // The #if (phase-4) arms, each as both references take it on this platform.
    s += std::format("#if (LONG_MAX > 2147483647) != {}\n#error \"LONG_MAX arm\"\n#endif\n",
                     p.longBits == 64 ? 1 : 0);
    s += std::format("#if (CHAR_MIN < 0) != {}\n#error \"CHAR_MIN arm\"\n#endif\n",
                     p.charSigned ? 1 : 0);
    // The EXACT values in `#if`, spelled the way a program spells them — lane `cs`'s
    // line (P68 round 9): `#if ULONG_MAX == 0xFFFFFFFFUL` was false on pe64 for DSS
    // alone before fold 1 — HEAD 4d9a24c4's `<limits.h>` did not define ULONG_MAX, so
    // `#if` read it as 0. The line now agrees with `sizeof(long)` on every pair
    // (✔MEASURED: 42 on pe64, ELF x86_64 and ELF aarch64, debug and release, as
    // under mingw-w64 13.2.0).
    s += std::format("#if ULONG_MAX != {}\n#error \"ULONG_MAX's value in #if\"\n#endif\n",
                     p.longBits == 64 ? "0xFFFFFFFFFFFFFFFFUL" : "0xFFFFFFFFUL");
    s += std::format("#if LONG_MAX != {} || LONG_MIN != -LONG_MAX - 1\n"
                     "#error \"LONG_MAX's value in #if\"\n#endif\n",
                     p.longBits == 64 ? "0x7FFFFFFFFFFFFFFFL" : "0x7FFFFFFFL");
    s += "#if ULLONG_MAX != 0xFFFFFFFFFFFFFFFFULL || LLONG_MAX != 0x7FFFFFFFFFFFFFFFLL\n"
         "#error \"long long's values in #if\"\n#endif\n";
    s += "#if -1 < UINT_MAX\n#error \"UINT_MAX is unsigned in #if\"\n#endif\n"
         "#if -1 < ULONG_MAX\n#error \"ULONG_MAX is unsigned in #if\"\n#endif\n"
         "#if -1 < ULLONG_MAX\n#error \"ULLONG_MAX is unsigned in #if\"\n#endif\n"
         "#if !(-1 < USHRT_MAX)\n#error \"USHRT_MAX is an int in #if\"\n#endif\n"
         "#if !(-1 < UCHAR_MAX)\n#error \"UCHAR_MAX is an int in #if\"\n#endif\n";
    // <stdint.h>: the limit and constant macros carry their typedef's identity.
    s += "IS(int64_t, INT64_MAX)\nIS(int64_t, INT64_MIN)\n"
         "EQ(INT64_MAX, 9223372036854775807LL)\n"
         "EQ(INT64_MIN, (-9223372036854775807LL - 1))\n"
         "IS(int_least64_t, INT64_C(1))\nIS(uint_least64_t, UINT64_C(1))\n"
         "IS(intmax_t, INTMAX_C(1))\nIS(uintmax_t, UINTMAX_C(1))\n"
         "IS(unsigned int, UINT32_C(1))\n";
    // <stddef.h>/<time.h>: NULL is a pointer; clock_t is the platform's; and
    // CLOCKS_PER_SEC has clock_t's type — C23 7.29.1p2 says so, and glibc, Darwin
    // and MSVC conform (mingw-w64's bare `1000` is an int; the standard decides).
    s += "IS(void *, NULL)\nEQ(sizeof(NULL), sizeof(void *))\n";
    s += std::format("IS({}, (clock_t)0)\n", p.clockT);
    s += std::format("IS({0}, CLOCKS_PER_SEC)\nIS(clock_t, CLOCKS_PER_SEC)\n", p.clockT);
    s += "int dss_limits_probe;\n";
    return s;
}

struct PairRun {
    std::string preprocessErrors;
    std::string semanticErrors;
    std::string driverErrors;
};

// Compile `source` for one pair through the driver's two halves.
[[nodiscard]] PairRun compileForPair(std::shared_ptr<GrammarSchema const> const& grammar,
                                     TargetSchema const& target,
                                     ObjectFormatSchema const& format,
                                     std::string source) {
    UnitBuilder builder{grammar, DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *grammar);
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    PairRun r;
    r.driverErrors = allErrors(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) r.preprocessErrors += allErrors(tree.diagnostics());
    auto const analysis = analyzeForTargetFormat(
        cu, DiagnosticBudget::libraryDefault(), target, format, nullptr);
    r.semanticErrors = allErrors(analysis.model.diagnostics());
    return r;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

}  // namespace

// ── (1) EVERY NAME'S TYPE AND VALUE, ON EVERY REAL PAIR, AS THE REFERENCES ───
TEST(LimitsHLattice, EveryNameHasTheReferencesTypeAndValueOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const targetNames = shippedNames("targets", ".target.json");
    ASSERT_GE(targetNames.size(), 2u);
    auto const formatNames = shippedNames("object-formats", ".format.json");
    ASSERT_GE(formatNames.size(), 24u);
    std::size_t pairs = 0;
    for (std::string const& targetName : targetNames) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        TargetSchema const& target = **targetR;
        for (std::string const& formatName : formatNames) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            ObjectFormatSchema const& format = **formatR;
            if (format.targetArch() != target.name()) continue;   // not a real pair
            SCOPED_TRACE(targetName + ":" + formatName);
            PlatformFacts const* facts = factsFor(target.name(), format.kind());
            if (facts == nullptr) {
                ADD_FAILURE() << "no reference measurement for " << targetName << " × "
                              << objectFormatKindName(format.kind())
                              << " — measure gcc/clang/MSVC there and add a row";
                continue;
            }
            ++pairs;
            PairRun const r = compileForPair(cLanguage(), target, format, limitsProbe(*facts));
            EXPECT_EQ(r.driverErrors, "");
            EXPECT_EQ(r.preprocessErrors, "") << "(" << facts->source << ")";
            EXPECT_EQ(r.semanticErrors, "") << "(" << facts->source << ")";
        }
    }
    // FLOOR: ✔MEASURED at P68 round 9 — 22 real pairs (13 x86_64 + 9 arm64 formats).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── (2) `#undef` REMOVES A SHIPPED CONSTANT, LIKE ANY MACRO ──────────────────
// ✔MEASURED 2026-09-23: `#undef INT_MAX` + a use is refused by gcc 13.3, clang
// 18.1.3 (both arches), mingw 13.2 and MSVC 14.51 (undeclared identifier). DSS
// used to inject every preprocessor-visible constant into the semantic scope as
// well, so the use compiled and ran to the old value. The three cells the
// coordinator ruled: the refusal, a redefinition that takes effect, and the
// plain use as the green control.
TEST(LimitsHLattice, UndefRemovesAShippedConstantLikeAnyMacro) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    for (std::string_view const formatName : {"elf64-x86_64-linux-exec", "pe64-x86_64-windows-exec"}) {
        SCOPED_TRACE(std::string{formatName});
        auto formatR = ObjectFormatSchema::loadShipped(std::string{formatName});
        ASSERT_TRUE(formatR.has_value());
        // The control: a plain use is the constant.
        {
            PairRun const r = compileForPair(
                cLanguage(), **targetR, **formatR,
                "#include <limits.h>\n_Static_assert(INT_MAX == 2147483647, \"x\");\n"
                "int f(void) { return INT_MAX; }\n");
            EXPECT_EQ(r.preprocessErrors + r.semanticErrors, "");
        }
        // `#undef` then a use: undeclared, as every reference says.
        {
            PairRun const r = compileForPair(
                cLanguage(), **targetR, **formatR,
                "#include <limits.h>\n#undef INT_MAX\nint f(void) { return INT_MAX; }\n");
            EXPECT_NE(r.semanticErrors.find("INT_MAX"), std::string::npos)
                << "an #undef'd shipped constant must be an undeclared identifier";
            EXPECT_EQ(r.preprocessErrors, "");
        }
        // `#undef` then `#define`: the program's definition wins.
        {
            PairRun const r = compileForPair(
                cLanguage(), **targetR, **formatR,
                "#include <limits.h>\n#undef INT_MAX\n#define INT_MAX 5\n"
                "_Static_assert(INT_MAX == 5, \"the program's INT_MAX\");\n"
                "int f(void) { return INT_MAX; }\n");
            EXPECT_EQ(r.preprocessErrors + r.semanticErrors, "");
        }
    }
}

// The code the refusal above carries, read from the diagnostics rather than a
// string (the collect-all reporter names the code on every row).
TEST(LimitsHLattice, TheUndefRefusalIsUndeclaredIdentifier) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("arm64");
    auto formatR = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(targetR.has_value() && formatR.has_value());
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, **targetR, **formatR);
    builder.addInMemory("#include <limits.h>\n#undef LONG_MAX\nlong f(void) { return LONG_MAX; }\n",
                        "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    auto const analysis = analyzeForTargetFormat(cu, DiagnosticBudget::libraryDefault(),
                                                 **targetR, **formatR, nullptr);
    bool undeclared = false;
    for (auto const& d : analysis.model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) undeclared = true;
    }
    EXPECT_TRUE(undeclared);
}

// ── (3) THE NON-PREPROCESSING CONSUMER HAS NO SHIPPED LANGUAGE ───────────────
// The `#undef` fix injects a preprocessor-visible constant semantically ONLY for a
// consumer that does not preprocess — keyed on the language's own `preprocess`
// declaration. ✔MEASURED: every shipped language that declares a shipped-library
// search path preprocesses (only `c` declares one), so that arm has no shipped
// consumer today. This pin goes red the day one appears, which is the moment it
// needs its own run-time witness.
TEST(LimitsHLattice, EveryShippedDescriptorConsumerPreprocesses) {
    std::size_t consumers = 0;
    std::size_t loaded = 0;
    for (std::string const& lang : shippedNames("sources", ".lang.json")) {
        auto g = GrammarSchema::loadShipped(lang);
        if (!g.has_value()) {
            // A CORE grammar with `requires` holes (the shared `asm` core the two
            // dialects embed) is not a language on its own and reads nothing; any
            // other load failure is a real one.
            std::ifstream in(dss::test::configRoot() / "sources" / (lang + ".lang.json"),
                             std::ios::binary);
            std::string const text{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
            EXPECT_NE(text.find("\"requires\""), std::string::npos)
                << lang << " does not load and is not a core grammar with holes";
            continue;
        }
        ++loaded;
        if ((*g)->semantics().shippedLibDirs.empty()) continue;
        ++consumers;
        EXPECT_TRUE((*g)->preprocess().enabled)
            << lang << " reads shipped descriptors WITHOUT preprocessing: give the "
                       "semantic-only injection arm a run-time witness for it";
    }
    EXPECT_GE(consumers, 1u) << "no shipped language reads the shipped descriptors";
    EXPECT_GE(loaded, 5u) << "the shipped-language enumeration collapsed";
}

// ── (4) THE TYPED SPLICE, ON A HAND-WRITTEN DESCRIPTOR ───────────────────────
// A constant whose declared type no literal of the language HAS (the anonymous
// 64-bit integer: `long` and `long long` are named, so no suffix yields it) is
// REFUSED at the splice when the pair is known — the signedness-only splice used
// to spell it `9223372036854775807` and let the ladder pick a type — and is simply
// not spliced when no pair is named (the data model decides such a literal's type,
// and there is none).
namespace {
[[nodiscard]] std::filesystem::path typedSpliceSysDir(ScratchDir const& dir) {
    std::filesystem::create_directories(dir.path() / "sys");
    std::ofstream(dir.path() / "sys" / "tsplice.json", std::ios::binary) << R"({
  "header": "tsplice.h",
  "constants": [
    { "name": "T_ANON64", "value": 9223372036854775807, "type": "i64" },
    { "name": "T_LONG",   "value": 2147483647,          "type": "i32 \"long\"" },
    { "name": "T_INT",    "value": 7,                   "type": "i32" }
  ]
})";
    return dir.path() / "sys";
}
[[nodiscard]] PreprocessResult ppTyped(std::filesystem::path const& sys,
                                       PredefinedTypeFacts const* facts) {
    auto buf = SourceBuffer::fromString(
        "#include <tsplice.h>\nint a = T_INT;\n", "main.c");
    std::vector<std::filesystem::path> const sysDirs{sys};
    return preprocess(buf, cLanguage(), {}, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault(), sysDirs, std::nullopt, {}, {},
                      {}, std::nullopt, nullptr, facts);
}
[[nodiscard]] std::string synthText(PreprocessResult const& r) {
    return r.synthBuffer ? std::string{r.synthBuffer->text()} : std::string{};
}
}  // namespace

TEST(LimitsHLattice, TheSpliceRefusesADeclaredTypeNoLiteralHasWhenThePairIsKnown) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "limits-typed-splice"};
    auto const sys = typedSpliceSysDir(dir);
    PredefinedTypeFacts facts;
    facts.dataModel = DataModel::Llp64;
    PreprocessResult const r = ppTyped(sys, &facts);
    bool refused = false;
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == DiagnosticCode::P_PreprocessorIncludeError
            && d.actual.find("T_ANON64") != std::string::npos) {
            refused = true;
        }
    }
    EXPECT_TRUE(refused) << "an anonymous i64 has no literal spelling — refuse, never guess";
    std::string const text = synthText(r);
    EXPECT_NE(text.find("#define T_LONG 2147483647l"), std::string::npos)
        << "on LLP64 `long` is 32-bit and a `long` literal needs the suffix: " << text;
    EXPECT_NE(text.find("#define T_INT 7\n"), std::string::npos) << text;
}

TEST(LimitsHLattice, WithNoPairAModelDecidedConstantIsNotSpliced) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "limits-typed-splice"};
    auto const sys = typedSpliceSysDir(dir);
    PreprocessResult const r = ppTyped(sys, nullptr);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "with no pair nothing is refused: the data model is not known";
    std::string const text = synthText(r);
    EXPECT_EQ(text.find("#define T_ANON64"), std::string::npos);
    EXPECT_EQ(text.find("#define T_LONG"), std::string::npos)
        << "`long`'s literal type is the data model's to decide, and there is none";
    EXPECT_NE(text.find("#define T_INT 7\n"), std::string::npos)
        << "an `int` literal types the same under every model";
}
