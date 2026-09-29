// Prefixed character constants — `L'…'`, `u'…'`, `U'…'`, `u8'…'` — are integer
// constant expressions (C 6.4.4.4 + 6.6) in every position one is required, and in
// `#if` (C 6.10.1p4); and a character constant's `#if` reading is signed iff its
// ELEMENT TYPE is. P68 round 9, found in passing by lane `lm`:
// D-C-PREFIXED-CHARACTER-CONSTANT-IS-NOT-A-CONSTANT-EXPRESSION and
// D-PP-IF-NARROW-CHARACTER-CONSTANT-IGNORES-PLAIN-CHAR-SIGNEDNESS.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// ✔MEASURED at HEAD 4d9a24c4 on pe64, each REFUSED while mingw-w64 13.2.0 compiled
// and ran it: `_Static_assert(L'a' == 97, "")` and `u'a'`/`U'a'` there
// (S_StaticAssertFailed, "not an integer constant expression"), a file-scope `int
// a[L'a' == 97 ? 1 : -1]` (S_NonConstantArrayLength), `enum { E = L'a' }`
// (S_NonConstantEnumeratorValue) and `#if L'a' == 97` (P_PreprocessorDirective,
// "unexpected token"). `case L'a':` and `static int x = L'a';` already worked.
// The const-expr engine refused the prefixed forms ON PURPOSE — their element
// type is the PAIR's (`wchar_t` is `int` on x86_64 Linux, `unsigned int` on
// aarch64 Linux, `unsigned short` on Windows) and the engine cannot see the pair —
// so each tier now hands the engine the core it already owns
// (`CstEvalEnvironment::resolveWideCharCore`), and the preprocessor hands `#if`
// the pair's facts (`PpCharConstantFacts`).
//
// ★ THE `#if` RULE, ✔MEASURED 2026-09-23 on the four references and without a
// fork: in `#if` a character constant is signed iff its element type is — gcc
// 13.3.0 and clang 18.1.3 on x86_64 Linux: `L'…'` signed, the narrow form signed;
// on aarch64 Linux (their cross compilers, qemu): `L'…'` unsigned AND the NARROW
// form unsigned (plain `char` is); mingw-w64 13.2.0 and MSVC 14.51: `L'…'`
// unsigned, the narrow form signed; `u'…'`/`U'…'`/`u8'…'` unsigned everywhere.
// DSS's narrow arm used to be signed on every pair, so on aarch64 Linux `#if 'a' -
// 98 < 0` took the wrong arm (✔MEASURED at HEAD: exit 7 where the references exit
// 42). Apple clang 21.0.0 on arm64 and on x86_64 gives the x86_64-Linux readings
// (`wchar_t` is `int`, plain `char` signed): ✔MEASURED 2026-09-23 on the
// coordinator's Mac, `#if` and expression readings identical on both arches.
//
// Pins, on every real (target, format) pair — each position through the driver's
// own halves (`applyTargetFormatPair`, `analyzeForTargetFormat`, `lowerToHir`):
//   (1) every ICE position accepts every prefix with the references' value — a
//       static assertion, an array bound, enumerators, bit-field widths,
//       `_Alignas`, and an index designator (the CST→HIR tier's own fold);
//   (2) each prefix's `#if` signedness is the references', and the narrow form
//       follows plain `char`;
//   (3) the widest code unit keeps its element type's sign, in `#if` and in an ICE;
//   (4) an escape too wide for the element is REFUSED in a constant expression
//       and in `#if`, as it is in a value position;
//   (5) a target that declares no `wchar_t` refuses `L'a'` in `#if` and in an ICE.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// The references' character facts per (processor, platform) — written out, never
// read from DSS's own documents: a pin that read the target to predict the target
// would agree with any mistake in it.
struct CharFacts {
    std::string_view target;
    ObjectFormatKind format;
    bool             wcharSigned;
    int              wcharBits;
    bool             charSigned;
    std::string_view source;
};
constexpr CharFacts kPlatforms[] = {
    {"x86_64", ObjectFormatKind::Elf,   true,  32, true,
     "gcc 13.3.0 + clang 18.1.3 x86_64-linux, MEASURED"},
    {"arm64",  ObjectFormatKind::Elf,   false, 32, false,
     "aarch64-linux-gnu-gcc 13.3.0 + clang 18.1.3 --target=aarch64-linux-gnu, MEASURED"},
    {"x86_64", ObjectFormatKind::Pe,    false, 16, true,
     "mingw-w64 gcc 13.2.0 + MSVC 14.51, MEASURED"},
    {"x86_64", ObjectFormatKind::MachO, true,  32, true,
     "Apple clang 21.0.0 -arch x86_64, MEASURED"},
    {"arm64",  ObjectFormatKind::MachO, true,  32, true,
     "Apple clang 21.0.0 -arch arm64, MEASURED"},
};

[[nodiscard]] CharFacts const* factsFor(std::string_view target, ObjectFormatKind f) {
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

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

struct Compiled {
    std::vector<ParseDiagnostic> diagnostics;   // every tier's
    [[nodiscard]] std::string errors() const {
        std::string out;
        for (auto const& d : diagnostics) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            out += "  " + d.actual + "\n";
        }
        return out;
    }
    [[nodiscard]] std::size_t count(DiagnosticCode c) const {
        std::size_t n = 0;
        for (auto const& d : diagnostics) n += (d.code == c) ? 1u : 0u;
        return n;
    }
    [[nodiscard]] bool mentions(DiagnosticCode c, std::string_view text) const {
        for (auto const& d : diagnostics) {
            if (d.code == c && d.actual.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

// The driver's two front halves for one pair, then the CST→HIR tier — the index
// designator folds THERE, through that tier's own resolver.
[[nodiscard]] Compiled compile(TargetSchema const& target, ObjectFormatSchema const& format,
                               std::string source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    Compiled c;
    for (auto const& d : cu->driverDiagnostics().all()) c.diagnostics.push_back(d);
    for (auto const& tree : cu->trees()) {
        for (auto const& d : tree.diagnostics().all()) c.diagnostics.push_back(d);
    }
    auto analysis = analyzeForTargetFormat(
        cu, DiagnosticBudget::libraryDefault(), target, format, nullptr);
    for (auto const& d : analysis.model.diagnostics().all()) c.diagnostics.push_back(d);
    if (analysis.model.hasErrors()) return c;
    DiagnosticReporter hirRep;
    auto const hir = lowerToHir(analysis.model, hirRep);
    for (auto const& d : hirRep.all()) c.diagnostics.push_back(d);
    return c;
}

// Run `check` over every real pair the references were measured on.
template <class Check>
void onEveryRealPair(Check const& check) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;   // not a real pair
            SCOPED_TRACE(targetName + ":" + formatName);
            CharFacts const* f = factsFor((*targetR)->name(), (*formatR)->kind());
            if (f == nullptr) {
                ADD_FAILURE() << "no reference measurement for this pair — measure the "
                                 "character facts there and add a row";
                continue;
            }
            ++pairs;
            check(**targetR, **formatR, *f);
        }
    }
    // FLOOR: ✔MEASURED at P68 round 9 — 22 real pairs (13 x86_64 + 9 arm64 formats).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

[[nodiscard]] std::string pairMacros(CharFacts const& f) {
    return std::format("#define WSIGNED {}\n#define CSIGNED {}\n#define WBITS {}\n",
                       f.wcharSigned ? 1 : 0, f.charSigned ? 1 : 0, f.wcharBits);
}

}  // namespace

// ── (1) EVERY ICE POSITION ACCEPTS EVERY PREFIX, WITH THE REFERENCES' VALUE ──
TEST(PrefixedCharConstant, EveryIcePositionAcceptsEveryPrefixOnEveryRealPair) {
    onEveryRealPair([](TargetSchema const& t, ObjectFormatSchema const& fmt,
                       CharFacts const& f) {
        std::string const src = pairMacros(f) +
            "_Static_assert(L'a' == 97 && u'a' == 97 && U'a' == 97 && u8'a' == 97, "
            "\"values\");\n"
            "int bound[L'a' == 97 && u'a' == 97 && U'a' == 97 && u8'a' == 97 ? 1 : -1];\n"
            "enum { EL = L'a', Eu = u'a', EU = U'a', E8 = u8'a' };\n"
            "_Static_assert(EL + Eu + EU + E8 == 4 * 97, \"enumerators\");\n"
            "struct Bits { unsigned f : u8'\\x05'; unsigned g : L'\\x03'; "
            "unsigned h : u'\\x02'; unsigned i : U'\\x01'; };\n"
            "_Alignas(u'\\x10') char aligned;\n"
            "int designated[4] = { [U'\\x02'] = 7, [L'\\x01'] = 5 };\n"
            "int f(int x) { switch (x) { case L'a': return 1; case u8'b': return 2; "
            "default: return 0; } }\n";
        Compiled const c = compile(t, fmt, src);
        EXPECT_EQ(c.errors(), "") << "(" << f.source << ")";
    });
}

// ── (2) EACH PREFIX'S `#if` SIGNEDNESS IS THE REFERENCES' ────────────────────
TEST(PrefixedCharConstant, IfReadsEachCharacterConstantSignedIffItsElementTypeIs) {
    onEveryRealPair([](TargetSchema const& t, ObjectFormatSchema const& fmt,
                       CharFacts const& f) {
        std::string const src = pairMacros(f) +
            "#if L'a' != 97 || u'a' != 97 || U'a' != 97 || u8'a' != 97\n"
            "#error \"values in #if\"\n#endif\n"
            "#if (L'a' - 98 < 0) != WSIGNED\n#error \"L'' reads as wchar_t\"\n#endif\n"
            "#if u'a' - 98 < 0\n#error \"u'' is unsigned\"\n#endif\n"
            "#if U'a' - 98 < 0\n#error \"U'' is unsigned\"\n#endif\n"
            "#if u8'a' - 98 < 0\n#error \"u8'' is unsigned\"\n#endif\n"
            "#if ('a' - 98 < 0) != CSIGNED\n#error \"the narrow form follows plain char\"\n"
            "#endif\n"
            "int probe_done;\n";
        Compiled const c = compile(t, fmt, src);
        EXPECT_EQ(c.errors(), "") << "(" << f.source << ")";
    });
}

// ── (3) THE WIDEST UNIT KEEPS ITS ELEMENT TYPE'S SIGN, IN `#if` AND IN AN ICE ─
TEST(PrefixedCharConstant, TheWidestUnitKeepsItsElementTypesSignInIfAndInAnIce) {
    onEveryRealPair([](TargetSchema const& t, ObjectFormatSchema const& fmt,
                       CharFacts const& f) {
        std::string const src = pairMacros(f) +
            "#if WBITS == 32\n"
            "#if (L'\\xffffffff' < 0) != WSIGNED\n#error \"L'\\\\xffffffff' in #if\"\n#endif\n"
            "_Static_assert((L'\\xffffffff' < 0) == WSIGNED, \"L'\\\\xffffffff'\");\n"
            "#else\n"
            "#if L'\\xffff' != 65535\n#error \"L'\\\\xffff' in #if\"\n#endif\n"
            "_Static_assert(L'\\xffff' == 65535, \"L'\\\\xffff'\");\n"
            "#endif\n"
            "_Static_assert(u'\\xffff' == 65535 && U'\\xffffffff' == 4294967295u "
            "&& u8'\\x7f' == 127, \"fixed-width units\");\n"
            "#if u'\\xffff' != 65535 || U'\\xffffffff' != 4294967295\n"
            "#error \"fixed-width units in #if\"\n#endif\n"
            "int probe_done;\n";
        Compiled const c = compile(t, fmt, src);
        EXPECT_EQ(c.errors(), "") << "(" << f.source << ")";
    });
}

// ── (4) AN ESCAPE TOO WIDE FOR ITS ELEMENT IS REFUSED, AS IN A VALUE POSITION ─
TEST(PrefixedCharConstant, AnEscapeTooWideForItsElementIsRefusedInEveryPosition) {
    onEveryRealPair([](TargetSchema const& t, ObjectFormatSchema const& fmt,
                       CharFacts const& f) {
        // `u8'\x100'` is too wide for `unsigned char` on every pair; a 16-bit
        // `wchar_t` also refuses `L'\x10000'`.
        std::string const tooWide =
            f.wcharBits == 16 ? std::string{"L'\\x10000'"} : std::string{"u8'\\x100'"};
        {
            Compiled const c = compile(t, fmt,
                "_Static_assert(" + tooWide + " != 0, \"too wide\");\n");
            EXPECT_NE(c.errors(), "") << tooWide << " must be refused in an ICE";
            EXPECT_EQ(c.count(DiagnosticCode::S_StaticAssertFailed), 1u) << c.errors();
        }
        {
            Compiled const c = compile(t, fmt, "#if " + tooWide + "\n#endif\nint x;\n");
            EXPECT_TRUE(c.mentions(DiagnosticCode::P_PreprocessorDirective,
                                   "wider than one code unit"))
                << tooWide << " must be refused in #if:\n" << c.errors();
        }
        {
            Compiled const c = compile(t, fmt, "int v(void) { return " + tooWide + "; }\n");
            EXPECT_NE(c.errors(), "") << "the value position refuses it too";
        }
    });
}

// ── (5) A TARGET WITHOUT `wchar_t` REFUSES `L'a'` IN `#if` AND IN AN ICE ─────
TEST(PrefixedCharConstant, ATargetWithoutWcharTRefusesLInIfAndInAnIce) {
    ASSERT_NE(cLanguage(), nullptr);
    std::ifstream in(dss::test::configRoot() / "targets" / "arm64.target.json",
                     std::ios::binary);
    ASSERT_TRUE(in.is_open());
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto const table = text.find("\"abiTypedefs\":");
    ASSERT_NE(table, std::string::npos) << "arm64.target.json has no abiTypedefs table";
    auto const row = text.find("\"wchar_t\":", table);
    ASSERT_NE(row, std::string::npos) << "arm64's abiTypedefs has no wchar_t row";
    text.replace(row, std::string_view{"\"wchar_t\":"}.size(), "\"wchar_x_t\":");
    auto stripped = TargetSchema::loadFromText(text, "<stripped-arm64>");
    ASSERT_TRUE(stripped.has_value())
        << (stripped.error().empty() ? "" : stripped.error()[0].message);
    auto formatR = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(formatR.has_value());
    {
        Compiled const c = compile(**stripped, **formatR, "#if L'a' == 97\n#endif\nint x;\n");
        EXPECT_TRUE(c.mentions(DiagnosticCode::P_PreprocessorDirective, "no element type"))
            << "L'a' in #if must be refused where the pair has no wchar_t:\n" << c.errors();
    }
    {
        Compiled const c = compile(**stripped, **formatR,
                                   "_Static_assert(L'a' == 97, \"L\");\n");
        EXPECT_EQ(c.count(DiagnosticCode::S_StaticAssertFailed), 1u)
            << "L'a' is not a constant where it has no type:\n" << c.errors();
    }
    // The control on the same stripped target: the prefixes with a fixed type fold.
    {
        Compiled const c = compile(**stripped, **formatR,
            "#if u'a' != 97 || U'a' != 97 || u8'a' != 97\n#error\n#endif\n"
            "_Static_assert(u'a' == 97 && U'a' == 97 && u8'a' == 97, \"fixed\");\n");
        EXPECT_EQ(c.errors(), "");
    }
}
