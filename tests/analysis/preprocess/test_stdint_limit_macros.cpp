// `<stdint.h>`'s limit and width macros — the whole C23 7.22.2 / 7.22.3 family —
// realized per (target, format) pair: P68 round 9, D-FFI-STDINT-LIMIT-MACROS.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// DSS's `<stdint.h>` defined TWO of the 84 names (✔MEASURED at 4d9a24c4 plus the
// round's fold 1: `INT64_MIN`/`INT64_MAX`; `INT32_MAX`, `UINT64_MAX`, `SIZE_MAX`
// and the rest were undeclared identifiers on every pair, and read as 0 in `#if`).
// Every name is now a `constants` row naming a TYPE and a LIMIT, realized from the
// pair: C 7.22.5 (by 5.2.5.3) gives each `_MIN`/`_MAX` its type's PROMOTED type and
// value, and the types are per platform — `int_fast16_t` is `long` under glibc and
// `short` on Darwin and Windows, `int64_t` is `long` on ELF and `long long`
// elsewhere, `wchar_t` is signed on x86_64 Linux and unsigned on aarch64 Linux.
//
// ★ THE PIN IS THE REFERENCES' ANSWER, COMPILED BY DSS ON EVERY REAL PAIR. The
// table below is ✔MEASURED 2026-09-24 per platform by `_Generic` type + printed
// value + four `#if` predicates per name, after `#include <stdint.h>` alone: gcc
// 13.3.0 `-std=c2x` and clang 18.1.3 `-std=c23` on x86_64-linux and on
// aarch64-linux (cross, under qemu) — all 84 defined, types and values as below;
// mingw-w64 gcc 13.2.0 `-std=c2x` and MSVC 14.51 `/std:clatest` on pe — the 51
// `_MIN`/`_MAX` defined, none of the 33 `_WIDTH` (C23 requires them: the union
// includes them).
// Where a pe reference deviates from C 7.22.5's promoted TYPE (mingw-w64 types
// `WCHAR_*`/`WINT_*` `unsigned int`; MSVC types `INT8_MAX` `char` and `UINT16_MAX`
// `unsigned short`), the other pe reference and every ELF one give the promoted
// type, and the standard decides. The Mach-O rows are Apple clang 21.0.0's own
// answers, MEASURED 2026-09-24 on the coordinator's Mac by the same probe (split
// in four for the runner): the 51 `_MIN`/`_MAX` with exactly these types and
// values, arm64 and `-arch x86_64` identical, every `#if` value equal to the C
// value, and — like both pe references — no `_WIDTH` and no version macro. Each
// real pair compiles a translation unit of `_Static_assert`s — the type through
// `_Generic`, the value through `==` — and of `#if` arms, through the driver's own
// two halves (`applyTargetFormatPair`, `analyzeForTargetFormat`), and must be
// diagnostic-free. A pair the table does not know FAILS: a new target or format is
// measured deliberately, never inherited.
//
// Also pinned, per pair: `SIG_ATOMIC_*` are defined exactly where `<signal.h>` is
// (C 7.22.3p2 — "only the macros corresponding to those typedef names it actually
// provides"; DSS ships no `<signal.h>` on pe, so they are undefined there — the
// gap is D-C-SIGNAL-H-INCOMPLETE-AND-ABSENT-ON-PE, and this pin follows its close);
// `__STDC_VERSION_STDINT_H__` is 202311L (C23 7.22p5); and each macro equals its GNU
// predefined twin (`SIZE_MAX == __SIZE_MAX__`) — the two readers of one typedef.
// The reader-level pins are `ffi/test_shipped_derived_constants`; the run-time half
// is `examples/c/stdint_limit_family`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ── THE REFERENCES' FACTS, PER (processor, platform) ─────────────────────────
// Each typedef's C type there, written out whole rather than derived from DSS's
// own documents: a pin that read the descriptor to predict the descriptor would
// agree with any mistake in it.
struct CType {
    std::string_view name;       // as `_Generic` names it
    int              width;      // bits
    bool             isUnsigned;
};
struct PlatformFacts {
    std::string_view target;     // `.target.json` name
    ObjectFormatKind format;
    std::string_view int64;      // int64_t / int_least64_t / int_fast64_t
    CType            fast16;     // int_fast16_t
    CType            fast32;     // int_fast32_t
    std::string_view intptr;     // intptr_t
    std::string_view intmax;     // intmax_t
    std::string_view ptrdiff;    // ptrdiff_t
    CType            wchar;      // wchar_t
    CType            wint;       // wint_t
    std::string_view source;     // where the numbers were read
};
constexpr PlatformFacts kPlatforms[] = {
    {"x86_64", ObjectFormatKind::Elf, "long", {"long", 64, false}, {"long", 64, false}, "long",
     "long", "long", {"int", 32, false}, {"unsigned int", 32, true},
     "gcc 13.3.0 + clang 18.1.3 x86_64-linux (glibc 2.39), MEASURED"},
    {"arm64", ObjectFormatKind::Elf, "long", {"long", 64, false}, {"long", 64, false}, "long",
     "long", "long", {"unsigned int", 32, true}, {"unsigned int", 32, true},
     "aarch64-linux-gnu-gcc 13.3.0 + clang 18.1.3 --target=aarch64-linux-gnu, MEASURED"},
    {"x86_64", ObjectFormatKind::Pe, "long long", {"short", 16, false}, {"int", 32, false},
     "long long", "long long", "long long", {"unsigned short", 16, true},
     {"unsigned short", 16, true},
     "mingw-w64 gcc 13.2.0 + MSVC 14.51 (types: C 7.22.5's promoted type), MEASURED"},
    {"x86_64", ObjectFormatKind::MachO, "long long", {"short", 16, false}, {"int", 32, false},
     "long", "long", "long", {"int", 32, false}, {"int", 32, false},
     "Apple clang 21.0.0 -arch x86_64 typedef identities, MEASURED"},
    {"arm64", ObjectFormatKind::MachO, "long long", {"short", 16, false}, {"int", 32, false},
     "long", "long", "long", {"int", 32, false}, {"int", 32, false},
     "Apple clang 21.0.0 arm64-apple-darwin typedef identities, MEASURED"},
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

// The C type an integer of this type PROMOTES to (C 6.3.1.1): every type narrower
// than `int` becomes `int` — both signednesses, since `int` holds all their values.
[[nodiscard]] std::string promoted(CType const& t) {
    return t.width < 32 ? std::string{"int"} : std::string{t.name};
}

// A limit's value, spelled as a literal whose VALUE is exact in both phase 7 and
// `#if` (the comparisons below convert, so the suffix need not name the type).
[[nodiscard]] std::string maxLiteral(CType const& t) {
    if (t.isUnsigned) {
        std::uint64_t const v =
            t.width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << t.width) - 1u);
        return std::to_string(v) + "ULL";
    }
    return std::to_string((std::uint64_t{1} << (t.width - 1)) - 1u) + "LL";
}
[[nodiscard]] std::string minLiteral(CType const& t) {
    if (t.isUnsigned) return "0";
    return "(-" + std::to_string((std::uint64_t{1} << (t.width - 1)) - 1u) + "LL - 1)";
}

// One family member's checks: defined, its type, its value, its `#if` value and its
// `#if` sign (`-1 < NAME` holds iff NAME is signed-typed and non-negative, since an
// unsigned operand drags -1 to the maximum), and — if it has one — its GNU twin.
void member(std::string& s, std::string_view name, std::string const& type,
            std::string const& value, bool signedNonNegative, std::string_view twin) {
    s += std::format("#ifndef {0}\n#error \"{0} is not defined\"\n#endif\n", name);
    s += std::format("IS({}, {})\nEQ({}, {})\n", type, name, name, value);
    s += std::format("#if ({}) != {}\n#error \"{}'s value in #if\"\n#endif\n", name, value,
                     name);
    s += std::format("#if (-1 < ({})) != {}\n#error \"{}'s signedness in #if\"\n#endif\n",
                     name, signedNonNegative ? 1 : 0, name);
    if (!twin.empty()) s += std::format("EQ({}, {})\n", name, twin);
}

// `_MIN`, `_MAX` and `_WIDTH` of one signed/unsigned typedef pair.
void family(std::string& s, std::string_view prefixS, std::string_view prefixU,
            CType const& signedT, CType const& unsignedT, std::string_view twinPrefixS,
            std::string_view twinPrefixU, bool widthTwin) {
    auto const twin = [](std::string_view prefix, std::string_view limit) {
        return prefix.empty() ? std::string{} : std::format("__{}_{}__", prefix, limit);
    };
    member(s, std::format("{}_MIN", prefixS), promoted(signedT), minLiteral(signedT), false, "");
    member(s, std::format("{}_MAX", prefixS), promoted(signedT), maxLiteral(signedT), true,
           twin(twinPrefixS, "MAX"));
    member(s, std::format("{}_MAX", prefixU), promoted(unsignedT), maxLiteral(unsignedT),
           promoted(unsignedT) == "int", twin(twinPrefixU, "MAX"));
    member(s, std::format("{}_WIDTH", prefixS), "int", std::to_string(signedT.width), true,
           widthTwin ? twin(twinPrefixS, "WIDTH") : std::string{});
    member(s, std::format("{}_WIDTH", prefixU), "int", std::to_string(unsignedT.width), true,
           widthTwin ? twin(twinPrefixU, "WIDTH") : std::string{});
}

[[nodiscard]] CType unsignedOf(CType const& t) {
    // The unsigned twin of a signed type, for the table's handful of names.
    std::string_view const n = t.name;
    std::string_view const u = n == "signed char" ? "unsigned char"
                             : n == "short"       ? "unsigned short"
                             : n == "int"         ? "unsigned int"
                             : n == "long"        ? "unsigned long"
                                                  : "unsigned long long";
    return CType{u, t.width, true};
}

// The translation unit one pair compiles.
[[nodiscard]] std::string stdintProbe(PlatformFacts const& p) {
    std::string s =
        "#include <stdint.h>\n"
        "#define IS(T, M) _Static_assert(_Generic((M), T: 1, default: 0), "
        "#M \" is not \" #T);\n"
        "#define EQ(M, V) _Static_assert((M) == (V), #M \" is not \" #V);\n";
    CType const i8{"signed char", 8, false};
    CType const i16{"short", 16, false};
    CType const i32{"int", 32, false};
    CType const i64{p.int64, 64, false};
    for (auto const& [n, t] : {std::pair{8, i8}, std::pair{16, i16}, std::pair{32, i32},
                               std::pair{64, i64}}) {
        // Exact width: GNU names only the MAX twin (`__INT8_MAX__`), and no width twin.
        family(s, std::format("INT{}", n), std::format("UINT{}", n), t, unsignedOf(t),
               std::format("INT{}", n), std::format("UINT{}", n), false);
        // Least width: the same types (C 7.22.1.2p3); GNU names MAX and WIDTH twins
        // for the signed type and the MAX twin for the unsigned one.
        family(s, std::format("INT_LEAST{}", n), std::format("UINT_LEAST{}", n), t,
               unsignedOf(t), std::format("INT_LEAST{}", n), std::format("UINT_LEAST{}", n),
               false);
        s += std::format("EQ(INT_LEAST{0}_WIDTH, __INT_LEAST{0}_WIDTH__)\n", n);
    }
    CType const fast[] = {i8, p.fast16, p.fast32, i64};
    int const fastN[] = {8, 16, 32, 64};
    for (int k = 0; k < 4; ++k) {
        family(s, std::format("INT_FAST{}", fastN[k]), std::format("UINT_FAST{}", fastN[k]),
               fast[k], unsignedOf(fast[k]), std::format("INT_FAST{}", fastN[k]),
               std::format("UINT_FAST{}", fastN[k]), false);
        s += std::format("EQ(INT_FAST{0}_WIDTH, __INT_FAST{0}_WIDTH__)\n", fastN[k]);
    }
    family(s, "INTPTR", "UINTPTR", CType{p.intptr, 64, false},
           unsignedOf(CType{p.intptr, 64, false}), "INTPTR", "UINTPTR", true);
    family(s, "INTMAX", "UINTMAX", CType{p.intmax, 64, false},
           unsignedOf(CType{p.intmax, 64, false}), "INTMAX", "UINTMAX", true);
    // C 7.22.3 — the types of OTHER headers.
    CType const ptrdiff{p.ptrdiff, 64, false};
    member(s, "PTRDIFF_MIN", promoted(ptrdiff), minLiteral(ptrdiff), false, "");
    member(s, "PTRDIFF_MAX", promoted(ptrdiff), maxLiteral(ptrdiff), true, "__PTRDIFF_MAX__");
    member(s, "PTRDIFF_WIDTH", "int", "64", true, "__PTRDIFF_WIDTH__");
    CType const size = unsignedOf(ptrdiff);
    member(s, "SIZE_MAX", promoted(size), maxLiteral(size), false, "__SIZE_MAX__");
    member(s, "SIZE_WIDTH", "int", "64", true, "__SIZE_WIDTH__");
    for (auto const& [prefix, t] : {std::pair{std::string_view{"WCHAR"}, p.wchar},
                                    std::pair{std::string_view{"WINT"}, p.wint}}) {
        // An unsigned type's `_MIN` is 0 in the PROMOTED type (C 7.22.5): an `int`
        // 0 is signed and non-negative in `#if`, an `unsigned int` 0 is not signed.
        member(s, std::format("{}_MIN", prefix), promoted(t), minLiteral(t),
               t.isUnsigned && promoted(t) == "int", std::format("__{}_MIN__", prefix));
        member(s, std::format("{}_MAX", prefix), promoted(t), maxLiteral(t),
               promoted(t) == "int" || !t.isUnsigned, std::format("__{}_MAX__", prefix));
        member(s, std::format("{}_WIDTH", prefix), "int", std::to_string(t.width), true,
               std::format("__{}_WIDTH__", prefix));
    }
    // `sig_atomic_t` is `int` on every reference; its limits exist exactly where
    // `<signal.h>` does (C 7.22.3p2).
    s += "#if __has_include(<signal.h>)\n";
    member(s, "SIG_ATOMIC_MIN", "int", minLiteral(i32), false, "__SIG_ATOMIC_MIN__");
    member(s, "SIG_ATOMIC_MAX", "int", maxLiteral(i32), true, "__SIG_ATOMIC_MAX__");
    member(s, "SIG_ATOMIC_WIDTH", "int", "32", true, "__SIG_ATOMIC_WIDTH__");
    s += "#else\n"
         "#if defined(SIG_ATOMIC_MIN) || defined(SIG_ATOMIC_MAX) || defined(SIG_ATOMIC_WIDTH)\n"
         "#error \"SIG_ATOMIC_* defined where sig_atomic_t is not (C 7.22.3p2)\"\n"
         "#endif\n"
         "#endif\n";
    // C23 7.22p5.
    s += "#if __STDC_VERSION_STDINT_H__ != 202311L\n#error \"__STDC_VERSION_STDINT_H__\"\n"
         "#endif\nIS(long, __STDC_VERSION_STDINT_H__)\n";
    s += "int dss_stdint_probe;\n";
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

// ── EVERY NAME'S TYPE AND VALUE, ON EVERY REAL PAIR, AS THE REFERENCES ───────
TEST(StdintLimitMacros, EveryNameHasThePlatformsReferenceAnswerOnEveryRealPair) {
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
            PairRun const r =
                compileForPair(cLanguage(), target, format, stdintProbe(*facts));
            EXPECT_EQ(r.driverErrors, "");
            EXPECT_EQ(r.preprocessErrors, "") << "(" << facts->source << ")";
            EXPECT_EQ(r.semanticErrors, "") << "(" << facts->source << ")";
        }
    }
    // FLOOR: ✔MEASURED at P68 round 9 — 22 real pairs (13 x86_64 + 9 arm64 formats).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── ONE NAME, BROKEN ON PURPOSE, IS SEEN ─────────────────────────────────────
// The control for the pin above: the same probe with one expected value moved must
// FAIL — so a green run is the table agreeing, not the checks being inert.
TEST(StdintLimitMacros, AMovedExpectationIsRefused) {
    ASSERT_NE(cLanguage(), nullptr);
    auto targetR = TargetSchema::loadShipped("x86_64");
    auto formatR = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(targetR.has_value() && formatR.has_value());
    PlatformFacts wrong = *factsFor("x86_64", ObjectFormatKind::Elf);
    wrong.fast16 = CType{"short", 16, false};   // glibc says long: this must fail
    PairRun const r = compileForPair(cLanguage(), **targetR, **formatR, stdintProbe(wrong));
    EXPECT_NE(r.preprocessErrors + r.semanticErrors, "")
        << "a wrong expectation compiled clean: the probe checks nothing";
}
