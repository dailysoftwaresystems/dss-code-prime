// ★★ D-CSUBSET-TYPEDEF-HEAD-DECORATION-TYPE-HIJACK — THE SWEEP.
//
// `resolveTypeNodeImpl` selects a type-position node's head by
// FIRST-CHILD-THAT-RESOLVES-WINS — not by index, not by rule — and its Token arm
// resolves ANY identifier through the scope chain as a possible type alias. So
// ANY identifier a decoration drags into a type-resolved child list becomes a
// candidate head type, and the first one that happens to name a type wins over
// the real head. The program then compiles CLEAN with a wrong-width type.
//
// ⛔ THE TEMPTING WRONG FIX, PINNED HERE SO IT CAN NEVER BE RE-PROPOSED SILENTLY:
// putting the attribute run INSIDE the head rule (`typedefHeadFull`), "where the
// type head already is". It looks right and it fails SILENTLY.
// `WrongFixInsideTheHeadIsRefusedLoudly` below APPLIES that exact edit to the
// SHIPPED document and proves the engine now REFUSES it instead of miscompiling.
//
// ★ THE PINS ASSERT THE RESOLVED TYPE, NEVER "COMPILES CLEAN" — a hijack IS
// clean-compiling, which is the entire defect. Every fixture discriminates
// `long` (I64 under LP64) from `int` (I32): a check that the symbol is merely
// "some integer" passes straight through the very hijack it claims to guard.
//
// ✔REFERENCE GROUND TRUTH (measured 2026-08-29, each reference SEPARATELY):
//   WSL gcc 13.3.0      `gcc -std=c2x -fsyntax-only`  accepts; T == long long, T != int
//   WSL clang 18.1.3    `clang -std=c2x -fsyntax-only` accepts; same
//   mingw-w64 gcc 13.2.0 `gcc -std=c2x -fsyntax-only`  accepts; same
//   MSVC 19.51          `cl /c /std:c17`               refuses `__attribute__`
//                       (not its dialect); its own `__declspec(align(16))`
//                       spelling accepts with T == long long and the decoration
//                       name still naming `int` outside the decoration.
// Three of four references ACCEPT the construct, so under
// `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` DSS must accept it — and all four agree
// the decoration is NOT the head type. Unanimous on MEANING.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

// `long` and `int` are DELIBERATELY the discriminator: under LP64 they are I64
// and I32, so a hijacked head is observable as a KIND, not merely as "an
// integer". Every fixture below declares the decoration's name as `typedef int`
// and the real head as `long`.
constexpr AggregateLayoutParams kLayout{ScalarAlignmentRule::Natural, 16};

[[nodiscard]] SemanticModel analyzeC(std::string src) {
    auto cu = buildShippedUnit("c", {std::move(src)});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   kLayout);
}

[[nodiscard]] SymbolRecord const* sym(SemanticModel const& m,
                                      std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// The ONE assertion this whole file exists to make: `name` resolved to `long`
// (I64), NOT to the `int` typedef whose name the decoration borrows.
void expectResolvedLong(SemanticModel const& m, std::string_view name,
                        char const* what) {
    SymbolRecord const* r = sym(m, name);
    ASSERT_NE(r, nullptr) << what;
    ASSERT_TRUE(r->type.valid()) << what;
    auto const& ti = m.lattice().interner();
    EXPECT_EQ(ti.kind(r->type), TypeKind::I64)
        << what << " — the head resolved to "
        << static_cast<int>(ti.kind(r->type))
        << "; I32 here is the decoration hijacking the head type";
}

// ⚠ WHY THE PINS BELOW DO NOT ALL ASSERT `!hasErrors()`, STATED SO THE WEAKER
// ASSERTION IS NEVER READ AS AN OVERSIGHT. `__attribute__((aligned(N)))` on a
// TYPEDEF draws `S_AlignasInvalidContext` from DSS — "the alias resolves to the
// same type as its aliasee" — in the MID and TRAILING slots. That refusal is
// DELIBERATE, PRE-EXISTING and carried by its own row
// ([[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]] — ⚠ P58: that row is CLOSED as of
// 2026-09-03 and is NO LONGER GATED, so the "⏳ gated" this line used to carry is
// retired; the refusal itself is unchanged and is now the shipped answer for the ISO
// `typedef alignas(16) int T;` spelling too. C11 6.7.5p2 / C23 **6.7.6**p2 — the C23
// number, which this line also had wrong, since C23's 6.7.5 is Function specifiers —
// makes an alignment specifier on a typedef a constraint violation, and gcc, clang,
// mingw-w64 gcc AND MSVC all refuse it, ✔MEASURED 2026-09-03 each
// separately). ✔MEASURED through
// the shipped CLI at `x86_64:elf64-x86_64-linux-exec`, at the pre-change tree
// and after. It is a different question from this row's, so these pins assert
// what THIS row owns — the resolved TYPE, and the absence of the ambiguous-head
// diagnostic — and deliberately do not assert a clean compile where an
// unrelated gated constraint legitimately speaks. The `may_alias` arms below
// carry the clean-compile half: that attribute has no layout sink, so nothing
// else fires and `!hasErrors()` is a real assertion there.
void expectNoAmbiguousHead(SemanticModel const& m, char const* what) {
    EXPECT_FALSE(hasCode(m.diagnostics(),
                         DiagnosticCode::S_InvalidTypeSpecifierCombination))
        << what << " — the ambiguous-head guard fired on a well-formed head";
}

// ── the enumeration, surface by surface ───────────────────────────────────────
//
// Every type position in the shipped c grammar routes through one of the head
// rules below, and each head rule is `{repeat headQualifier} <ONE base slot>
// {repeat headQualifier}` (`headQualifier` = Const/Volatile/Atomic KEYWORDS
// only, never an attribute). The decoration always rides a SIBLING slot. These
// pins assert that discipline where it is observable — through the resolved type.

// SURFACE 1 — `typedefDecl` / `typedefHeadFull`, all THREE decoration slots.
// The row's own case. Slot (1) is the `typedefDeclSpecifiers` prefix, slots (2)
// and (3) the two `typedefAttrRun`s.
TEST(TypeHeadHijackSweep, TypedefHeadSurvivesAllThreeDecorationSlots) {
    struct Case { char const* src; char const* slot; };
    // The row's canonical `aligned` fixture, in all three slots. Type + guard
    // only — see `expectNoAmbiguousHead` for why not `!hasErrors()`.
    for (Case const& c : {
             Case{"typedef int aligned;\n"
                  "typedef __attribute__((aligned(16))) long T;\n"
                  "T v;\n",
                  "slot 1 — after the `typedef` keyword (typedefDeclSpecifiers)"},
             Case{"typedef int aligned;\n"
                  "typedef long __attribute__((aligned(16))) T;\n"
                  "T v;\n",
                  "slot 2 — between head and declarator (typedefAttrRun #1)"},
             Case{"typedef int aligned;\n"
                  "typedef long T __attribute__((aligned(16)));\n"
                  "T v;\n",
                  "slot 3 — after the declarator (typedefAttrRun #2)"},
         }) {
        auto m = analyzeC(c.src);
        expectNoAmbiguousHead(m, c.slot);
        expectResolvedLong(m, "v", c.slot);
    }
    // The same three slots with a decoration that has NO layout sink, so the
    // clean-compile half is asserted too and nothing unrelated can mask a
    // regression.
    for (Case const& c : {
             Case{"typedef int may_alias;\n"
                  "typedef __attribute__((may_alias)) long T;\n"
                  "T v;\n",
                  "slot 1, no layout sink (typedefDeclSpecifiers)"},
             Case{"typedef int may_alias;\n"
                  "typedef long __attribute__((may_alias)) T;\n"
                  "T v;\n",
                  "slot 2, no layout sink (typedefAttrRun #1)"},
             Case{"typedef int may_alias;\n"
                  "typedef long T __attribute__((may_alias));\n"
                  "T v;\n",
                  "slot 3, no layout sink (typedefAttrRun #2)"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors()) << c.slot;
        expectResolvedLong(m, "v", c.slot);
    }
}

// SURFACE 1b — hijack path (b): an ARGUMENT identifier rather than the attribute
// NAME. `aligned`'s argument is a constant, so this uses a two-clause attribute
// whose SECOND clause name is the colliding identifier — the same child list,
// reached from inside the attribute's own argument/clause run.
TEST(TypeHeadHijackSweep, TypedefHeadSurvivesAnArgumentIdentifierCollision) {
    auto m = analyzeC(
        "typedef int may_alias;\n"
        "typedef __attribute__((aligned(16), may_alias)) long T;\n"
        "T v;\n");
    expectResolvedLong(m, "v",
                       "a SECOND attribute clause whose name is a typedef must "
                       "not become the head type either");
}

// SURFACE 2 — `topLevelDecl` / `topLevelHead`: the LEADING slot (`declSpecifiers`)
// and the MID slot (`declAttrRun`), which must agree with each other.
TEST(TypeHeadHijackSweep, TopLevelHeadSurvivesLeadingAndMidDecoration) {
    struct Case { char const* src; char const* slot; };
    for (Case const& c : {
             Case{"typedef int aligned;\n"
                  "__attribute__((aligned(16))) long g;\n",
                  "top level, LEADING (declSpecifiers)"},
             Case{"typedef int aligned;\n"
                  "long __attribute__((aligned(16))) g;\n",
                  "top level, MID (declAttrRun)"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors()) << c.slot;
        expectResolvedLong(m, "g", c.slot);
    }
}

// SURFACE 3 — `varDecl` / `declHead` at BLOCK scope, both slots. The sibling pin
// in test_semantic_analyzer_c.cpp asserts the ALIGNMENT survives here; this one
// asserts the TYPE does, which is the half a hijack corrupts.
TEST(TypeHeadHijackSweep, BlockScopeHeadSurvivesLeadingAndMidDecoration) {
    struct Case { char const* src; char const* slot; };
    for (Case const& c : {
             Case{"typedef int aligned;\n"
                  "int main(void){ __attribute__((aligned(16))) long b = 1; "
                  "return (int)b; }\n",
                  "block scope, LEADING (localDeclSpecifiers)"},
             Case{"typedef int aligned;\n"
                  "int main(void){ long __attribute__((aligned(16))) b = 1; "
                  "return (int)b; }\n",
                  "block scope, MID (declAttrRun)"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors()) << c.slot;
        expectResolvedLong(m, "b", c.slot);
    }
}

// SURFACE 4 — `param` / `declHeadForParam`: `paramDeclSpecifiers` (leading) and
// `paramTrailingAttrRun` (after the declarator).
TEST(TypeHeadHijackSweep, ParamHeadSurvivesItsDecorationSlots) {
    auto m = analyzeC(
        "typedef int aligned;\n"
        "long f(__attribute__((aligned(16))) long p);\n"
        "long f(__attribute__((aligned(16))) long p){ return p; }\n");
    expectResolvedLong(m, "p",
                       "a decorated PARAMETER head must stay `long`");
}

// SURFACE 5 — `structField` / `unionField` / `typeRefAllowingStruct`:
// `structMemberDeclSpecifiers` (leading) and `structMemberAttrList` (trailing).
TEST(TypeHeadHijackSweep, StructMemberHeadSurvivesItsDecorationSlots) {
    struct Case { char const* src; char const* slot; };
    for (Case const& c : {
             Case{"typedef int aligned;\n"
                  "struct S { __attribute__((aligned(16))) long m; };\n",
                  "struct member, LEADING (structMemberDeclSpecifiers)"},
             Case{"typedef int aligned;\n"
                  "struct S { long m __attribute__((aligned(16))); };\n",
                  "struct member, TRAILING (structMemberAttrList)"},
         }) {
        auto m = analyzeC(c.src);
        EXPECT_FALSE(m.hasErrors()) << c.slot;
        expectResolvedLong(m, "m", c.slot);
    }
}

// SURFACE 6 — `externDecl` / `typeRefAllowingStruct`, whose `externSpecifiers`
// prefix admits `attrSpec` directly.
TEST(TypeHeadHijackSweep, ExternHeadSurvivesItsDecorationSlot) {
    auto m = analyzeC(
        "typedef int aligned;\n"
        "extern __attribute__((aligned(16))) long e;\n");
    expectResolvedLong(m, "e",
                       "a decorated EXTERN head must stay `long`");
}

// SURFACE 7 — the `castTypeRef` FAMILY. Cast, sizeof, alignof, typeof, compound
// literal, `_Generic` association, `__builtin_offsetof`,
// `__builtin_types_compatible_p` and `va_arg` ALL take their type through the
// ONE rule `castTypeRef`, so they are ONE surface, not nine. `castTypeRef` has
// NO attribute slot at all (an attribute inside a cast is a parse error), so the
// reachable question is whether a DECORATED typedef still measures correctly
// when used there — i.e. whether the hijack propagates through a type-name.
TEST(TypeHeadHijackSweep, CastTypeRefFamilyMeasuresTheDecoratedTypedef) {
    // `sizeof(long)` (8) vs `sizeof(int)` (4) under LP64 — folded into an array
    // dimension, the established sizeof-folding probe. `may_alias` (not
    // `aligned`) so the whole unit compiles clean and `!hasErrors()` is a real
    // assertion rather than one masked by the alignas-on-typedef refusal.
    // ⓘ P58: that refusal used to be described here as "gated". It is not gated any
    // more — [[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]] closed 2026-09-03 and the
    // refusal is now the SHIPPED answer for the ISO spelling too (`typedef
    // alignas(16) int T;` draws `S_AlignasInvalidContext` rather than a parse error).
    // The reason `may_alias` is used here is UNCHANGED and is the load-bearing half:
    // an `aligned` decoration on a typedef is refused, so it would mask this
    // assertion. Only the word describing the row's status was stale.
    //
    // ⓘ The `_Generic` arm measures `sizeof` of the SELECTED ASSOCIATION'S TYPE
    // rather than a `_Generic` yielding an integer directly: ✔MEASURED through
    // the shipped CLI, a `_Generic` selection does NOT const-fold into a
    // file-scope array dimension (`S_NonConstantArrayLength`), which is a
    // separate const-eval gap and not this row's question.
    auto m = analyzeC(
        "typedef int may_alias;\n"
        "typedef __attribute__((may_alias)) long T;\n"
        "char viaSizeof[sizeof(T)];\n"
        "char viaCast[sizeof((T)0)];\n"
        "char viaGeneric[sizeof(_Generic((T)0, long: (long)0, "
        "default: (char)0))];\n"
        "char viaTypeof[sizeof(typeof(T))];\n"
        "char viaCompound[sizeof((T){0})];\n");
    EXPECT_FALSE(m.hasErrors());
    auto const& ti = m.lattice().interner();
    for (char const* name : {"viaSizeof", "viaCast", "viaGeneric",
                             "viaTypeof", "viaCompound"}) {
        SymbolRecord const* r = sym(m, name);
        ASSERT_NE(r, nullptr) << name;
        ASSERT_TRUE(r->type.valid()) << name;
        ASSERT_EQ(ti.kind(r->type), TypeKind::Array) << name;
        ASSERT_EQ(ti.scalars(r->type).size(), 1u) << name;
        EXPECT_EQ(ti.scalars(r->type)[0], 8)
            << name << " — 4 here means the castTypeRef family measured `int`, "
                       "i.e. the decoration hijacked the typedef's head";
    }
}

// ── the guard: THE TEMPTING WRONG FIX, APPLIED, AND REFUSED ───────────────────

[[nodiscard]] nlohmann::json shippedCJson() {
    std::filesystem::path const path =
        dss::test::configRoot() / "sources" / "c.lang.json";
    if (!std::filesystem::exists(path))
        throw std::runtime_error("shipped c.lang.json is missing: "
                                 + path.string());
    std::ifstream in{path, std::ios::binary};
    if (!in.good())
        throw std::runtime_error("cannot read " + path.string());
    return nlohmann::json::parse(in);
}

// ★★ THE ROW'S WHOLE REASON TO EXIST, AS AN EXECUTABLE PIN.
//
// This APPLIES the wrong fix to the SHIPPED document — it moves the attribute
// run OUT of the `typedefDeclSpecifiers` sibling prefix and INTO
// `typedefHeadFull`, the type-resolved head — and then compiles the canonical
// hijack program. Before the ambiguous-head guard this produced `int` with NO
// diagnostic anywhere. It must now be a LOUD refusal.
//
// A mutant over the SHIPPED document (rather than a synthetic grammar) is
// deliberate: a synthetic fixture would keep passing if the real config ever
// lost the sibling-slot discipline, which is the exact regression this guards.
TEST(TypeHeadHijackSweep, WrongFixInsideTheHeadIsRefusedLoudly) {
    nlohmann::json doc = shippedCJson();

    // Sanity: the shipped document really is in the SAFE shape this mutates
    // away from. If either of these fails, the mutation below is not the wrong
    // fix any more and the pin is measuring nothing.
    ASSERT_TRUE(doc["shapes"].contains("typedefHeadFull"));
    ASSERT_TRUE(doc["shapes"].contains("typedefDeclSpecifiers"));
    ASSERT_EQ(doc["shapes"]["typedefHeadFull"]["sequence"].size(), 3u)
        << "typedefHeadFull is no longer {qualifiers, typedefHead, qualifiers}";

    auto const attrRun = nlohmann::json{
        {"repeat", nlohmann::json{{"alt", {"attrSpec", "stdAttr"}}}}};

    // THE WRONG FIX, in two halves — take the decoration off the sibling slot…
    // ⚠ MOVE ONLY THE POST-KEYWORD RUN. Replacing this rule with a bare
    // ["TypedefKeyword"] also deletes the LEADING run, which stops
    // `__attribute__((x)) typedef ...` PARSING — a red for the wrong reason
    // (P_NoAlternativeMatched in the parser, before any head is resolved).
    // ✔MEASURED: the first version of this mutation did exactly that.
    doc["shapes"]["typedefDeclSpecifiers"] =
        nlohmann::json{{"sequence", {attrRun, "TypedefKeyword"}}};
    // …and put it inside the type-resolved head.
    doc["shapes"]["typedefHeadFull"] = nlohmann::json{
        {"sequence", {attrRun, nlohmann::json{{"repeat", "headQualifier"}},
                      "typedefHead",
                      nlohmann::json{{"repeat", "headQualifier"}}}}};

    auto schema = GrammarSchema::loadFromText(doc.dump(), "<wrong-fix-mutant>");
    ASSERT_TRUE(schema.has_value())
        << "the wrong-fix mutant must LOAD — the point is that it loads and then "
           "miscompiles, which is why the refusal has to come from the resolver";

    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory("typedef int aligned;\n"
                        "typedef __attribute__((aligned(16))) long T;\n"
                        "T v;\n",
                        "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto m = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                     kLayout);

    // The DEFINING assertion: the decoration inside the head must not silently
    // become the head type. Either the head is refused LOUDLY, or — if some
    // future grammar makes this shape unambiguous again — `v` still types as
    // `long`. What must NEVER happen is a clean compile typed `int`.
    SymbolRecord const* v = sym(m, "v");
    bool const refusedLoudly = m.hasErrors();
    bool const stillLong =
        v != nullptr && v->type.valid()
        && m.lattice().interner().kind(v->type) == TypeKind::I64;
    EXPECT_TRUE(refusedLoudly || stillLong)
        << "THE SILENT MISCOMPILE IS BACK: an attribute placed inside "
           "`typedefHeadFull` re-pointed the head at the typedef named "
           "`aligned` and the program compiled clean as `int`. The decoration "
           "must live in a SIBLING slot (the declaration row's "
           "`specifierPrefix`), never inside the type-resolved head.";

    // And the refusal must be the AMBIGUOUS-HEAD diagnostic specifically, not
    // an incidental parse failure that would mask the class if the grammar
    // shifted underneath.
    EXPECT_TRUE(hasCode(m.diagnostics(),
                        DiagnosticCode::S_InvalidTypeSpecifierCombination))
        << "the ambiguous-head guard in `resolveTypeNodeImpl` did not fire; "
           "whatever refused this program was not the type-hijack guard";
}

// CONTROL for the pin above: the SAME mutation shape applied where it changes
// nothing observable (the decoration's name is NOT a typedef). The head is
// unambiguous, so the guard must stay SILENT — a guard that fires here would be
// refusing every decorated typedef in the corpus.
TEST(TypeHeadHijackSweep, WrongFixMutantWithoutACollisionStaysQuiet) {
    nlohmann::json doc = shippedCJson();
    auto const attrRun = nlohmann::json{
        {"repeat", nlohmann::json{{"alt", {"attrSpec", "stdAttr"}}}}};
    // ⚠ MOVE ONLY THE POST-KEYWORD RUN. Replacing this rule with a bare
    // ["TypedefKeyword"] also deletes the LEADING run, which stops
    // `__attribute__((x)) typedef ...` PARSING — a red for the wrong reason
    // (P_NoAlternativeMatched in the parser, before any head is resolved).
    // ✔MEASURED: the first version of this mutation did exactly that.
    doc["shapes"]["typedefDeclSpecifiers"] =
        nlohmann::json{{"sequence", {attrRun, "TypedefKeyword"}}};
    doc["shapes"]["typedefHeadFull"] = nlohmann::json{
        {"sequence", {attrRun, nlohmann::json{{"repeat", "headQualifier"}},
                      "typedefHead",
                      nlohmann::json{{"repeat", "headQualifier"}}}}};

    auto schema = GrammarSchema::loadFromText(doc.dump(), "<wrong-fix-control>");
    ASSERT_TRUE(schema.has_value());
    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    // No `typedef int aligned;` — nothing for the attribute name to collide with.
    builder.addInMemory("typedef __attribute__((aligned(16))) long T;\n"
                        "T v;\n",
                        "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto m = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                     kLayout);
    EXPECT_FALSE(hasCode(m.diagnostics(),
                         DiagnosticCode::S_InvalidTypeSpecifierCombination))
        << "the ambiguous-head guard fired on an UNAMBIGUOUS head — it is "
           "keyed on 'two children resolve to DIFFERENT types', so this is a "
           "false positive that would reject ordinary decorated typedefs";
    expectResolvedLong(m, "v", "the control's head is unambiguous");
}

// ── SECOND TIER: the sibling first-match resolvers ───────────────────────────
//
// The row's closing work says "and any sibling first-match resolver". Three were
// found and all are guarded; these pin the two that are observable in the
// semantic model.
//
// ★ WHY A CONFIG PERTURBATION IS THE RIGHT INSTRUMENT HERE. The second-tier fix
// is "read the DECLARED child index instead of guessing it by ordinal". A pin
// over the shipped config alone cannot tell those apart — the shipped indices
// and the ordinal guess agree, which is exactly why the bug survived. Perturbing
// the config separates them: if the engine reads the field, changing it changes
// the answer; if the engine guesses, the change is INERT and the pin goes red.
// This is the established "prove a config verb is LIVE" idiom.

// The folded length of the first array symbol named `name`, or nullopt when it
// did not fold (the loud-refusal outcome).
[[nodiscard]] std::optional<std::int64_t>
foldedArrayLen(SemanticModel const& m, std::string_view name) {
    SymbolRecord const* r = sym(m, name);
    if (r == nullptr || !r->type.valid()) return std::nullopt;
    auto const& ti = m.lattice().interner();
    if (ti.kind(r->type) != TypeKind::Array) return std::nullopt;
    auto const sc = ti.scalars(r->type);
    if (sc.size() != 1u) return std::nullopt;
    return sc[0];
}

[[nodiscard]] SemanticModel analyzeWithDoc(nlohmann::json const& doc,
                                           std::string src,
                                           char const* label) {
    auto schema = GrammarSchema::loadFromText(doc.dump(), label);
    if (!schema) throw std::runtime_error(std::string{"perturbed schema failed "
                                                      "to load: "} + label);
    UnitBuilder builder{*schema, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   kLayout);
}

// ★★ `subtreeType`'s TRANSPARENT-WRAPPER FALLTHROUGH — the second-tier twin of
// the type-head guard, and the one with the most history behind it.
//
// The fallthrough was FIRST-VALID-CHILD-WINS, and EVERY arm above it (cast,
// sizeof, `_Generic`, ternary, statement-expression, …) was added AFTER the
// fallthrough had already produced a wrong answer for that construct — it was
// OUTRUN, not guarded, so the next construct to reach it repeats the debt.
//
// This pin reproduces that history mechanically: removing `semantics.generic`
// from the SHIPPED document disables the `_Generic` arm and drops `_Generic`
// back onto the fallthrough — the exact state the codebase was in before that
// arm existed. The selection's type is `char` (1); the CONTROLLING expression's
// is `long` (8 under LP64). First-valid-wins answers with the CONTROLLING type.
//
// ✔MEASURED end-to-end through the shipped CLI (the 2x2 transcript): with the
// guard removed this config BUILDS CLEAN and returns the controlling type's
// width; with the guard it refuses. The assertion is therefore "did NOT fold to
// the controlling width", never "compiles clean".
TEST(TypeHeadHijackSweep, WrapperFallthroughRefusesAnAmbiguousTypeInsteadOfGuessing) {
    char const* src =
        "char probe[sizeof(_Generic((long)0, long: (char)0, default: (char)0))];\n";

    // CONTROL: with the shipped config the `_Generic` arm owns the node and the
    // answer is the SELECTION's type — 1, not 8.
    {
        auto m = analyzeC(src);
        EXPECT_FALSE(m.hasErrors());
        auto const len = foldedArrayLen(m, "probe");
        ASSERT_TRUE(len.has_value())
            << "the shipped config must still fold this array length";
        EXPECT_EQ(*len, 1)
            << "the `_Generic` arm must yield the SELECTED association's type "
               "(char), not the controlling expression's (long)";
    }

    // MUTANT: drop the arm; the node falls to the wrapper fallthrough, whose
    // children now type to two DIFFERENT things.
    nlohmann::json doc = shippedCJson();
    ASSERT_TRUE(doc["semantics"].contains("generic"))
        << "the shipped config no longer declares `semantics.generic`, so this "
           "pin is not exercising the fallthrough any more";
    doc["semantics"].erase("generic");

    auto m = analyzeWithDoc(doc, src, "<no-generic-arm>");
    auto const len = foldedArrayLen(m, "probe");
    // THE DEFINING ASSERTION: it must not silently answer with the CONTROLLING
    // expression's width. A refusal (no fold) is the correct outcome; folding to
    // 1 would also be acceptable if some future arm made it unambiguous again.
    // What must never happen is a clean fold to 8.
    EXPECT_NE(len.value_or(-1), 8)
        << "THE SILENT WRONG WIDTH IS BACK: the transparent-wrapper fallthrough "
           "took its FIRST typed child (the `_Generic` controlling expression, "
           "`long`) as the whole construct's type. A wrapper is transparent only "
           "when exactly ONE child carries a type; two disagreeing children mean "
           "the engine is GUESSING, and it must refuse instead.";
    EXPECT_TRUE(m.hasErrors())
        << "an ambiguous wrapper must surface as a loud refusal through the "
           "caller's own positioned diagnostic, never as a silent fold";
}

// ★★ THE DECLARED TYPE-CHILD INDEX IS LIVE — the const-eval cast-target path.
//
// `buildConstEvalEnv`'s `resolveCastTarget` used to take "the first Internal
// child" of a cast node as its type-ref: KIND-ONLY, no rule check, no index. The
// `casts` row has always DECLARED where the type sits (`typeChild`), and Pass 2
// reads that field — so the two tiers agreed only by luck. This pin proves the
// const-eval path now reads the config: point `typeChild` somewhere else and the
// fold must change. Under the OLD code the perturbation is INERT.
TEST(TypeHeadHijackSweep, ConstEvalCastTargetReadsTheDeclaredTypeChild) {
    // `(char)300` truncates to 44 under a `char` target; the array length is the
    // cast's folded value, so the TARGET TYPE is observable as a length.
    char const* src = "char probe[(char)300];\n";

    {
        auto m = analyzeC(src);
        auto const len = foldedArrayLen(m, "probe");
        ASSERT_TRUE(len.has_value()) << "the shipped config must fold this";
        EXPECT_EQ(*len, 44)
            << "(char)300 must fold through the DECLARED cast target `char`";
    }

    // Perturb the declared index to something that is not the type-ref. If the
    // engine still folds to 44, it is not reading the field — it is guessing.
    nlohmann::json doc = shippedCJson();
    ASSERT_TRUE(doc["semantics"].contains("casts"));
    ASSERT_FALSE(doc["semantics"]["casts"].empty());
    ASSERT_EQ(doc["semantics"]["casts"][0]["typeChild"].get<int>(), 1)
        << "the shipped cast row no longer declares typeChild 1; re-derive this "
           "pin against the row as it now stands";
    doc["semantics"]["casts"][0]["typeChild"] = 99;   // out of range

    auto m = analyzeWithDoc(doc, src, "<cast-typechild-perturbed>");
    EXPECT_NE(foldedArrayLen(m, "probe").value_or(-1), 44)
        << "`resolveCastTarget` ignored the perturbed `casts[0].typeChild` and "
           "still found the type by scanning for the first Internal child — the "
           "declared index is not live, so a decoration landing in front of the "
           "type-ref would silently become the cast target";
}

} // namespace
