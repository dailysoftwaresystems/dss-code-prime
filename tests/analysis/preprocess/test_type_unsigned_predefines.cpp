// The `type-unsigned` predefined-macro kind — `__CHAR_UNSIGNED__`,
// `__WCHAR_UNSIGNED__`, `__WINT_UNSIGNED__` — P68 round 9, the preprocessor face
// of D-C-WCHAR-T-IS-SIGNED-ON-ARM64-LINUX.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// The row fixed `wchar_t`'s TYPE on aarch64 Linux (`unsigned int`), but a
// program asks the same question in `#ifdef __WCHAR_UNSIGNED__` too, and DSS
// defined no such macro anywhere (✔MEASURED, `--dump-predefined-macros` on
// arm64:elf64-aarch64-linux-exec at HEAD 4d9a24c4). And `__CHAR_UNSIGNED__` was a
// hand-gated `constant` row in arm64.target.json — a second notation of
// `charIsUnsigned`. All three are now LANGUAGE rows of kind `type-unsigned`: each
// names a TYPE, and the merge defines it (as 1) exactly where that type is an
// unsigned integer type on the build pair — `char` by the target's
// `charIsUnsigned`, `wchar_t`/`wint_t` by its `abiTypedefs`.
//
// ★ THE PIN IS THE REFERENCES' `-dM`, PER PAIR. ✔MEASURED 2026-09-23, `-dM -E -x c`
// on an empty unit:
//   clang 18.1.3  __CHAR_UNSIGNED__   aarch64-linux-gnu
//                 __WCHAR_UNSIGNED__  aarch64-linux-gnu, x86_64-pc-windows-msvc,
//                                     x86_64-w64-mingw32
//                 __WINT_UNSIGNED__   x86_64-linux-gnu, aarch64-linux-gnu, both
//                                     Windows triples
//                 none of the three   arm64-apple-darwin, x86_64-apple-darwin
//   gcc 13.3.0    __CHAR_UNSIGNED__ on aarch64 only; the wide pair only in C++
//                 (the aarch64 g++ control defines __WCHAR_UNSIGNED__)
//   mingw-w64 gcc 13.2.0 and MSVC 14.51: none of the three in C.
// DECIDED (row 2): clang's rule — defined exactly where the type is unsigned —
// on every pair; it is inside the union everywhere and never untruthful.
//
// Pins: (1) the family is three `type-unsigned` rows of the LANGUAGE and no
// target or format declares any of them (one owner); (2) on every real pair each
// macro is defined exactly where the table says, equals 1, and agrees with the
// type it names (`(char)-1`, `(wchar_t)-1`); (3) the merge realizes them from the
// pair's facts alone — flip `charIsUnsigned`, drop the ABI typedef, drop the
// pair; (4) the loader refuses every malformed row.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

constexpr std::array<std::string_view, 3> kFamily{
    "__CHAR_UNSIGNED__", "__WCHAR_UNSIGNED__", "__WINT_UNSIGNED__"};

// The references' answer per (processor, platform): is each macro defined?
struct Expected {
    std::string_view target;
    ObjectFormatKind format;
    bool             charUnsigned;
    bool             wcharUnsigned;
    bool             wintUnsigned;
};
constexpr Expected kExpected[] = {
    {"x86_64", ObjectFormatKind::Elf,   false, false, true },
    {"arm64",  ObjectFormatKind::Elf,   true,  true,  true },
    {"x86_64", ObjectFormatKind::Pe,    false, true,  true },
    {"x86_64", ObjectFormatKind::MachO, false, false, false},
    {"arm64",  ObjectFormatKind::MachO, false, false, false},
};

[[nodiscard]] Expected const* expectedFor(std::string_view target, ObjectFormatKind f) {
    for (auto const& e : kExpected) {
        if (e.target == target && e.format == f) return &e;
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

[[nodiscard]] std::string errorsOf(TargetSchema const& target, ObjectFormatSchema const& format,
                                   std::string source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    std::string out;
    auto const collect = [&](DiagnosticReporter const& rep) {
        for (auto const& d : rep.all()) {
            if (d.severity == DiagnosticSeverity::Error) out += "  " + d.actual + "\n";
        }
    };
    collect(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) collect(tree.diagnostics());
    auto const analysis = analyzeForTargetFormat(cu, DiagnosticBudget::libraryDefault(),
                                                 target, format, nullptr);
    collect(analysis.model.diagnostics());
    return out;
}

// The probe for one pair: each macro defined iff expected, and == 1 where it is;
// and each macro agrees with the type it names.
[[nodiscard]] std::string probeFor(Expected const& e) {
    std::string s = "#include <stddef.h>\n";
    auto const pin = [&](std::string_view name, bool defined) {
        if (defined) {
            s += std::format("#ifndef {0}\n#error \"{0} must be defined here\"\n#endif\n"
                             "#if {0} != 1\n#error \"{0} must be 1\"\n#endif\n", name);
        } else {
            s += std::format("#ifdef {0}\n#error \"{0} must not be defined here\"\n#endif\n",
                             name);
        }
    };
    pin(kFamily[0], e.charUnsigned);
    pin(kFamily[1], e.wcharUnsigned);
    pin(kFamily[2], e.wintUnsigned);
    s += "#ifdef __CHAR_UNSIGNED__\n"
         "_Static_assert((char)-1 > 0, \"__CHAR_UNSIGNED__ but char is signed\");\n"
         "#else\n"
         "_Static_assert((char)-1 < 0, \"no __CHAR_UNSIGNED__ but char is unsigned\");\n"
         "#endif\n"
         "#ifdef __WCHAR_UNSIGNED__\n"
         "_Static_assert((wchar_t)-1 > 0, \"__WCHAR_UNSIGNED__ but wchar_t is signed\");\n"
         "#else\n"
         "_Static_assert((wchar_t)-1 < 0, \"no __WCHAR_UNSIGNED__ but wchar_t is unsigned\");\n"
         "#endif\n"
         "int dss_type_unsigned_probe;\n";
    return s;
}

}  // namespace

// ── (1) ONE OWNER: THE LANGUAGE'S `type-unsigned` ROWS ──────────────────────
TEST(TypeUnsignedPredefines, TheFamilyIsThreeLanguageRowsAndNoTargetOrFormatDeclaresOne) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const& rows = cLanguage()->preprocess().predefinedMacros;
    for (std::string_view const name : kFamily) {
        auto const it = std::ranges::find(rows, name, &PredefinedMacroDef::name);
        ASSERT_NE(it, rows.end()) << name << " is not a row of the C language";
        EXPECT_EQ(it->kind, PredefinedMacroKind::TypeUnsigned) << name;
        EXPECT_TRUE(it->value.empty()) << name << ": a type-unsigned row states no value";
        EXPECT_NE(it->sizedType.source, PredefinedTypeSource::None) << name;
        EXPECT_TRUE(it->availableObjectFormats.empty())
            << name << ": the TYPE decides where it is defined, never a format list";
    }
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto t = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(t.has_value()) << targetName;
        for (auto const& pm : (*t)->predefinedMacros()) {
            EXPECT_EQ(std::ranges::find(kFamily, std::string_view{pm.name}), kFamily.end())
                << targetName << " declares " << pm.name << " — a second owner";
        }
    }
    for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
        auto f = ObjectFormatSchema::loadShipped(formatName);
        ASSERT_TRUE(f.has_value()) << formatName;
        for (auto const& pm : (*f)->predefinedMacros()) {
            EXPECT_EQ(std::ranges::find(kFamily, std::string_view{pm.name}), kFamily.end())
                << formatName << " declares " << pm.name << " — a second owner";
        }
    }
}

// ── (2) EVERY REAL PAIR: THE REFERENCES' `-dM`, AND THE TYPE'S OWN ANSWER ───
TEST(TypeUnsignedPredefines, EachIsDefinedExactlyWhereTheReferencesDefineItOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;
            SCOPED_TRACE(targetName + ":" + formatName);
            Expected const* e = expectedFor((*targetR)->name(), (*formatR)->kind());
            if (e == nullptr) {
                ADD_FAILURE() << "no reference measurement for this pair — measure it";
                continue;
            }
            ++pairs;
            EXPECT_EQ(errorsOf(**targetR, **formatR, probeFor(*e)), "");
        }
    }
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── (3) THE MERGE READS THE PAIR'S FACTS AND NOTHING ELSE ───────────────────
TEST(TypeUnsignedPredefines, TheMergeRealizesEachFromThePairsFactsAlone) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const& rows = cLanguage()->preprocess().predefinedMacros;
    auto const definedUnder = [&](PredefinedTypeFacts const* facts) {
        auto const m = mergePredefinedMacros(rows, {}, {}, ObjectFormatKind::Elf, {}, facts);
        EXPECT_TRUE(m.conflicts.empty());
        std::vector<std::string> out;
        for (auto const& pm : m.effective) {
            if (std::ranges::find(kFamily, std::string_view{pm.name}) == kFamily.end()) continue;
            EXPECT_EQ(pm.value, "1") << pm.name;
            out.push_back(pm.name);
        }
        std::ranges::sort(out);
        return out;
    };
    PredefinedTypeFacts facts;
    facts.dataModel = DataModel::Lp64;
    facts.abiTypedefs = {{"wchar_t", TypeKind::U32}, {"wint_t", TypeKind::I32}};
    facts.charIsUnsigned = true;
    EXPECT_EQ(definedUnder(&facts),
              (std::vector<std::string>{"__CHAR_UNSIGNED__", "__WCHAR_UNSIGNED__"}));
    // Flip ONE fact at a time: each macro follows its own type and no other.
    facts.charIsUnsigned = false;
    EXPECT_EQ(definedUnder(&facts), (std::vector<std::string>{"__WCHAR_UNSIGNED__"}));
    facts.abiTypedefs = {{"wchar_t", TypeKind::I32}, {"wint_t", TypeKind::U16}};
    EXPECT_EQ(definedUnder(&facts), (std::vector<std::string>{"__WINT_UNSIGNED__"}));
    // A pair that declares no such typedef has no such type: never a guess.
    facts.abiTypedefs = {};
    EXPECT_EQ(definedUnder(&facts), (std::vector<std::string>{}));
    // No pair at all (the LSP / direct-API callers): none of them.
    EXPECT_EQ(definedUnder(nullptr), (std::vector<std::string>{}));
}

// ── (4) THE LOADER ──────────────────────────────────────────────────────────
namespace {

[[nodiscard]] std::string shippedCText() {
    std::ifstream in(dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string shippedCEdited(std::string_view anchor, std::string_view from,
                                         std::string_view to) {
    std::string text = shippedCText();
    auto const at = text.find(anchor);
    if (at == std::string::npos) {
        ADD_FAILURE() << "the shipped c config no longer carries " << anchor;
        return {};
    }
    auto const pos = text.find(from, at);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "no '" << from << "' after " << anchor;
        return {};
    }
    text.replace(pos, from.size(), to);
    return text;
}

[[nodiscard]] std::vector<ConfigDiagnostic> refusedNaming(std::string const& text,
                                                          std::string_view needle) {
    if (text.empty()) return {};
    auto r = GrammarSchema::loadFromText(text, "<edited-c>");
    if (r.has_value()) {
        ADD_FAILURE() << "the edited document LOADED — it had to be refused";
        return {};
    }
    bool named = false;
    for (auto const& d : r.error()) {
        if (d.message.find(needle) != std::string::npos
            || d.path.find(needle) != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "no diagnostic names '" << needle << "'; first: "
                       << (r.error().empty() ? "<none>" : r.error()[0].message);
    return r.error();
}

constexpr std::string_view kWcharRow = "\"__WCHAR_UNSIGNED__\"";
constexpr std::string_view kWcharType = "{ \"abiTypedef\": \"wchar_t\" }";
constexpr std::string_view kCharRow = "\"__CHAR_UNSIGNED__\"";

}  // namespace

TEST(TypeUnsignedPredefinesLoader, TheShippedDocumentLoads) {
    auto r = GrammarSchema::loadFromText(shippedCText(), "<shipped-c>");
    ASSERT_TRUE(r.has_value()) << (r.error().empty() ? "<none>" : r.error()[0].message);
}

// A pointer has a size and no signedness: the row would be defined nowhere.
TEST(TypeUnsignedPredefinesLoader, APointerTypeIsRefused) {
    (void)refusedNaming(shippedCEdited(kWcharRow, kWcharType, "{ \"pointerTo\": \"void\" }"),
                        "has none");
}

// A type with no signedness (a float) is refused at LANGUAGE load, by name.
TEST(TypeUnsignedPredefinesLoader, ANonIntegerTypeIsRefused) {
    (void)refusedNaming(shippedCEdited(kCharRow, "\"type\": \"char\"", "\"type\": \"float\""),
                        "not an integer type");
}

// The row states a TYPE; a `value` beside it would be a second statement.
TEST(TypeUnsignedPredefinesLoader, AValueBesideTheTypeIsRefused) {
    (void)refusedNaming(shippedCEdited(kCharRow, "\"type\": \"char\"",
                                       "\"type\": \"char\", \"value\": \"1\""),
                        "Remove 'value'");
}

TEST(TypeUnsignedPredefinesLoader, AMissingTypeIsRefused) {
    auto const ds = refusedNaming(shippedCEdited(kCharRow, "\"type\": \"char\", ", ""),
                                  "requires 'type'");
    bool missing = false;
    for (auto const& d : ds) missing |= (d.code == DiagnosticCode::C_MissingField);
    EXPECT_TRUE(missing) << "a missing required key is C_MissingField";
}

// The kind is the LANGUAGE's: a target has no type vocabulary.
TEST(TypeUnsignedPredefinesLoader, ATargetDocumentCannotDeclareTheKind) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X_UNSIGNED__","kind":"type-unsigned",
              "type":"char","programRedefinition":"ordinary",
              "impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value()) << "a target document must not declare a 'type-unsigned' row";
    bool named = false;
    for (auto const& d : r.error()) {
        named |= d.message.find("LANGUAGE-family") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the refusal must say the kind is the language's";
}
