// The `__SIZEOF_*__` family — every member a `type-size` row of the C language,
// realized on every real (target, format) pair to the size `sizeof` gives its
// type there — and the platform ABI typedefs `wchar_t`/`wint_t` it reads.
//
// ═══ WHY THIS FILE EXISTS (P68 round 8, D-C-SIZEOF-PREDEFINED-MACRO-FAMILY-MISSING) ═
//
// gcc and clang predefine thirteen `__SIZEOF_*__` macros on every 64-bit
// triple (✔MEASURED 2026-09-21, `-dM -E`, gcc 13.3.0, clang 18.1.3, mingw gcc
// 13.2.0, Apple clang; eight triples). DSS defined one, as 18 per-format
// constants. A program that sizes a buffer or picks a code path on
// `__SIZEOF_POINTER__` or `__SIZEOF_WCHAR_T__` took its fallback arm under DSS,
// silently — an undefined name in `#if` is 0.
//
// ★ THE MACRO NAMES A TYPE, NOT A NUMBER. Each row states WHICH type it sizes;
// the merge computes the number per pair with the layout `sizeof` uses. So the
// honest pin is not a table of numbers here — it is `_Static_assert(sizeof(T)
// == __SIZEOF_T__)` COMPILED FOR EVERY REAL PAIR, which asks DSS's own
// preprocessor and DSS's own `sizeof` the same question and requires one
// answer. The only numbers written here are the platform ABI facts the target
// documents OWN (`wchar_t`, `wint_t`), each ✔MEASURED from the references.
//
// Pins:
//   (1) the language declares exactly this family, once each, as `type-size`
//       rows naming the types the references size — and no target or format
//       document declares any `__SIZEOF_*__` name (one owner);
//       `__SIZEOF_FLOAT128__`/`__SIZEOF_FLOAT80__` are NOT declared (DSS has
//       no such type yet; a size for it would be a promise a program branches on);
//   (2) the two targets declare the platform ABI types of `wchar_t` and
//       `wint_t` per object format exactly as the references define them;
//   (3) on every (target, format) pair whose format names that target
//       (`targetArch`), a translation unit asserting `sizeof(T) ==
//       __SIZEOF_T__` for the whole family — `wchar_t` through BOTH its
//       carriers, `<stddef.h>`'s typedef and `L'a'` — compiles with no
//       diagnostic, every member is DEFINED (the `long double` member exactly
//       where the format realizes the type, and undefined elsewhere), and
//       `__SIZEOF_WINT_T__ >= __SIZEOF_WCHAR_T__` (C: `wint_t` holds every
//       `wchar_t` value and `WEOF`).
// The run-time half is `examples/c/sizeof_macro_family`.

#include "analysis/compilation_unit/compilation_unit.hpp"
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

#include <cstddef>
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

struct FamilyMember {
    std::string_view     name;
    PredefinedTypeSource source;
    std::string_view     spelled;   // what the row's `type` names
    std::string_view     cType;     // the C type `sizeof` is asked about; empty = none visible
};

// The family the references define on every 64-bit triple (✔MEASURED, see the
// header). A member added to `c.lang.json` must be added here deliberately.
constexpr FamilyMember kFamily[] = {
    {"__SIZEOF_SHORT__",       PredefinedTypeSource::Vocabulary,  "short",             "short"},
    {"__SIZEOF_INT__",         PredefinedTypeSource::Vocabulary,  "int",               "int"},
    {"__SIZEOF_LONG__",        PredefinedTypeSource::Vocabulary,  "long",              "long"},
    {"__SIZEOF_LONG_LONG__",   PredefinedTypeSource::Vocabulary,  "long long",         "long long"},
    {"__SIZEOF_FLOAT__",       PredefinedTypeSource::Vocabulary,  "float",             "float"},
    {"__SIZEOF_DOUBLE__",      PredefinedTypeSource::Vocabulary,  "double",            "double"},
    {"__SIZEOF_LONG_DOUBLE__", PredefinedTypeSource::Vocabulary,  "long double",       "long double"},
    {"__SIZEOF_POINTER__",     PredefinedTypeSource::PointerTo,   "void",              "void *"},
    {"__SIZEOF_SIZE_T__",      PredefinedTypeSource::Synthesized, "sizeof",            "size_t"},
    {"__SIZEOF_PTRDIFF_T__",   PredefinedTypeSource::Synthesized, "pointerDifference", "ptrdiff_t"},
    {"__SIZEOF_WCHAR_T__",     PredefinedTypeSource::AbiTypedef,  "wchar_t",           "wchar_t"},
    {"__SIZEOF_WINT_T__",      PredefinedTypeSource::AbiTypedef,  "wint_t",            ""},
    {"__SIZEOF_INT128__",      PredefinedTypeSource::Vocabulary,  "__int128",          "__int128"},
};
constexpr std::string_view kLongDoubleMember = "__SIZEOF_LONG_DOUBLE__";

[[nodiscard]] bool isFamilyName(std::string_view n) {
    return n.starts_with("__SIZEOF_");
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

// The translation unit (3) compiles for one pair. `realizesLongDouble` is the
// FORMAT's own `longDoubleFormat` answer, read by the caller from the document.
[[nodiscard]] std::string familyProbe(bool realizesLongDouble) {
    std::string s =
        "#include <stddef.h>\n"
        "#define DSS_SIZE_IS(T, M) _Static_assert(sizeof(T) == (M), "
        "#M \" is not sizeof(\" #T \")\");\n";
    for (auto const& m : kFamily) {
        bool const required =
            m.name != kLongDoubleMember || realizesLongDouble;
        if (!required) {
            s += std::format("#ifdef {0}\n#error \"{0} is defined where the type "
                             "is not realized\"\n#endif\n", m.name);
            continue;
        }
        s += std::format("#ifndef {0}\n#error \"{0} is not defined\"\n#endif\n",
                         m.name);
        if (!m.cType.empty()) {
            s += std::format("DSS_SIZE_IS({}, {})\n", m.cType, m.name);
        }
    }
    // `wchar_t`'s OTHER carrier: a wide character constant has type wchar_t.
    s += "DSS_SIZE_IS(L'a', __SIZEOF_WCHAR_T__)\n";
    s += "_Static_assert(__SIZEOF_WINT_T__ >= __SIZEOF_WCHAR_T__, "
         "\"wint_t must hold every wchar_t value\");\n";
    s += "int dss_family_probe;\n";
    return s;
}

}  // namespace

// ── (1) THE LANGUAGE OWNS THE FAMILY, AND EVERY MEMBER NAMES A TYPE ─────────
TEST(SizeofMacroFamily, TheLanguageDeclaresEveryMemberOnceAsATypeSizeRow) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    auto const& rows = (*grammarR)->preprocess().predefinedMacros;

    std::size_t familyRows = 0;
    for (auto const& pm : rows) {
        if (isFamilyName(pm.name)) ++familyRows;
    }
    EXPECT_EQ(familyRows, std::size(kFamily))
        << "c.lang.json declares a `__SIZEOF_*__` row this pin does not list "
           "(or lost one) — add it here deliberately";
    for (auto const& m : kFamily) {
        std::size_t seen = 0;
        for (auto const& pm : rows) {
            if (pm.name != m.name) continue;
            ++seen;
            EXPECT_EQ(pm.kind, PredefinedMacroKind::TypeSize)
                << m.name << " must be derived from its type, never a number";
            EXPECT_EQ(pm.sizedType.source, m.source) << m.name;
            EXPECT_EQ(pm.sizedType.spelled, m.spelled) << m.name;
            EXPECT_TRUE(pm.value.empty())
                << m.name << ": a type-size row carries no stated value";
            if (m.source != PredefinedTypeSource::AbiTypedef) {
                EXPECT_TRUE(pm.sizedType.resolved)
                    << m.name << ": the loader left its type unresolved";
            }
            EXPECT_TRUE(pm.availableObjectFormats.empty())
                << m.name << ": whether it is defined is the pair's question, "
                             "answered by the type";
        }
        EXPECT_EQ(seen, 1u) << m.name << " must be declared exactly once";
    }
    for (std::string_view const absent :
         {std::string_view{"__SIZEOF_FLOAT128__"},
          std::string_view{"__SIZEOF_FLOAT80__"}}) {
        for (auto const& pm : rows) {
            EXPECT_NE(pm.name, absent)
                << absent << " sizes a type DSS does not have yet "
                             "(S_UnknownType) — it must stay undefined";
        }
    }

    // ONE OWNER: no target or format document declares any member.
    for (std::string const& t : shippedNames("targets", ".target.json")) {
        auto tr = TargetSchema::loadShipped(t);
        ASSERT_TRUE(tr.has_value()) << t;
        for (auto const& pm : (*tr)->predefinedMacros()) {
            EXPECT_FALSE(isFamilyName(pm.name))
                << t << ".target.json declares " << pm.name;
        }
    }
    auto const formats = shippedNames("object-formats", ".format.json");
    EXPECT_GE(formats.size(), 24u) << "the .format.json enumeration collapsed";
    for (std::string const& f : formats) {
        auto fr = ObjectFormatSchema::loadShipped(f);
        ASSERT_TRUE(fr.has_value()) << f;
        for (auto const& pm : (*fr)->predefinedMacros()) {
            EXPECT_FALSE(isFamilyName(pm.name))
                << f << ".format.json declares " << pm.name
                << " — the language's `type-size` rows own the family";
        }
    }
}

// ── (2) THE PLATFORM ABI TYPEDEFS, AS THE REFERENCES DEFINE THEM ───────────
//
// ✔MEASURED 2026-09-21 with `-dM -E` (`__WCHAR_TYPE__`, `__WINT_TYPE__`): gcc
// 13.3.0 and clang 18.1.3 for x86_64 and aarch64 Linux, mingw gcc 13.2.0 for
// Windows, Apple clang for arm64 and x86_64 macOS. The arm64 `pe` row is the
// Windows ABI (`unsigned short` for every Windows processor); no arm64 PE
// format ships. Written out whole, not counted: two rows trading values would
// keep any count green.
TEST(SizeofMacroFamily, TheTargetsDeclareThePlatformAbiTypedefs) {
    struct Fact {
        std::string_view target;
        std::string_view typedefName;
        ObjectFormatKind format;
        TypeKind         core;
    };
    constexpr Fact kFacts[] = {
        {"x86_64", "wchar_t", ObjectFormatKind::Elf,   TypeKind::I32},
        {"x86_64", "wchar_t", ObjectFormatKind::MachO, TypeKind::I32},
        {"x86_64", "wchar_t", ObjectFormatKind::Pe,    TypeKind::U16},
        {"x86_64", "wint_t",  ObjectFormatKind::Elf,   TypeKind::U32},
        {"x86_64", "wint_t",  ObjectFormatKind::MachO, TypeKind::I32},
        {"x86_64", "wint_t",  ObjectFormatKind::Pe,    TypeKind::U16},
        {"arm64",  "wchar_t", ObjectFormatKind::Elf,   TypeKind::U32},
        {"arm64",  "wchar_t", ObjectFormatKind::MachO, TypeKind::I32},
        {"arm64",  "wchar_t", ObjectFormatKind::Pe,    TypeKind::U16},
        {"arm64",  "wint_t",  ObjectFormatKind::Elf,   TypeKind::U32},
        {"arm64",  "wint_t",  ObjectFormatKind::MachO, TypeKind::I32},
        {"arm64",  "wint_t",  ObjectFormatKind::Pe,    TypeKind::U16},
    };
    for (auto const& f : kFacts) {
        auto t = TargetSchema::loadShipped(std::string{f.target});
        ASSERT_TRUE(t.has_value()) << f.target;
        auto const core = (*t)->abiTypedefCore(f.typedefName, f.format);
        ASSERT_TRUE(core.has_value())
            << f.target << " declares no ABI typedef " << f.typedefName;
        EXPECT_EQ(*core, f.core)
            << f.target << " × " << objectFormatKindName(f.format) << ": "
            << f.typedefName << " is " << typeKindNameOrEmpty(*core)
            << ", the platform says " << typeKindNameOrEmpty(f.core);
    }
    for (char const* name : {"x86_64", "arm64"}) {
        auto t = TargetSchema::loadShipped(name);
        ASSERT_TRUE(t.has_value());
        auto const names = (*t)->abiTypedefNames();
        EXPECT_EQ(names, (std::vector<std::string_view>{"wchar_t", "wint_t"}))
            << name << ": the ABI typedef table is exactly the two the family reads";
        EXPECT_FALSE((*t)->abiTypedefCore("no_such_t", ObjectFormatKind::Elf))
            << "an undeclared typedef has NO core — never a default one";
    }
}

// ── (3) EVERY MEMBER IS WHAT `sizeof` SAYS, ON EVERY REAL PAIR ──────────────
TEST(SizeofMacroFamily, EveryMemberEqualsSizeofItsTypeOnEveryRealPair) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    std::shared_ptr<GrammarSchema const> const grammar = *grammarR;
    auto const targetNames = shippedNames("targets", ".target.json");
    ASSERT_GE(targetNames.size(), 2u) << "the target enumeration collapsed";
    auto const formatNames = shippedNames("object-formats", ".format.json");
    ASSERT_GE(formatNames.size(), 24u) << "the format enumeration collapsed";

    std::size_t pairs = 0, withLongDouble = 0;
    for (std::string const& targetName : targetNames) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        TargetSchema const& target = **targetR;
        for (std::string const& formatName : formatNames) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            ObjectFormatSchema const& format = **formatR;
            // A REAL pair: the format document names the target it serves.
            // Data on both sides, compared as data — the same check the
            // driver's `crossValidateTargetFormat` makes.
            if (format.targetArch() != target.name()) continue;
            ++pairs;
            bool const realizesLongDouble =
                format.longDoubleFormat() != LongDoubleFormat::None;
            if (realizesLongDouble) ++withLongDouble;
            SCOPED_TRACE(targetName + ":" + formatName);

            UnitBuilder builder{grammar, DiagnosticBudget::libraryDefault()};
            applySystemDirs(builder, *grammar);
            applyTargetFormatPair(builder, target, format);
            builder.addInMemory(familyProbe(realizesLongDouble), "family.c");
            auto const cu = std::make_shared<CompilationUnit const>(
                std::move(builder).finish());
            EXPECT_FALSE(cu->driverDiagnostics().hasErrors())
                << allErrors(cu->driverDiagnostics());
            for (auto const& tree : cu->trees()) {
                EXPECT_FALSE(tree.diagnostics().hasErrors())
                    << "preprocess/parse:\n" << allErrors(tree.diagnostics());
            }
            auto const analysis = analyzeForTargetFormat(
                cu, DiagnosticBudget::libraryDefault(), target, format, nullptr);
            EXPECT_FALSE(analysis.model.diagnostics().hasErrors())
                << "semantic:\n" << allErrors(analysis.model.diagnostics());
        }
    }
    // FLOORS. ✔MEASURED at P68 round 8 part 4: 22 real pairs (13 x86_64 + 9
    // arm64 formats), 18 of which realize `long double` before (b) lands.
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
    EXPECT_GE(withLongDouble, 18u);
}

// ══ (4) THE KIND'S LOAD-TIME RULES ═══════════════════════════════════════════
//
// Driven through the REAL input path: the SHIPPED c document with ONE edit, so
// every refusal is measured against the rows that actually ship. Each edit is
// located from the row's own NAME (anchor), then the first `from` after it — so
// an edit that finds nothing is LOUD (an ADD_FAILURE), never a vacuous pass
// over a document that changed shape.
namespace {

[[nodiscard]] std::string shippedCText() {
    std::ifstream in(dss::test::configRoot() / "sources" / "c.lang.json",
                     std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The shipped text with `from` → `to` at the first `from` after `anchor`, or
// empty (with an ADD_FAILURE) when either is gone.
[[nodiscard]] std::string shippedCEdited(std::string_view anchor,
                                         std::string_view from,
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

// The load's diagnostics when it FAILS as it must, naming `needle`; empty (with
// an ADD_FAILURE) when it loads.
[[nodiscard]] std::vector<ConfigDiagnostic>
refusedNaming(std::string const& text, std::string_view needle) {
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

}  // namespace

// The control: the shipped document, unedited, loads — every refusal below is
// therefore the edit's, not the document's.
TEST(SizeofMacroFamilyLoader, TheShippedDocumentLoads) {
    auto r = GrammarSchema::loadFromText(shippedCText(), "<shipped-c>");
    ASSERT_TRUE(r.has_value())
        << (r.error().empty() ? "<none>" : r.error()[0].message);
}

// A TYPE the language does not have is refused at LOAD, by name — never a row
// that silently realizes to nothing on every target.
TEST(SizeofMacroFamilyLoader, AnUnknownTypeNameIsRefusedByName) {
    (void)refusedNaming(shippedCEdited("\"__SIZEOF_SHORT__\"", "\"type\": \"short\"",
                                       "\"type\": \"shortt\""),
                        "shortt");
}

// `void` resolves, and has no size: a row sizing it would realize to nothing.
TEST(SizeofMacroFamilyLoader, ATypeWithNoSizeIsRefused) {
    (void)refusedNaming(shippedCEdited("\"__SIZEOF_SHORT__\"", "\"type\": \"short\"",
                                       "\"type\": \"void\""),
                        "no scalar size");
}

// A `type-size` row states a TYPE; a `value` beside it would be a second
// statement of the fact, and the one the merge never reads.
TEST(SizeofMacroFamilyLoader, AValueBesideTheTypeIsRefused) {
    auto const ds = refusedNaming(
        shippedCEdited("\"__SIZEOF_SHORT__\"", "\"type\": \"short\"",
                       "\"type\": \"short\", \"value\": \"2\""),
        "value");
    EXPECT_FALSE(ds.empty());
}

TEST(SizeofMacroFamilyLoader, AMissingTypeIsRefused) {
    auto const ds = refusedNaming(
        shippedCEdited("\"__SIZEOF_SHORT__\"", "\"type\": \"short\", ", ""),
        "requires 'type'");
    bool missing = false;
    for (auto const& d : ds) missing |= (d.code == DiagnosticCode::C_MissingField);
    EXPECT_TRUE(missing) << "a missing required key is C_MissingField";
}

// The object form names exactly ONE source.
TEST(SizeofMacroFamilyLoader, ATypeObjectNamingTwoSourcesIsRefused) {
    (void)refusedNaming(
        shippedCEdited("\"__SIZEOF_POINTER__\"", "{ \"pointerTo\": \"void\" }",
                       "{ \"pointerTo\": \"void\", \"abiTypedef\": \"wchar_t\" }"),
        "exactly ONE");
}

TEST(SizeofMacroFamilyLoader, AnUnknownTypeObjectKeyIsRefused) {
    (void)refusedNaming(
        shippedCEdited("\"__SIZEOF_POINTER__\"", "{ \"pointerTo\": \"void\" }",
                       "{ \"pointerTO\": \"void\" }"),
        "pointerTO");
}

// A role the engine does not synthesize is refused, and the refusal names the
// roles it does — read from the one role table.
TEST(SizeofMacroFamilyLoader, AnUnknownSynthesizedRoleIsRefused) {
    auto const ds = refusedNaming(
        shippedCEdited("\"__SIZEOF_SIZE_T__\"", "{ \"synthesized\": \"sizeof\" }",
                       "{ \"synthesized\": \"sizeoff\" }"),
        "sizeoff");
    bool listsRoles = false;
    for (auto const& d : ds) {
        listsRoles |= d.message.find("'pointerDifference'") != std::string::npos;
    }
    EXPECT_TRUE(listsRoles) << "the refusal must name the roles that exist";
}

// `type` belongs to the `type-size` kind alone.
TEST(SizeofMacroFamilyLoader, ATypeKeyOnAConstantRowIsRefused) {
    (void)refusedNaming(
        shippedCEdited("\"__STDC_HOSTED__\"", "\"value\": \"1\"",
                       "\"value\": \"1\", \"type\": \"int\""),
        "valid only on a 'type-size'");
}

// The kind is the LANGUAGE's: a target document has no type vocabulary, so a
// `type-size` row there is refused rather than realized against nothing.
TEST(SizeofMacroFamilyLoader, ATargetDocumentCannotDeclareTheKind) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__SIZEOF_X__","kind":"type-size",
              "type":"int","programRedefinition":"ordinary",
              "impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a target document must not declare a 'type-size' row";
    bool named = false;
    for (auto const& d : r.error()) {
        named |= d.message.find("LANGUAGE-family") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the refusal must say the kind is the language's";
}
