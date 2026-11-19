// core/test_type_spelling — A DIAGNOSTIC NAMES A TYPE AS ITS LANGUAGE SPELLS IT (cycle P69 f26d, lane `cs`,
// rulings 617 and 621).
//
// ONE renderer (`spellType`, core/types/type_spelling.hpp) over the type lattice, every spelling from the language
// document's `semantics.typeSpelling` table, which the grammar loader reads complete or refuses by name. This file
// pins:
//   * THE LOAD: an absent table loads (that language's diagnostics name no types) and records whether the document
//     has types; a present table is refused by name when a kind its rows reach is unspelled, when a key is unknown,
//     when a kind spelling restates a name the document's own rows do not give that kind, and when a form its
//     declarators need is missing;
//   * THE SHIPPED DOCUMENTS (ruling 621): every shipped language document whose own rows reach a core type kind
//     carries a declared table — so deleting c's block turns THIS test red, never a silence;
//   * A SECOND LANGUAGE: a toy document's own spellings, with no C word in them, through the same renderer;
//   * THE C UNIVERSE: the shipped c document's spelling of every type form, each expected text MEASURED on the
//     references (the n507c types probe), never written from memory;
//   * THE CORE-KIND FACE: a type known only by its kind and tag (`spellCoreKind`), and no spelling without a table;
//   * A KIND ONLY A DESCRIPTOR REACHES: not spelled, nor any type built on it — never the engine's kind name,
//     never a stop.

#include "core/types/grammar_schema.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_spelling.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using dss::GrammarSchema;
using dss::TypeKind;

// A toy language: two named types, no declarators. `{TS}` is replaced by the table under test.
std::string toyDocument(std::string_view typeSpelling) {
    std::string doc = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "Toy", "version": "0.1.0" },
      "tokens": {
        " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "=": [{ "kind": "Eq" }],
        ";": [{ "kind": "Semi" }]
      },
      "keywords": [ { "word": "let", "kind": "LetKw" } ],
      "shapes": {
        "root":  { "sequence": [ { "repeat": "stmt" } ] },
        "stmt":  { "alt": [ "decl", "use" ] },
        "decl":  { "sequence": [ "LetKw", "Identifier", "Eq", "use", "Semi" ] },
        "use":   { "sequence": [ "Identifier" ] },
        "block": { "sequence": [ "stmt" ] }
      },
      "semantics": {
        "declarations": [ { "rule": "decl", "name": 1, "init": 3, "kind": "variable" } ],
        "references": [ { "rule": "use" } ],
        "scopes": [ "block" ],
        "builtinTypes": [
          { "name": "whole", "core": "I32"  },
          { "name": "truth", "core": "Bool" }
        ],
        "literalTypes": []{TS}
      }
    })JSON";
    std::string const key = "{TS}";
    doc.replace(doc.find(key), key.size(), typeSpelling.empty() ? "" : std::string{",\n"} + std::string{typeSpelling});
    return doc;
}

[[nodiscard]] std::string diagnosticsOf(dss::LoadResult<std::shared_ptr<GrammarSchema>> const& r) {
    std::string out;
    if (r.has_value()) return out;
    for (auto const& d : r.error()) out += d.path + ": " + d.message + "\n";
    return out;
}

// ── THE LOAD ──────────────────────────────────────────────────────────────────

TEST(TypeSpelling, AnAbsentTableLoadsAndRecordsThatTheDocumentHasTypes) {
    auto const r = GrammarSchema::loadFromText(toyDocument(""));
    ASSERT_TRUE(r.has_value()) << diagnosticsOf(r);
    auto const& t = (*r)->semantics().typeSpelling;
    EXPECT_FALSE(t.declared);
    EXPECT_TRUE(t.documentHasTypes) << "the toy's rows reach I32 and Bool";
}

TEST(TypeSpelling, AKindTheRowsReachAndTheTableDoesNotSpellIsRefusedByName) {
    auto const r = GrammarSchema::loadFromText(toyDocument(R"("typeSpelling": { "kinds": { "I32": "whole" } })"));
    ASSERT_FALSE(r.has_value()) << "a table that spells I32 but not Bool loaded";
    std::string const d = diagnosticsOf(r);
    EXPECT_NE(d.find("/semantics/typeSpelling/kinds/Bool"), std::string::npos) << d;
    EXPECT_NE(d.find("'Bool'"), std::string::npos) << d;
}

TEST(TypeSpelling, AKindSpellingTheDocumentsRowsDoNotGiveThatKindIsRefused) {
    // `int` is no name the toy gives I32 (its row names it `whole`): a second copy free to drift.
    auto const r = GrammarSchema::loadFromText(
        toyDocument(R"("typeSpelling": { "kinds": { "I32": "int", "Bool": "truth" } })"));
    ASSERT_FALSE(r.has_value()) << "a spelling that restates the rows' name loaded";
    std::string const d = diagnosticsOf(r);
    EXPECT_NE(d.find("/semantics/typeSpelling/kinds/I32"), std::string::npos) << d;
    EXPECT_NE(d.find("'whole'"), std::string::npos) << "the refusal names the rows' own name: " << d;
}

TEST(TypeSpelling, AnUnknownKeyAndAnUnknownKindAreRefusedByName) {
    auto const unknownKey = GrammarSchema::loadFromText(toyDocument(
        R"("typeSpelling": { "kinds": { "I32": "whole", "Bool": "truth" }, "pointr": "*" })"));
    ASSERT_FALSE(unknownKey.has_value());
    EXPECT_NE(diagnosticsOf(unknownKey).find("pointr"), std::string::npos) << diagnosticsOf(unknownKey);
    auto const unknownKind = GrammarSchema::loadFromText(toyDocument(
        R"("typeSpelling": { "kinds": { "I32": "whole", "Bool": "truth", "Integer": "n" } })"));
    ASSERT_FALSE(unknownKind.has_value());
    EXPECT_NE(diagnosticsOf(unknownKind).find("'Integer' is no core type kind"), std::string::npos)
        << diagnosticsOf(unknownKind);
}

// ── A SECOND LANGUAGE: its own words, the same renderer ──────────────────────

TEST(TypeSpelling, ASecondLanguageSpellsItsTypesWithItsOwnWords) {
    auto const r = GrammarSchema::loadFromText(
        toyDocument(R"("typeSpelling": { "kinds": { "I32": "whole", "Bool": "truth" } })"));
    ASSERT_TRUE(r.has_value()) << diagnosticsOf(r);
    auto const& table = (*r)->semantics().typeSpelling;
    ASSERT_TRUE(table.declared);
    dss::TypeInterner in{dss::CompilationUnitId{1}};
    EXPECT_EQ(dss::spellType(in, in.primitive(TypeKind::I32), table), "whole");
    EXPECT_EQ(dss::spellType(in, in.primitive(TypeKind::Bool), table), "truth");
}

// ── THE SHIPPED DOCUMENTS (ruling 621) ────────────────────────────────────────
// Every shipped language document whose own rows reach a core type kind declares its table. The directory is
// walked (never a hand list), and a document that does not load as a root must be one another document folds in.

TEST(TypeSpelling, EveryShippedLanguageWithTypesSpellsThem) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    constexpr std::string_view kSuffix{".lang.json"};
    std::vector<std::string> stems;
    std::error_code          ec;
    for (std::filesystem::directory_iterator it{*cfg / "sources", ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        std::string const leaf = it->path().filename().generic_string();
        if (leaf.size() > kSuffix.size() && leaf.ends_with(kSuffix))
            stems.push_back(leaf.substr(0, leaf.size() - kSuffix.size()));
    }
    ASSERT_FALSE(ec) << "the walk of the shipped sources stopped: " << ec.message();
    std::sort(stems.begin(), stems.end());

    std::size_t              withTypes = 0;
    std::vector<std::string> refused, folded;
    for (std::string const& stem : stems) {
        auto const loaded = GrammarSchema::loadShipped(stem);
        if (!loaded.has_value()) {
            refused.push_back(stem);
            continue;
        }
        for (auto const& dep : (*loaded)->referencedDocuments())
            folded.push_back(std::filesystem::path{dep.path}.filename().generic_string());
        auto const& t = (*loaded)->semantics().typeSpelling;
        if (!t.documentHasTypes) continue;
        ++withTypes;
        EXPECT_TRUE(t.declared) << stem << ".lang.json: its rows reach core type kinds and it declares no "
                                << "'semantics.typeSpelling': its diagnostics would name no types";
    }
    for (std::string const& stem : refused) {
        std::string const leaf = stem + std::string{kSuffix};
        EXPECT_NE(std::find(folded.begin(), folded.end(), leaf), folded.end())
            << leaf << " does not load as a root and no shipped document folds it in: it was never checked";
    }
    // THE CONTROL: a predicate that matched nothing would pass every document.
    EXPECT_GT(withTypes, 0u) << "no shipped language document has types: is `documentHasTypes` still recorded?";
}

// ── THE C UNIVERSE ────────────────────────────────────────────────────────────
// The shipped c document's spelling of every type form. The expected texts are clang 18.1.3's (`-std=c2x`), MEASURED
// by the n507c types probe: each line initializes a structure from an expression of the type under test, and clang
// names that type in its refusal; gcc 13.3.0 names none ("invalid initializer"), so clang's is the one spelling a
// reference prints. Where a row differs from clang, it is what the lattice IS, said beside the row: `const` is no
// bit of the lattice (`int (const char *, ...)` is spelled from a `char *` parameter), an anonymous tag carries no
// location in the type, and `_Atomic` is written as the qualifier (`_Atomic int`) where clang writes the specifier
// form `_Atomic(int)` — one type, both valid C. The forms the probe did not write follow the same rules (a joined
// array suffix, an unknown bound, a variadic list of no parameters, a function declared without a prototype).

struct CRow {
    std::string_view   what;
    dss::TypeId        type;
    std::string_view   expected;
    dss::SpellOptions  options{};
};

TEST(TypeSpelling, TheShippedCDocumentSpellsEveryFormAsClangDoes) {
    auto const c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value()) << diagnosticsOf(c);
    auto const& table = (*c)->semantics().typeSpelling;
    ASSERT_TRUE(table.declared) << "the shipped c document spells its types";

    using dss::CallConv;
    dss::TypeInterner in{dss::CompilationUnitId{1}};
    dss::TypeId const i   = in.primitive(TypeKind::I32);
    dss::TypeId const d   = in.primitive(TypeKind::F64);
    dss::TypeId const f   = in.primitive(TypeKind::F32);
    dss::TypeId const v   = in.primitive(TypeKind::Void);
    dss::TypeId const ch  = in.primitive(TypeKind::Char);
    dss::TypeId const one[] = {i};
    dss::TypeId const S   = in.structType("S", one);
    dss::TypeId const U   = in.unionType("U", one);
    dss::TypeId const E   = in.enumType("E");
    dss::TypeId const anonymous = in.structType("<anon:structSpec:7>", one);
    dss::TypeId const untagged  = in.structType("", one);
    dss::TypeId const intDouble[] = {i, d};
    dss::TypeId const fnProto = in.fnSig(intDouble, i, CallConv::CcSysV);
    dss::TypeId const fnVoid  = in.fnSig({}, i, CallConv::CcSysV);
    dss::TypeId const charPtr[] = {in.pointer(ch)};
    dss::TypeId const fnVar   = in.fnSig(charPtr, i, CallConv::CcSysV, /*isVariadic=*/true);
    dss::TypeId const fnOnlyVariadic = in.fnSig({}, i, CallConv::CcSysV, /*isVariadic=*/true);
    dss::TypeId const pfr = in.pointer(in.fnSig(one, in.pointer(in.array(i, 4)), CallConv::CcSysV));

    std::vector<CRow> const rows = {
        {"int", i, "int"},
        {"unsigned int", in.primitive(TypeKind::U32), "unsigned int"},
        {"long (the lattice's vocabulary tag)", in.primitive(TypeKind::I64, "long"), "long"},
        {"unsigned long (the tag)", in.primitive(TypeKind::U64, "unsigned long"), "unsigned long"},
        {"long long", in.primitive(TypeKind::I64), "long long"},
        {"short", in.primitive(TypeKind::I16), "short"},
        {"unsigned short", in.primitive(TypeKind::U16), "unsigned short"},
        {"signed char", in.primitive(TypeKind::I8), "signed char"},
        {"unsigned char", in.primitive(TypeKind::U8), "unsigned char"},
        {"char", ch, "char"},
        {"bool", in.primitive(TypeKind::Bool), "bool"},
        {"float", f, "float"},
        {"double", d, "double"},
        {"long double (x87)", in.primitive(TypeKind::F80), "long double"},
        {"long double (binary128)", in.primitive(TypeKind::F128), "long double"},
        {"volatile int", in.volatileQualified(i), "volatile int"},
        {"_Atomic int (clang: `_Atomic(int)`)", in.atomicQualified(i), "_Atomic int"},
        {"int *", in.pointer(i), "int *"},
        {"int *volatile", in.volatileQualified(in.pointer(i)), "int *volatile"},
        {"volatile int *", in.pointer(in.volatileQualified(i)), "volatile int *"},
        {"int **", in.pointer(in.pointer(i)), "int **"},
        {"int (*)[3]", in.pointer(in.array(i, 3)), "int (*)[3]"},
        {"int *(*)[3]", in.pointer(in.array(in.pointer(i), 3)), "int *(*)[3]"},
        {"int (int, double)", fnProto, "int (int, double)"},
        {"int (void)", fnVoid, "int (void)"},
        {"int (char *, ...) (clang: `int (const char *, ...)`)", fnVar, "int (char *, ...)"},
        {"int (*(*)(int))[4]", pfr, "int (*(*)(int))[4]"},
        {"struct S", S, "struct S"},
        {"union U", U, "union U"},
        {"enum E", E, "enum E"},
        {"struct S *", in.pointer(S), "struct S *"},
        {"an anonymous member's structure (clang: `struct (unnamed struct at FILE:LINE:COL)`)", anonymous,
         "struct (unnamed)"},
        {"a structure with no tag", untagged, "struct (unnamed)"},
        {"_BitInt(13)", in.bitInt(13, true), "_BitInt(13)"},
        {"unsigned _BitInt(70)", in.bitInt(70, false), "unsigned _BitInt(70)"},
        {"_Complex double", in.complex(d), "_Complex double"},
        {"_Complex float", in.complex(f), "_Complex float"},
        {"int (*)(int, double)", in.pointer(fnProto), "int (*)(int, double)"},
        // the forms the probe did not write, by the same rules
        {"int[3] (an array suffix joins the specifiers)", in.array(i, 3), "int[3]"},
        {"int[] (an unknown bound)", in.incompleteArray(i), "int[]"},
        {"int *[3]", in.array(in.pointer(i), 3), "int *[3]"},
        {"void (*)(int, ...)", in.pointer(in.fnSig(one, v, CallConv::CcSysV, /*isVariadic=*/true)),
         "void (*)(int, ...)"},
        {"int (...) (a variadic list of no parameters, C23)", fnOnlyVariadic, "int (...)"},
        {"int () (declared without a prototype: a fact of the symbol, passed by the caller)", fnVoid, "int ()",
         dss::SpellOptions{.declaredWithoutPrototype = true}},
        {"int (*)(void) (the option reaches the OUTERMOST function form only)", in.pointer(fnVoid), "int (*)(void)",
         dss::SpellOptions{.declaredWithoutPrototype = false}},
    };
    for (CRow const& row : rows) {
        EXPECT_EQ(dss::spellType(in, row.type, table, row.options), row.expected) << row.what;
    }
}

// ── THE CORE-KIND FACE (`spellCoreKind`): a declared type known only by its kind and tag ─────────────────────────

TEST(TypeSpelling, AKindAndItsTagAreSpelledByTheTableOrNotAtAll) {
    auto const c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value()) << diagnosticsOf(c);
    auto const& table = (*c)->semantics().typeSpelling;
    EXPECT_EQ(dss::spellCoreKind(TypeKind::I64, "long", table), std::optional<std::string>{"long"});
    EXPECT_EQ(dss::spellCoreKind(TypeKind::I64, "", table), std::optional<std::string>{"long long"});
    EXPECT_EQ(dss::spellCoreKind(TypeKind::F80, "", table), std::optional<std::string>{"long double"});
    dss::TypeSpelling const none{};
    EXPECT_EQ(dss::spellCoreKind(TypeKind::I64, "long", none), std::nullopt)
        << "a language whose document states no table names no types";
}

// ── A KIND THE TABLE HAS NO WORD FOR: one only a descriptor reaches ──────────────────────────────────────────────
// The load refuses a kind the document's OWN rows reach unspelled. A kind reached from outside them — a shipped or
// user descriptor's `f16` constant or parameter, in a language with no half-precision type — has no word in the
// language: the renderer answers "not spelled" for it and for every type built on it, and the sentence takes its
// form without a type. Before, `spellType` aborted the compiler there (MEASURED: test_math_h_float_constants,
// an `f16` descriptor constant on a live include, stopped with exit 0xc0000409).
TEST(TypeSpelling, AKindTheTableHasNoWordForIsNotSpelled) {
    auto const c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value()) << diagnosticsOf(c);
    auto const& table = (*c)->semantics().typeSpelling;
    ASSERT_TRUE(table.declared) << "the case is a DECLARED table without the word";
    ASSERT_TRUE(table.kind(TypeKind::F16).empty())
        << "the fixture needs a kind the shipped c table has no word for; F16 now has one: choose another";
    dss::TypeInterner in{dss::CompilationUnitId{1}};
    dss::TypeId const h = in.primitive(TypeKind::F16);
    dss::TypeId const i = in.primitive(TypeKind::I32);
    dss::TypeId const takesHalf[] = {h};
    EXPECT_EQ(dss::spellType(in, h, table), std::nullopt);
    EXPECT_EQ(dss::spellType(in, in.pointer(h), table), std::nullopt) << "nor a type built on it";
    EXPECT_EQ(dss::spellType(in, in.fnSig(takesHalf, i, dss::CallConv::CcSysV), table), std::nullopt)
        << "nor a function with a parameter of it";
    EXPECT_EQ(dss::spellCoreKind(TypeKind::F16, "", table), std::nullopt);
    EXPECT_EQ(dss::spellType(in, in.pointer(i), table), std::optional<std::string>{"int *"})
        << "CONTROL: a type the table spells";
}

} // namespace
