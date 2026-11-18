// The type names a compilation unit's FIRST parse knows — and the ones it must not.
//
// A C translation unit parses once, with every type name it can see: its own text,
// a quote-`#include`d header's text (the C preprocessor splices it inline before
// the parse), and a shipped system descriptor's typedef NAMES (seeded into the
// binder sketch before the parse, D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION). So a cast
// through any of them commits on that parse. A name only ANOTHER translation unit
// of the build declares is no type name here — a typedef's scope ends with its own
// translation unit (C 6.2.1) — and gcc 13.3.0, clang 18.1.3 and cl refuse such a
// use in every form measured (P69 L2b2, 24 of 24); a use written before its own
// typedef is none either (C 6.2.1p7). These tests pin the reading of the first
// parse (the cast subtree exists, or does not); which diagnostic refuses a non-type
// use is the analyzer's.
//
// P69 L2b2 (D-PP-FC2-ORACLE-RETIRE): this file pinned the compilation-unit type-name
// oracle and its one-shot reparse, which that row retired; every pin that held a
// first-parse property stays here as one.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "link/object_format_schema.hpp"

#include "analysis/compilation_unit/toy_cu_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace dss;
using dss::cu_test::hasCode;
using dss::cu_test::loadShippedSchema;

// RAII temp directory (same facade as test_import_resolver.cpp): include
// targets must share the includer's directory. The facade lives in
// `toy_cu_fixture.hpp` — its unique-path scheme is NOT reimplemented locally
// because a reimplemented one is exactly what collided: a fixed scratch path
// let concurrent processes of this binary delete each other's fixtures. The
// GROUP below is this suite's own, so its scratch tree stays separate from
// that sibling's.
constexpr char kScratchGroup[] = "unit-type-names";
using TempDir = dss::cu_test::ScratchSourceDir<kScratchGroup>;

[[nodiscard]] std::size_t countRule(Tree const& t, std::string_view ruleName) {
    if (!t.hasSchema()) return 0;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return 0;
    std::size_t n = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) ++n;
    }
    return n;
}

[[nodiscard]] bool treeHasRule(Tree const& t, std::string_view ruleName) {
    return countRule(t, ruleName) > 0;
}

} // namespace

// The typedef lives in a quote-included header, the cast in the includer: the
// header's text is spliced inline, so the cast commits on the first parse.
TEST(UnitTypeNames, AQuoteIncludedTypedefCommitsTheCastOnTheFirstParse) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"types.h\"\n"
        "int main() { return (MyT)-1 + (MyT)1 + (int)2; }\n");
    dir.write("types.h", "typedef int MyT;\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    Tree const& mainTree = cu.trees()[0];
    EXPECT_FALSE(mainTree.diagnostics().hasErrors());
    EXPECT_EQ(countRule(mainTree, "castExpr"), 3u)
        << "the inlined typedef must let every cast through it commit on the first parse";
    EXPECT_FALSE(treeHasRule(mainTree, "parenExpr"));
    EXPECT_TRUE(cu.crossRefs().empty());
    EXPECT_FALSE(hasCode(cu.driverDiagnostics(),
                         DiagnosticCode::D_UnresolvedImport));
}

// A name nothing declares keeps the value reading before an operator — the
// analysis is the authority on the undeclared name.
TEST(UnitTypeNames, AnUndeclaredNameBeforeAnOperatorKeepsTheValueReading) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "int main() { int x; return (zzz)-x; }\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_FALSE(treeHasRule(cu.trees()[0], "castExpr"));
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "parenExpr"));
}

// Nested includes (a header includes a header): both texts are spliced, so a
// typedef two include-hops away commits the cast too.
TEST(UnitTypeNames, ATransitivelyIncludedTypedefCommitsTheCast) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"outer.h\"\n"
        "int main() { return (DeepT)-1; }\n");
    dir.write("outer.h", "#include \"inner.h\"\nint helper() { return 0; }\n");
    dir.write("inner.h", "typedef int DeepT;\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "castExpr"));
}

// ★ P69 L2b2 (lane `lm`, the L2b1 design audit's condition 4): A USE WRITTEN BEFORE A TYPEDEF IS
// NO TYPE NAME. helper.h (defines HelpT, uses MainT) is inlined into main.c (defines MainT): after
// the splice, helper.h's `(MainT)-v` stands in the buffer BEFORE `typedef int MainT;`. MainT's scope
// begins after its declarator (C 6.2.1p7), so at the use MainT is undeclared — gcc 13.3.0, clang
// 18.1.3 and cl 19.51 refuse the same program written in one file ("'MainT' undeclared", ✔MEASURED
// 2026-11-04, probe-reference-cc runs 20261104-205440-634751cd and 20261104-205634-b38088e4, probe
// c4_forward_use_plain). The cast through the EARLIER typedef HelpT stands, and `(MainT)-v` keeps
// its value reading for the analysis to refuse.
TEST(UnitTypeNames, AUseBeforeTheTypedefInOneBufferIsNoTypeName) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "#include \"helper.h\"\n"
        "typedef int MainT;\n"
        "int main() { return (HelpT)-1; }\n");
    dir.write("helper.h",
              "typedef int HelpT;\n"
              "int helper(int v) { return (MainT)-v; }\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_EQ(countRule(cu.trees()[0], "castExpr"), 1u)
        << "the cast through HelpT, declared before its use, and no other";
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "parenExpr"))
        << "`(MainT)-v`, written before MainT's typedef, keeps its value reading";
}

// The same rule whatever wrote the tokens: the plain forward use, and the same program written by ONE
// macro whose argument is both the use and the typedef's name (the two tokens then share a span). The
// NEIGHBOURS of that use, each in both spellings, are no cast either — a TAG of the spelling (no
// ordinary identifier at all), an ENUMERATION CONSTANT declared after the use, a typedef in ANOTHER
// function's scope: every reference refuses each of them as undeclared (gcc 13.3.0, clang 18.1.3,
// cl 19.51; ✔MEASURED 2026-11-04, probe-reference-cc runs 20261104-214127-0d4588a1,
// 20261104-214242-d9a4fc6a, 20261104-214336-281cce67). Which diagnostic refuses them is the
// analyzer's, read where a use resolves.
TEST(UnitTypeNames, AUseBeforeItsDeclarationIsNoCastWhateverWroteItsTokens) {
    struct Form { char const* what; char const* src; };
    Form const forms[] = {
        {"the plain forward use",
         "static int use(void) { return (F)(1) + 41; }\ntypedef int F;\n"
         "int main(void) { return use(); }\n"},
        {"one macro writing the use and the typedef",
         "#define FWD(n) static int use(void) { return (n)(1) + 41; } typedef int n;\nFWD(F)\n"
         "int main(void) { return use(); }\n"},
        {"a tag of the spelling, plain",
         "static int use(void) { return (F)(1) + 41; }\nstruct F { int a; };\n"
         "int main(void) { return use(); }\n"},
        {"a tag of the spelling, one macro",
         "#define FWD(n) static int use(void) { return (n)(1) + 41; } struct n { int a; };\nFWD(F)\n"
         "int main(void) { return use(); }\n"},
        {"an enumeration constant declared after the use, plain",
         "static int use(void) { return (E)(1) + 41; }\nenum { E = 1 };\n"
         "int main(void) { return use(); }\n"},
        {"an enumeration constant declared after the use, one macro",
         "#define FWD(n) static int use(void) { return (n)(1) + 41; } enum { n = 1 };\nFWD(E)\n"
         "int main(void) { return use(); }\n"},
        {"a typedef in another function's scope, plain",
         "static int use(void) { return (F)(1) + 41; }\n"
         "int main(void) { typedef int F; F r = use(); return r; }\n"},
        {"a typedef in another function's scope, one macro",
         "#define FWD(n) static int use(void) { return (n)(1) + 41; } "
         "int main(void) { typedef int n; n r = use(); return r; }\nFWD(F)\n"},
    };
    for (Form const& f : forms) {
        TempDir dir;
        auto main = dir.write("main.c", f.src);
        UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
        b.addFile(main);
        auto cu = std::move(b).finish();
        ASSERT_EQ(cu.trees().size(), 1u) << f.what;
        EXPECT_FALSE(treeHasRule(cu.trees()[0], "castExpr"))
            << f.what << ": the use became a cast";
    }
}

// D-CSUBSET-FN-TYPE-TYPEDEF-PAREN-NAME (TF-C64): a parenthesized-NAME typedef (`typedef int (F);` /
// the function-type `typedef int (F)(int);` — pervasive in real tcl.h as
// `typedef int (Tcl_AppInitProc)(Tcl_Interp*)`) DECLARES its name: while its `(F)` declarator
// parses, F is not yet in scope (C 6.2.1p7), so `fnSuffix`'s requireKnownType guard rolls back to the
// parenthesized-declarator reading. Read as an abstract function suffix (F a parameter TYPE) it
// would declare nothing (S_DeclarationDeclaresNothing).
TEST(UnitTypeNames, AParenNameTypedefDeclaresItsName) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "typedef int (F)(int);\n"          // fn-TYPE typedef, paren name
        "typedef int (G);\n"               // bare paren name
        "int main(void) { return 0; }\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    // F/G are NAMES: the `(F)` parses as a parenthesized declarator, not an
    // abstract function-suffix.
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "parenDeclarator"));
    EXPECT_FALSE(treeHasRule(cu.trees()[0], "fnSuffix"))
        << "neither typedef's `(F)`/`(G)` may parse as a function-suffix param";
}

// A parenthesized-name typedef DEFINED, then USED as a cast after its definition: F is bound by
// the time the cast parses, so the definition stands (F is the NAME) and the cast commits.
TEST(UnitTypeNames, AParenNameTypedefThenItsCastUseCommitsTheCast) {
    TempDir dir;
    auto main = dir.write(
        "main.c",
        "typedef int (F);\n"
        "int main(void) { return (F)0; }\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "parenDeclarator"));
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "castExpr"));
}

// ── D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION ─────────────────────────────────────
//
// A shipped SYSTEM descriptor's typedefs are injected SEMANTICALLY (post-parse), so an angle
// `#include <h>` does NOT splice them into the buffer. The unit seeds the LIVE resolved
// descriptors' typedef NAMES into the binder sketch BEFORE the parse (parseAndAdd_ harvests
// pp.resolvedShippedDescriptors), so a cast through one commits on that parse.
// RED-ON-DISABLE: revert the seed harvest → `MyT` is unknown to the sketch → the cast's
// requireKnownType guard refuses it → `(MyT)(0)` is a call and `(MyT)-1` a subtraction.
TEST(UnitTypeNames, AShippedTypedefCommitsTheCastOnTheFirstParse) {
    TempDir srcDir;
    TempDir sysDir;
    auto main = srcDir.write(
        "main.c",
        "#include <mydefs.h>\n"
        "int main() { return (MyT)(0) + (MyT)-1 + (int)sizeof(MyT) - 4; }\n");
    // A NEUTRAL descriptor: the typedef `MyT` is a typed surface injected
    // semantically, NOT spliced into the buffer like a quote include's text.
    sysDir.write(
        "mydefs.json",
        R"({ "header": "mydefs.h",
             "typedefs": [ { "name": "MyT", "type": "i32" } ] })");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addSystemDir(sysDir.path());
    b.addFile(main);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_FALSE(cu.trees()[0].diagnostics().hasErrors());
    EXPECT_EQ(countRule(cu.trees()[0], "castExpr"), 3u)
        << "the seeded shipped typedef must let every cast through it commit on the first parse";
    EXPECT_TRUE(treeHasRule(cu.trees()[0], "sizeofType"))
        << "and read `sizeof(MyT)` as the size of the type";
    // The descriptor path is still recorded for semantic injection (the seed is
    // additive, not a replacement for the import-resolver record).
    EXPECT_EQ(cu.shippedLibDescriptors().size(), 1u);
}

// P69 L2b2 (ruling 516): a shipped descriptor's VALUE names (its symbols and constants) are seeded
// into the binder sketch before the parse too, so a parenthesised one is a KNOWN name — never the
// type triage's refusal, never named undeclared: `(my_fn)42;` gives the follower's error alone, as
// gcc 13.3.0, clang 18.1.3 and cl give `(printf)42;` after `#include <stdio.h>` (✔MEASURED
// 2026-11-04, probe b06: "expected ';'" at the literal and nothing else). The CONTROL: without the
// include nothing declares the name, and the parser names it undeclared before the error.
TEST(UnitTypeNames, AShippedDescriptorsValueNameIsAKnownName) {
    TempDir srcDir;
    TempDir sysDir;
    auto main = srcDir.write("main.c",
                             "#include <myfns.h>\n"
                             "int main(void) { (my_fn)42; return 0; }\n");
    sysDir.write(
        "myfns.json",
        R"({ "header": "myfns.h",
             "symbols": [ { "name": "my_fn", "signature": "fn() -> i32",
                            "kind": "function", "linkage": "external" } ] })");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addSystemDir(sysDir.path());
    b.addFile(main);
    auto cu = std::move(b).finish();
    ASSERT_EQ(cu.trees().size(), 1u);
    ASSERT_TRUE(cu.trees()[0].diagnostics().hasErrors()) << "`42` cannot follow `(my_fn)`";
    EXPECT_FALSE(hasCode(cu.trees()[0].diagnostics(), DiagnosticCode::S_UndeclaredIdentifier))
        << "a shipped descriptor's value name is a known name";

    TempDir ctl;
    auto c = ctl.write("c.c", "int main(void) { (my_fn)42; return 0; }\n");
    UnitBuilder b2{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b2.addFile(c);
    auto cu2 = std::move(b2).finish();
    ASSERT_EQ(cu2.trees().size(), 1u);
    EXPECT_TRUE(hasCode(cu2.trees()[0].diagnostics(), DiagnosticCode::S_UndeclaredIdentifier))
        << "without the include nothing declares `my_fn`, and the parser names it";
}

// ★ P69 L2b2 (ruling 494's measurement; D-PP-FC2-ORACLE-RETIRE): ANOTHER TRANSLATION UNIT'S
// TYPEDEF IS NO TYPE NAME HERE, in either direction. a.c declares AT and uses BT; b.c declares BT
// and uses AT; neither sees the other's typedef (C 6.2.1). gcc 13.3.0, clang 18.1.3 and cl refuse
// every such use (✔MEASURED 2026-11-04: 24 of 24 cells, probe-reference-cc linux host run
// 20261104-230115-6587f953, windows run 20261104-230237-8f520aeb), so no use is a type reading:
// `sizeof(BT)` is the value form and `_Alignof(AT)` the value form, for the analysis to refuse. The
// CONTROL: the same use through a quote-included header that declares the typedef commits the type
// reading. (The retired oracle seeded another tree's type name and reparsed: the uses became types.)
TEST(UnitTypeNames, AnotherTranslationUnitsTypedefIsNoTypeNameHere) {
    TempDir dir;
    auto a = dir.write("a.c",
                       "typedef int AT;\n"
                       "int fa(void) { return sizeof(BT) + (BT)-1; }\n");
    auto bfile = dir.write("b.c",
                           "typedef int BT;\n"
                           "int fb(void) { return _Alignof(AT) + (AT)-1; }\n");

    UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    b.addFile(a);
    b.addFile(bfile);
    auto cu = std::move(b).finish();

    ASSERT_EQ(cu.trees().size(), 2u);
    for (Tree const& t : cu.trees()) {
        EXPECT_FALSE(t.diagnostics().hasErrors());
        EXPECT_FALSE(treeHasRule(t, "castExpr"))
            << "another translation unit's typedef was read as a type name in a cast";
        EXPECT_FALSE(treeHasRule(t, "sizeofType"));
        EXPECT_FALSE(treeHasRule(t, "alignofType"));
    }

    TempDir ctl;
    auto c = ctl.write("c.c",
                       "#include \"bt.h\"\n"
                       "int fc(void) { return (int)sizeof(BT) + (int)_Alignof(BT) + (BT)-1; }\n");
    ctl.write("bt.h", "typedef int BT;\n");
    UnitBuilder cb{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
    cb.addFile(c);
    auto ccu = std::move(cb).finish();
    ASSERT_EQ(ccu.trees().size(), 1u);
    EXPECT_FALSE(ccu.trees()[0].diagnostics().hasErrors());
    EXPECT_TRUE(treeHasRule(ccu.trees()[0], "sizeofType"));
    EXPECT_TRUE(treeHasRule(ccu.trees()[0], "alignofType"));
    EXPECT_EQ(countRule(ccu.trees()[0], "castExpr"), 3u)
        << "the control: `(int)sizeof`, `(int)_Alignof` and `(BT)-1` through the included typedef";
}

// ★ P69 fold 3f (ledger 563 (A)): A DESCRIPTOR'S VALUE NAME AVAILABLE ON ONE ARCH OF A FORMAT IS A
// KNOWN NAME ON THAT ARCH ALONE. A symbol row's availability may name a format on one arch
// (`{"format": "elf", "arch": "arm64"}`); the semantic tier declares the row on the pair the entry
// matches and on no other, so the value seed the parse reads is harvested on the same PAIR -- the
// unit's format AND its target's arch. Without the arch the entry is undecided and dropped, and
// `(my_fn)42;` on arm64 elf would name `my_fn` undeclared where the semantic tier declares it. The
// pair is applied the way the driver applies it (`applyTargetFormatPair`).
TEST(UnitTypeNames, AValueNameAvailableOnOneArchIsKnownOnThatArchAlone) {
    TempDir srcDir;
    TempDir sysDir;
    auto main = srcDir.write("main.c",
                             "#include <archfns.h>\n"
                             "int main(void) { (my_fn)42; return 0; }\n");
    sysDir.write(
        "archfns.json",
        R"({ "header": "archfns.h",
             "symbols": [ { "name": "my_fn", "signature": "fn() -> i32",
                            "kind": "function", "linkage": "external",
                            "availableObjectFormats": [ { "format": "elf", "arch": "arm64" } ] } ] })");

    struct Pair {
        char const* target;
        char const* format;
        bool        known;
    };
    for (Pair const& p :
         {Pair{"arm64", "elf64-aarch64-linux", true}, Pair{"x86_64", "elf64-x86_64-linux", false}}) {
        SCOPED_TRACE(std::string{p.target} + ":" + p.format);
        auto const target = TargetSchema::loadShipped(p.target);
        ASSERT_TRUE(target.has_value());
        auto const format = ObjectFormatSchema::loadShipped(p.format);
        ASSERT_TRUE(format.has_value());
        UnitBuilder b{loadShippedSchema("c"), DiagnosticBudget::libraryDefault()};
        applyTargetFormatPair(b, **target, **format);
        b.addSystemDir(sysDir.path());
        b.addFile(main);
        auto cu = std::move(b).finish();
        ASSERT_EQ(cu.trees().size(), 1u);
        ASSERT_TRUE(cu.trees()[0].diagnostics().hasErrors()) << "`42` cannot follow `(my_fn)`";
        EXPECT_EQ(hasCode(cu.trees()[0].diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), !p.known)
            << (p.known ? "the row's arch entry matches this pair, so `my_fn` is a known name here"
                        : "the row exists on arm64 alone, so nothing declares `my_fn` here");
    }
}
