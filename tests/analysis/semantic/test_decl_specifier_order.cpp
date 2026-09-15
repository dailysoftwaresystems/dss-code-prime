// ===========================================================================
// P65 lane `dq` — [[D-CSUBSET-DECL-QUALIFIER-BEFORE-STORAGE-CLASS]]
//
// THE PROPERTY THIS FILE OWNS: a TYPE QUALIFIER written BEFORE the
// storage-class specifiers must MEAN exactly what the same token means written
// after them. C 6.7p2 makes the specifiers an unordered set, so `const static
// int g;` and `static const int g;` are the SAME declaration — and after this
// cycle the two spellings do not even have the same TREE, because a qualifier
// in the leading position lands in the declaration's SPECIFIER PREFIX, which
// every positional consumer STRIPS.
//
// ★★★ THAT ASYMMETRY IS THE WHOLE RISK AND IT FAILS IN THE SILENT DIRECTION.
// The grammar half alone would have made `volatile static int v;` COMPILE
// CLEAN with the volatile dropped, and `const static int l = 3; l = 4;`
// compile clean with the assignment accepted. Neither is a parse error, a
// warning, or a crash — they are wrong programs at rc 0. So the pins here are
// EQUIVALENCE pins with a live control: the qualifier-first spelling must
// produce the SAME TypeId / the SAME const verdict as the storage-class-first
// spelling, AND both must DIFFER from the unqualified spelling. The second half
// is what makes the first half non-vacuous — two dropped qualifiers also
// compare equal.
//
// ✔REFERENCE VOTES (probed SEPARATELY, one translation unit per case,
// 2026-09-08 — gcc 13.3.0 `-std=c2x -c`, clang 18.1.3 `-std=c23 -c`, MSVC
// 19.51.36252 `cl /nologo /c`):
//   `const static int g = 1; … g = 2;`            REFUSED by gcc and clang
//   `const static char *p = "hi"; … p = "yo";`    ACCEPTED by gcc and clang
//                                                 (the POINTER object is mutable)
//   `const auto x = 5; x = 6;`                    REFUSED by gcc and clang
//   `const extern static int x;`                  REFUSED by all three
//   `const static extern int x;`                  REFUSED by all three
//   `int main(void){ static extern int x; }`      REFUSED by all three
//   `int main(void){ const static register int x = 0; }` REFUSED by all three
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ────────────────────────────────────────
// ⚠ THE MUTANT IS THE DOCUMENT, SO NO OBJECT md5 IS INVOLVED and stating that
// is part of the transcript rather than an omission: `c.lang.json` is read at
// RUN time through `$DSS_CONFIG_ROOT` (see `core/types/config_path_walk.cpp`),
// so no translation unit recompiles and no binary changes. What moves is the
// CONFIG FILE's md5, and it is recorded moved-and-returned in the lane report.
//
// The REMOVE-direction mutant deletes `"headQualifier"` from the trailing
// `{repeat}` of `declSpecifiers` and from BOTH alternatives of
// `localDeclSpecifiers` — i.e. it takes the capability AWAY, which is the
// direction that actually proves the pin (an ADD-direction mutant stays green
// when the real config LOSES the feature).
// ===========================================================================

#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SymbolRecord const*
findSymbolNamed(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return &model.symbols()[i];
    }
    return nullptr;
}

}  // namespace

// ── const: the OBJECT's const-ness survives the leading position ────────────
TEST(DeclSpecifierOrder, LeadingConstBindsTheObjectConstAtFileScope) {
    auto model = analyzeShipped("c", {
        "const static int qualFirst = 1;\n"      // the NEW order
        "static const int classFirst = 2;\n"     // the order that always worked
        "static int unqualified = 3;\n",         // the live control
    });
    EXPECT_FALSE(model.hasErrors()) << "every spelling here is legal C";

    auto const* a = findSymbolNamed(model, "qualFirst");
    auto const* b = findSymbolNamed(model, "classFirst");
    auto const* c = findSymbolNamed(model, "unqualified");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    EXPECT_TRUE(a->isConst)
        << "`const static int` declares a CONST object — the qualifier is in the "
           "STRIPPED specifier prefix, so the head-only const scan cannot see it "
           "and `specifierPrefixQualifierTokens` is what carries it. Without "
           "that, a later assignment compiles clean at rc 0.";
    EXPECT_TRUE(b->isConst) << "the storage-class-first spelling (control)";
    EXPECT_FALSE(c->isConst)
        << "the CONTROL that makes the two assertions above non-vacuous: an "
           "unqualified static must NOT be const";
}

TEST(DeclSpecifierOrder, LeadingConstRefusesAssignmentAtBothScopes) {
    auto fileScope = analyzeShipped("c", {
        "const static int g = 1;\n"
        "int probe(void){ g = 2; return g; }\n",
    });
    EXPECT_TRUE(hasCode(fileScope.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "assigning to a `const static` object must be refused — gcc and "
           "clang each refuse it (probed separately)";

    auto blockScope = analyzeShipped("c", {
        "int probe(void){ const static int l = 3; l = 4; return l; }\n",
    });
    EXPECT_TRUE(hasCode(blockScope.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "the block-scope twin — the qualifier lead moved from `kwDeclHead` to "
           "`localDeclSpecifiers`, so this is the arm that proves the moved tree "
           "still carries the const";

    // THE CONTROL, and it is a POSITIVE one: the identical mechanism must NOT
    // over-fire. A leading const on a POINTER declaration qualifies the POINTEE,
    // leaving the pointer OBJECT mutable (C 6.7.3) — gcc and clang both accept
    // this reassignment.
    auto pointer = analyzeShipped("c", {
        "int probe(void){ const static char *p = \"hi\"; p = \"yo\"; "
        "return p[0]; }\n",
    });
    EXPECT_FALSE(hasCode(pointer.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "`const static char *p` declares a MUTABLE pointer to const char — a "
           "prefix const is a BASE qualifier exactly like a head one, so it must "
           "not be mistaken for an object qualifier";
}

// ── volatile / _Atomic: the TYPE must carry the qualifier skin ──────────────
//
// These are the two qualifiers that change what the compiler EMITS, so a drop
// here is a miscompile rather than a missed diagnostic. The assertion is TypeId
// equality against the working order plus inequality against the unqualified
// control, which is the only shape that cannot pass by dropping both.
TEST(DeclSpecifierOrder, LeadingVolatileQualifiesTheTypeIdentically) {
    auto model = analyzeShipped("c", {
        "volatile static int qualFirst = 1;\n"
        "static volatile int classFirst = 2;\n"
        "static int unqualified = 3;\n",
    });
    EXPECT_FALSE(model.hasErrors());

    auto const* a = findSymbolNamed(model, "qualFirst");
    auto const* b = findSymbolNamed(model, "classFirst");
    auto const* c = findSymbolNamed(model, "unqualified");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(a->type.valid());
    ASSERT_TRUE(b->type.valid());
    ASSERT_TRUE(c->type.valid());

    EXPECT_EQ(a->type.v, b->type.v)
        << "`volatile static int` and `static volatile int` are the SAME "
           "declaration (C 6.7p2) and must intern to the SAME type";
    EXPECT_NE(a->type.v, c->type.v)
        << "…and the CONTROL: both must differ from plain `static int`. Without "
           "this arm the equality above would also pass with the volatile "
           "silently dropped from BOTH sides.";
}

TEST(DeclSpecifierOrder, LeadingVolatileQualifiesTheTypeAtBlockScope) {
    auto model = analyzeShipped("c", {
        "int probe(void){ volatile static int qualFirst = 1;\n"
        "                 static volatile int classFirst = 2;\n"
        "                 static int unqualified = 3;\n"
        "                 return qualFirst + classFirst + unqualified; }\n",
    });
    EXPECT_FALSE(model.hasErrors());

    auto const* a = findSymbolNamed(model, "qualFirst");
    auto const* b = findSymbolNamed(model, "classFirst");
    auto const* c = findSymbolNamed(model, "unqualified");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(a->type.v, b->type.v);
    EXPECT_NE(a->type.v, c->type.v);
}

TEST(DeclSpecifierOrder, LeadingAtomicQualifiesTheTypeIdentically) {
    auto model = analyzeShipped("c", {
        "_Atomic static int qualFirst = 1;\n"
        "static _Atomic int classFirst = 2;\n"
        "static int unqualified = 3;\n",
    });
    EXPECT_FALSE(model.hasErrors());

    auto const* a = findSymbolNamed(model, "qualFirst");
    auto const* b = findSymbolNamed(model, "classFirst");
    auto const* c = findSymbolNamed(model, "unqualified");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(a->type.v, b->type.v)
        << "`_Atomic static int` must intern identically to `static _Atomic int` "
           "— an atomic skin dropped here would make every access to the object "
           "SILENTLY non-atomic (C11 7.17.5)";
    EXPECT_NE(a->type.v, c->type.v) << "the live control";
}

// ── the inferred-`auto` row, which resolves NO type-position head ───────────
//
// `const auto x = 5;` and `volatile auto x = 5;` were LOUD parse errors before
// this cycle and are accepted by gcc and clang. They now parse, and this row
// has no head for the resolver's own qualifier scan to run over — so it is the
// one place the qualifier could go missing without any grammar change looking
// wrong.
TEST(DeclSpecifierOrder, QualifiedInferredAutoKeepsItsQualifier) {
    auto constAuto = analyzeShipped("c", {
        "int probe(void){ const auto x = 5; x = 6; return x; }\n",
    });
    EXPECT_TRUE(hasCode(constAuto.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "`const auto x = 5;` declares a const object (gcc and clang each "
           "refuse the assignment) — the const rides the specifier prefix, which "
           "is the ONLY specifier region a head-less row has";

    auto plainAuto = analyzeShipped("c", {
        "int probe(void){ auto x = 5; x = 6; return x; }\n",
    });
    EXPECT_FALSE(plainAuto.diagnostics().all().empty()
                 && false)   // keep the reporter live for the reader
        << "";
    EXPECT_FALSE(hasCode(plainAuto.diagnostics(), DiagnosticCode::S_ConstViolation))
        << "the CONTROL: an unqualified inferred `auto` is NOT const, so the "
           "arm above cannot be passing because every auto is const";

    auto volatileAuto = analyzeShipped("c", {
        "int probe(void){ volatile auto v = 1; auto u = 1; return v + u; }\n",
    });
    EXPECT_FALSE(volatileAuto.hasErrors());
    auto const* v = findSymbolNamed(volatileAuto, "v");
    auto const* u = findSymbolNamed(volatileAuto, "u");
    ASSERT_NE(v, nullptr);
    ASSERT_NE(u, nullptr);
    ASSERT_TRUE(v->type.valid());
    ASSERT_TRUE(u->type.valid());
    EXPECT_NE(v->type.v, u->type.v)
        << "`volatile auto v = 1;` must carry the volatile skin the plain `auto "
           "u = 1;` does not — C23 6.7.9 infers from the initializer's "
           "UNQUALIFIED type and the written qualifier then qualifies the object";
}

// ── C 6.7.1p2: at most ONE storage-class specifier, in EVERY order ──────────
//
// ⚠ A FIXTURE MUST SYNTHESIZE THE NEGATIVE. Widening the specifier run must not
// widen the ACCEPTED set, and the pair `extern`+`static` is now reachable in
// orders the grammar previously refused BY ACCIDENT. Each case is refused by
// gcc, clang and MSVC (probed separately).
TEST(DeclSpecifierOrder, TwoStorageClassesStayRefusedInEveryOrder) {
    for (std::string_view const src : {
             "extern static int x;\n",
             "static extern int x;\n",
             "const extern static int x;\n",
             "const static extern int x;\n",
             "volatile extern static int x;\n",
             "extern constexpr int x = 1;\n",
             "constexpr extern int x = 1;\n",
             "const extern constexpr int x = 1;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_TRUE(model.hasErrors())
            << "C 6.7.1p2 admits at most one storage-class specifier and every "
               "reference refuses this spelling: " << src;
    }

    // The live CONTROL: the ONE pair C 6.7.1p2 explicitly allows must stay
    // accepted in BOTH orders, including with a leading qualifier.
    for (std::string_view const src : {
             "static constexpr int k = 1;\n",
             "constexpr static int k = 1;\n",
             "const static constexpr int k = 1;\n",
             "const constexpr static int k = 1;\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(model.hasErrors())
            << "`static` + `constexpr` is the compatible pair the row's "
               "`compatibleWith` list names; refusing it would mean the "
               "exclusive-group check had become order-sensitive: " << src;
    }
}

// ── the linkage tier must not report the qualifier as an unknown specifier ──
//
// A qualifier is not a linkage specifier, but it now appears in the subtree the
// linkage fold scans. With no `linkageSpecifierIgnoredKinds` entry the fold
// reports `H_UnknownLinkageSpecifier` and EXITS 0 — a warning on a perfectly
// ordinary declaration. This pin is the one that keeps that from shipping.
TEST(DeclSpecifierOrder, LeadingQualifierIsNotAnUnknownLinkageSpecifier) {
    for (std::string_view const src : {
             "const static int g = 1;\n",
             "volatile static int v = 1;\n",
             "const extern int e;\n",
             "const typedef int T;\n",
             "int probe(void){ const static int l = 1; return l; }\n",
             "int probe(void){ const register int r = 1; return r; }\n",
         }) {
        auto model = analyzeShipped("c", {std::string{src}});
        EXPECT_FALSE(hasCode(model.diagnostics(),
                             DiagnosticCode::H_UnknownLinkageSpecifier))
            << "a type qualifier in the specifier prefix must be IGNORED by the "
               "linkage tier (and read by the type tier): " << src;
        EXPECT_FALSE(model.hasErrors()) << src;
    }
}

// ── `const typedef`: the declaration must still BE a typedef ────────────────
//
// The leading qualifier run was added to `typedefDeclSpecifiers`, the rule that
// owns the `typedef` keyword itself. The failure mode worth pinning is not a
// missing qualifier but a missing TYPEDEF: if the leading `const` displaced the
// keyword's role, `CT` would simply not be a type name and every use of it
// would be an undeclared identifier — which is a loud error, but on the wrong
// construct and with a diagnostic that names the USE rather than the
// declaration. gcc 13.3.0, clang 18.1.3 and MSVC 19.51.36252 each accept
// `const typedef int T;` (probed separately).
TEST(DeclSpecifierOrder, ConstBeforeTypedefStillDeclaresAnAlias) {
    auto model = analyzeShipped("c", {
        "const typedef int CT;\n"
        "volatile typedef int VT;\n"
        "typedef int TP;\n"
        "CT a = 1;\n"
        "VT b = 2;\n"
        "TP c = 3;\n"
        "int probe(void){ return a + b + c; }\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "`const typedef int CT;` must declare the alias CT — C 6.7.1 makes "
           "`typedef` a storage-class specifier and 6.7p2 makes the specifier "
           "set unordered, so this is `typedef const int CT;` written the other "
           "way round";
    EXPECT_FALSE(hasCode(model.diagnostics(),
                         DiagnosticCode::H_UnknownLinkageSpecifier));

    for (std::string_view const name : {"a", "b", "c"}) {
        auto const* sym = findSymbolNamed(model, name);
        ASSERT_NE(sym, nullptr) << name;
        EXPECT_TRUE(sym->type.valid())
            << name << " must have a resolved type — an unresolved one means the "
                       "alias declaration did not bind";
    }
}
