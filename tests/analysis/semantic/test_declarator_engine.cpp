// FC4 c1 — the C-declarator ENGINE substrate, proven against a SYNTHETIC
// schema (rule names deliberately non-shipped: vdecl/dlist/idecl/dtor/
// player/ddirect/dgroup/fsuf/asuf/plist/parm; shipped grammars untouched).
//
//   M1 — the `semantics.declarators` role block + declarator-mode
//        DeclarationRule fields: loader accept/reject matrix (closed keys,
//        dangling rule/token names, mode-consistency validation).
//   M2 — the SHARED declarator name-extraction walk
//        (core/types/declarator_walk.hpp): nested-group innermost name +
//        abstract-returns-invalid, pinned on hand-built RawTreeBuilder
//        trees (exact shape control, no grammar in the loop).
//   M3 — the declarator-inversion engine (Pass 1 multi-mint + Pass 1.5
//        type fold). THE VERIFIED ALGORITHM: declared(d, T) applies the
//        declarator's pointer stars FIRST (innermost), then folds the
//        direct's suffixes RIGHT-to-LEFT (source-first suffix = OUTERMOST
//        constructor), then binds at the name or recurses into a group
//        with the accumulated type. Pinned by the three canonical cases
//        (`base (*fp)(base)` / `base *fp(base)` / `base x[2][3]`) plus
//        array-of-fn-ptr and multi-declarator multi-symbol minting.
//   M5 — `gatedMarkers` config-driven fail-loud gates (the volatile-wall
//        vocabulary): fires positioned at the marker token; the loader
//        rejects unknown diagnostic-code names.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/raw_tree_builder.hpp"
#include "core/types/declarator_walk.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

// ── the synthetic declarator language ───────────────────────────────────
//
// Surface syntax mirrors C's declarator SHAPES with non-C spellings
// (`base` is the one builtin type; `vol` is the gated marker keyword):
//
//   base x;                  x   : I32
//   base *p, q;              p   : Ptr<I32>,  q : I32
//   base (*fp)(base);        fp  : Ptr<FnSig([I32] -> I32)>
//   base *fp(base);          fp  : FnSig([I32] -> Ptr<I32>)
//   base x[2][3];            x   : Array<2, Array<3, I32>>
//   base (*arr[2])(base);    arr : Array<2, Ptr<FnSig([I32] -> I32)>>
constexpr char kDeclSchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "DeclSynth", "version": "0.0.1", "fileExtensions": [".dsyn"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "*":  [{ "kind": "Star" }],
    ",":  [{ "kind": "Comma" }],
    ";":  [{ "kind": "Semi" }],
    "(":  [{ "kind": "ParenOpen" }],
    ")":  [{ "kind": "ParenClose" }],
    "[":  [{ "kind": "BracketOpen" }],
    "]":  [{ "kind": "BracketClose" }]
  },
  "numberStyle": { "decimal": true, "emitKind": { "integer": "IntLiteral" } },
  "keywords": [
    { "word": "base", "kind": "BaseKw" },
    { "word": "vol",  "kind": "VolKw" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":    { "sequence": [ { "repeat": "vdecl" } ] },
    "vdecl":   { "sequence": [ "heads", "dlist", "Semi" ] },
    "heads":   { "sequence": [ { "optional": "VolKw" }, "BaseKw" ] },
    "dlist":   { "sequence": [ "idecl", { "repeat": { "sequence": [ "Comma", "idecl" ] } } ] },
    "idecl":   { "sequence": [ "dtor" ] },
    "dtor":    { "sequence": [ { "repeat": "player" }, "ddirect" ] },
    "player":  { "sequence": [ "Star" ] },
    "ddirect": { "sequence": [ { "alt": [ "Identifier", "dgroup" ] },
                               { "repeat": { "alt": [ "fsuf", "asuf" ] } } ] },
    "dgroup":  { "sequence": [ "ParenOpen", "dtor", "ParenClose" ] },
    "fsuf":    { "sequence": [ "ParenOpen", { "optional": "plist" }, "ParenClose" ] },
    "asuf":    { "sequence": [ "BracketOpen", "IntLiteral", "BracketClose" ] },
    "plist":   { "sequence": [ "parm", { "repeat": { "sequence": [ "Comma", "parm" ] } } ] },
    "parm":    { "sequence": [ "heads", { "optional": "dtor" } ] }
  },
  "semantics": {
    "identifierToken": "Identifier",
    "declarators": {
      "declaratorRule":     "dtor",
      "pointerLayerRule":   "player",
      "pointerToken":       "Star",
      "directRule":         "ddirect",
      "groupRule":          "dgroup",
      "nameToken":          "Identifier",
      "fnSuffixRule":       "fsuf",
      "fnSuffixParamsRule": "plist",
      "arraySuffixRule":    "asuf",
      "initDeclaratorRule": "idecl",
      "listRule":           "dlist"
    },
    "declarations": [
      { "rule": "vdecl", "head": 0, "declaratorList": 1, "kind": "variable",
        "gatedMarkers": [ { "token": "VolKw", "code": "S_VolatileNotSupported" } ] },
      { "rule": "parm",  "head": 0, "declarator": 1, "kind": "variable" }
    ],
    "builtinTypes": [ { "name": "base", "core": "I32" } ],
    "literalTypes": [ { "literal": "IntLiteral", "core": "I32" } ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<GrammarSchema const> loadDeclSchema() {
    auto loaded = GrammarSchema::loadFromText(kDeclSchemaText, "<decl-synth>");
    if (!loaded) {
        ADD_FAILURE() << "synthetic declarator schema failed to load";
        std::abort();
    }
    return *loaded;
}

[[nodiscard]] std::shared_ptr<CompilationUnit const>
buildDeclCu(std::string source) {
    auto schema = loadDeclSchema();
    UnitBuilder builder{schema};
    builder.addInMemory(std::move(source), "<decl-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}

[[nodiscard]] SymbolId symbolNamed(SemanticModel const& m,
                                   std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i) {
        if (m.symbols()[i].name == name) {
            return SymbolId{static_cast<std::uint32_t>(i)};
        }
    }
    return InvalidSymbol;
}

[[nodiscard]] TypeId typeOf(SemanticModel const& m, std::string_view name) {
    SymbolId const sym = symbolNamed(m, name);
    if (!sym.valid()) {
        ADD_FAILURE() << "no symbol named '" << name << "'";
        return InvalidType;
    }
    return m.symbols()[sym.v].type;
}

// Loader-matrix helper: perturb the BASE schema doc and report whether the
// perturbed text still loads (the FC3 width-semantics pattern).
[[nodiscard]] bool schemaLoads(nlohmann::json const& doc) {
    auto r = GrammarSchema::loadFromText(doc.dump(), "<decl-perturbed>");
    return r.has_value();
}

[[nodiscard]] nlohmann::json baseDoc() {
    return nlohmann::json::parse(kDeclSchemaText);
}

} // namespace

// ── M1: loader accept/reject matrix ─────────────────────────────────────

TEST(DeclaratorConfigLoader, BaseSchemaLoadsAndExposesResolvedRoles) {
    auto schema = loadDeclSchema();
    auto const& sem = schema->semantics();
    ASSERT_TRUE(sem.declarators.has_value());
    auto const& dc = *sem.declarators;
    // Every role resolved to the DECLARED rule/token (id-level identity —
    // not just "valid"), so the engine consults exactly the configured
    // vocabulary.
    EXPECT_EQ(dc.declaratorRule.v,     schema->rules().find("dtor").v);
    EXPECT_EQ(dc.pointerLayerRule.v,   schema->rules().find("player").v);
    EXPECT_EQ(dc.directRule.v,         schema->rules().find("ddirect").v);
    EXPECT_EQ(dc.groupRule.v,          schema->rules().find("dgroup").v);
    EXPECT_EQ(dc.fnSuffixRule.v,       schema->rules().find("fsuf").v);
    EXPECT_EQ(dc.arraySuffixRule.v,    schema->rules().find("asuf").v);
    EXPECT_EQ(dc.initDeclaratorRule.v, schema->rules().find("idecl").v);
    EXPECT_EQ(dc.listRule.v,           schema->rules().find("dlist").v);
    ASSERT_TRUE(dc.fnSuffixParamsRule.has_value());
    EXPECT_EQ(dc.fnSuffixParamsRule->v, schema->rules().find("plist").v);
    EXPECT_TRUE(dc.pointerToken.valid());
    EXPECT_TRUE(dc.nameToken.valid());
    // The two rows landed in declarator mode.
    ASSERT_EQ(sem.declarations.size(), 2u);
    EXPECT_TRUE(sem.declarations[0].isDeclaratorMode());
    EXPECT_TRUE(sem.declarations[1].isDeclaratorMode());
    ASSERT_EQ(sem.declarations[0].gatedMarkers.size(), 1u);
    EXPECT_EQ(sem.declarations[0].gatedMarkers[0].code,
              DiagnosticCode::S_VolatileNotSupported);
}

TEST(DeclaratorConfigLoader, UnknownKeyInBlockRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarators"]["pointerLayerRulee"] = "player";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, MissingRequiredRoleRejects) {
    for (char const* role : {"declaratorRule", "pointerLayerRule",
                             "pointerToken", "directRule", "groupRule",
                             "nameToken", "fnSuffixRule", "arraySuffixRule",
                             "initDeclaratorRule", "listRule"}) {
        auto doc = baseDoc();
        doc["semantics"]["declarators"].erase(role);
        EXPECT_FALSE(schemaLoads(doc)) << "missing '" << role
                                       << "' must reject";
    }
}

TEST(DeclaratorConfigLoader, OptionalFnSuffixParamsRuleMayBeAbsent) {
    auto doc = baseDoc();
    doc["semantics"]["declarators"].erase("fnSuffixParamsRule");
    EXPECT_TRUE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, DanglingRuleNameRejects) {
    for (char const* role : {"declaratorRule", "directRule", "groupRule",
                             "fnSuffixRule", "fnSuffixParamsRule",
                             "arraySuffixRule", "initDeclaratorRule",
                             "listRule", "pointerLayerRule"}) {
        auto doc = baseDoc();
        doc["semantics"]["declarators"][role] = "noSuchRule";
        EXPECT_FALSE(schemaLoads(doc)) << "dangling '" << role
                                       << "' must reject";
    }
}

TEST(DeclaratorConfigLoader, DanglingTokenNameRejects) {
    for (char const* role : {"pointerToken", "nameToken"}) {
        auto doc = baseDoc();
        doc["semantics"]["declarators"][role] = "NoSuchToken";
        EXPECT_FALSE(schemaLoads(doc)) << "dangling '" << role
                                       << "' must reject";
    }
}

TEST(DeclaratorConfigLoader, DeclaratorModeRowWithoutBlockRejects) {
    auto doc = baseDoc();
    doc["semantics"].erase("declarators");
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, MixingLegacyNameWithDeclaratorModeRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0]["name"] = 0;
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, MixingLegacyTypeWithDeclaratorModeRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][1]["type"] = 0;
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, BothListAndSingleDeclaratorRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0]["declarator"] = 1;
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, HeadOnlyRowRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0].erase("declaratorList");
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(DeclaratorConfigLoader, DeclaratorModeWithoutHeadRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0].erase("head");
    EXPECT_FALSE(schemaLoads(doc));
}

// ── M5 loader arm: gatedMarkers validation ──────────────────────────────

TEST(GatedMarkerLoader, UnknownDiagnosticCodeNameRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0]["gatedMarkers"][0]["code"] =
        "S_NoSuchDiagnostic";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(GatedMarkerLoader, SentinelCodeNamesReject) {
    for (char const* sentinel : {"None", "Unknown"}) {
        auto doc = baseDoc();
        doc["semantics"]["declarations"][0]["gatedMarkers"][0]["code"] =
            sentinel;
        EXPECT_FALSE(schemaLoads(doc)) << sentinel;
    }
}

TEST(GatedMarkerLoader, UnknownTokenNameRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0]["gatedMarkers"][0]["token"] =
        "NoSuchKw";
    EXPECT_FALSE(schemaLoads(doc));
}

TEST(GatedMarkerLoader, UnknownEntryKeyRejects) {
    auto doc = baseDoc();
    doc["semantics"]["declarations"][0]["gatedMarkers"][0]["tokn"] = "VolKw";
    EXPECT_FALSE(schemaLoads(doc));
}

// ── M2: the shared name-extraction walk (hand-built trees) ──────────────
//
// RawTreeBuilder gives exact shape control with ZERO grammar in the loop:
// the walk's contract is over the config-resolved ROLES, so the test
// interns its own rules and hand-assembles the declarator shapes —
// including the abstract form the synthetic grammar deliberately cannot
// produce (ddirect requires a base there).

namespace {

struct WalkFixture {
    tests::RawTreeBuilder rb;
    DeclaratorConfig      dc;
    RuleId rDtor, rPlayer, rDirect, rGroup, rFsuf, rAsuf, rIdecl, rDlist;
    SchemaTokenId const tokName{1};
    SchemaTokenId const tokStar{2};
    SchemaTokenId const tokOther{3};   // parens/brackets/commas — structure

    explicit WalkFixture(std::string source)
        : rb{std::move(source), "<walk>"} {
        rDtor   = rb.internRule("w_dtor");
        rPlayer = rb.internRule("w_player");
        rDirect = rb.internRule("w_direct");
        rGroup  = rb.internRule("w_group");
        rFsuf   = rb.internRule("w_fsuf");
        rAsuf   = rb.internRule("w_asuf");
        rIdecl  = rb.internRule("w_idecl");
        rDlist  = rb.internRule("w_dlist");
        dc.declaratorRule     = rDtor;
        dc.pointerLayerRule   = rPlayer;
        dc.pointerToken       = tokStar;
        dc.directRule         = rDirect;
        dc.groupRule          = rGroup;
        dc.nameToken          = tokName;
        dc.fnSuffixRule       = rFsuf;
        dc.arraySuffixRule    = rAsuf;
        dc.initDeclaratorRule = rIdecl;
        dc.listRule           = rDlist;
    }

    [[nodiscard]] NodeId tok(SchemaTokenId kind, std::uint32_t at = 0) {
        return rb.addNode(NodeKind::Token, RuleId{}, SourceSpan::of(at, at + 1),
                          NodeFlags::None, InvalidNode, {}, kind);
    }
    [[nodiscard]] NodeId node(RuleId rule, std::vector<NodeId> kids) {
        return rb.addNode(NodeKind::Internal, rule, SourceSpan::of(0, 1),
                          NodeFlags::None, InvalidNode, std::move(kids));
    }
};

} // namespace

// `(*(*x)(b))(c)` — the name nests TWO groups deep; the walk must descend
// group -> declarator -> direct -> group -> declarator -> direct -> name.
TEST(DeclaratorWalk, NestedGroupFindsInnermostName) {
    WalkFixture f{"(*(*x)(b))(c)"};
    NodeId const name    = f.tok(f.tokName, 4);   // "x"
    NodeId const direct2 = f.node(f.rDirect, {name});
    NodeId const dtor2   = f.node(f.rDtor, {f.node(f.rPlayer, {f.tok(f.tokStar)}),
                                            direct2});
    NodeId const group1  = f.node(f.rGroup, {f.tok(f.tokOther), dtor2,
                                             f.tok(f.tokOther)});
    NodeId const fsufB   = f.node(f.rFsuf, {f.tok(f.tokOther), f.tok(f.tokOther)});
    NodeId const direct1 = f.node(f.rDirect, {group1, fsufB});
    NodeId const dtor1   = f.node(f.rDtor, {f.node(f.rPlayer, {f.tok(f.tokStar)}),
                                            direct1});
    NodeId const group0  = f.node(f.rGroup, {f.tok(f.tokOther), dtor1,
                                             f.tok(f.tokOther)});
    NodeId const fsufC   = f.node(f.rFsuf, {f.tok(f.tokOther), f.tok(f.tokOther)});
    NodeId const direct0 = f.node(f.rDirect, {group0, fsufC});
    NodeId const dtor0   = f.node(f.rDtor, {direct0});
    NodeId const root    = f.node(f.rDtor, {dtor0});   // dummy holder
    Tree const tree      = std::move(f.rb).finish(root);

    NodeId const found = declaratorNameNode(tree, dtor0, f.dc);
    ASSERT_TRUE(found.valid());
    EXPECT_EQ(found.v, name.v);
    EXPECT_EQ(tree.text(found), "x");
}

// An ABSTRACT declarator (direct has suffixes but neither name nor group)
// legally yields InvalidNode — never an error, never a wrong node.
TEST(DeclaratorWalk, AbstractDeclaratorReturnsInvalid) {
    WalkFixture f{"*[2]"};
    NodeId const asuf   = f.node(f.rAsuf, {f.tok(f.tokOther), f.tok(f.tokOther)});
    NodeId const direct = f.node(f.rDirect, {asuf});
    NodeId const dtor   = f.node(f.rDtor, {f.node(f.rPlayer, {f.tok(f.tokStar)}),
                                           direct});
    Tree const tree     = std::move(f.rb).finish(dtor);
    EXPECT_FALSE(declaratorNameNode(tree, dtor, f.dc).valid());
}

// initDeclarator unwraps to its declarator; a list collects every
// initDeclarator/declarator child in source order (commas skipped).
TEST(DeclaratorWalk, ListCollectsEachDeclaratorInOrder) {
    WalkFixture f{"a,b"};
    NodeId const nameA   = f.tok(f.tokName, 0);
    NodeId const nameB   = f.tok(f.tokName, 2);
    NodeId const ideclA  = f.node(f.rIdecl,
                                  {f.node(f.rDtor, {f.node(f.rDirect, {nameA})})});
    NodeId const ideclB  = f.node(f.rIdecl,
                                  {f.node(f.rDtor, {f.node(f.rDirect, {nameB})})});
    NodeId const list    = f.node(f.rDlist, {ideclA, f.tok(f.tokOther), ideclB});
    Tree const tree      = std::move(f.rb).finish(list);

    std::vector<NodeId> out;
    collectDeclarators(tree, list, f.dc, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].v, ideclA.v);
    EXPECT_EQ(out[1].v, ideclB.v);
    EXPECT_EQ(declaratorNameNode(tree, out[0], f.dc).v, nameA.v);
    EXPECT_EQ(declaratorNameNode(tree, out[1], f.dc).v, nameB.v);
}

// A node outside the declarator role shapes collects nothing and names
// nothing (the structurally-absent / errored-decl degrade direction).
TEST(DeclaratorWalk, NonDeclaratorShapeYieldsNothing) {
    WalkFixture f{"?"};
    NodeId const stray = f.node(f.rFsuf, {f.tok(f.tokOther)});
    Tree const tree    = std::move(f.rb).finish(stray);
    EXPECT_FALSE(declaratorNameNode(tree, stray, f.dc).valid());
    std::vector<NodeId> out;
    collectDeclarators(tree, stray, f.dc, out);
    EXPECT_TRUE(out.empty());
}

// ── M3: the inversion engine (full parse + analyze pipeline) ────────────

namespace {

[[nodiscard]] SemanticModel analyzeDecl(std::string source) {
    auto cu = buildDeclCu(std::move(source));
    assertNoBuilderErrors(*cu);
    return analyze(cu);
}

} // namespace

TEST(DeclaratorInversion, PlainNameDeclaresHeadType) {
    auto m = analyzeDecl("base x;");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "x");
    ASSERT_TRUE(t.valid());
    EXPECT_EQ(in.kind(t), TypeKind::I32);
}

// Canonical case 1: `base (*fp)(base)` — POINTER TO FUNCTION. The fn
// suffix applies to the GROUP before the descent; the inner star then
// wraps the FnSig.
TEST(DeclaratorInversion, PointerToFunction) {
    auto m = analyzeDecl("base (*fp)(base);");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "fp");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Ptr);
    TypeId const fn = in.operands(t)[0];
    ASSERT_EQ(in.kind(fn), TypeKind::FnSig);
    auto const ops = in.operands(fn);   // [result, params...]
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(in.kind(ops[0]), TypeKind::I32);   // result
    EXPECT_EQ(in.kind(ops[1]), TypeKind::I32);   // the one param
}

// Canonical case 2: `base *fp(base)` — FUNCTION RETURNING POINTER. The
// stars bind FIRST (innermost), the fn suffix wraps the star-applied type.
TEST(DeclaratorInversion, FunctionReturningPointer) {
    auto m = analyzeDecl("base *fp(base);");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "fp");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::FnSig);
    auto const ops = in.operands(t);
    ASSERT_EQ(ops.size(), 2u);
    ASSERT_EQ(in.kind(ops[0]), TypeKind::Ptr);   // result = Ptr<I32>
    EXPECT_EQ(in.kind(in.operands(ops[0])[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(ops[1]), TypeKind::I32);   // param
}

// Canonical case 3: `base x[2][3]` — the FIRST suffix is the OUTER array:
// Array<2, Array<3, I32>> (the right-to-left suffix fold).
TEST(DeclaratorInversion, MultiDimArrayOuterFirst) {
    auto m = analyzeDecl("base x[2][3];");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "x");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Array);
    EXPECT_EQ(in.scalars(t)[0], 2);
    TypeId const innerArr = in.operands(t)[0];
    ASSERT_EQ(in.kind(innerArr), TypeKind::Array);
    EXPECT_EQ(in.scalars(innerArr)[0], 3);
    EXPECT_EQ(in.kind(in.operands(innerArr)[0]), TypeKind::I32);
}

// Array of pointers: `base *x[3]` — stars first, then the suffix:
// Array<3, Ptr<I32>> (NOT Ptr<Array<...>>).
TEST(DeclaratorInversion, ArrayOfPointers) {
    auto m = analyzeDecl("base *x[3];");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "x");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Array);
    EXPECT_EQ(in.scalars(t)[0], 3);
    TypeId const elem = in.operands(t)[0];
    ASSERT_EQ(in.kind(elem), TypeKind::Ptr);
    EXPECT_EQ(in.kind(in.operands(elem)[0]), TypeKind::I32);
}

// `base (*arr[2])(base)` — ARRAY OF POINTER-TO-FUNCTION: the group's
// suffix folds before the descent; inside, the array suffix wraps the
// star-applied... no: the OUTER direct is the group + fn suffix; the
// INNER declarator is `*arr[2]` whose direct carries the array suffix.
// Result: Array<2, Ptr<FnSig([I32] -> I32)>>.
TEST(DeclaratorInversion, ArrayOfFunctionPointers) {
    auto m = analyzeDecl("base (*arr[2])(base);");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "arr");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Array);
    EXPECT_EQ(in.scalars(t)[0], 2);
    TypeId const elem = in.operands(t)[0];
    ASSERT_EQ(in.kind(elem), TypeKind::Ptr);
    TypeId const fn = in.operands(elem)[0];
    ASSERT_EQ(in.kind(fn), TypeKind::FnSig);
    auto const ops = in.operands(fn);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(in.kind(ops[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(ops[1]), TypeKind::I32);
}

// Multi-declarator multi-symbol minting: `base *p, q;` — TWO symbols off
// ONE declaration node, with DIFFERENT types (the head is shared; each
// declarator owns its own type structure).
TEST(DeclaratorInversion, MultiDeclaratorMintsDistinctTypes) {
    auto m = analyzeDecl("base *p, q;");
    auto const& in = m.lattice().interner();
    ASSERT_EQ(m.symbols().size() - 1, 2u);
    TypeId const tp = typeOf(m, "p");
    TypeId const tq = typeOf(m, "q");
    ASSERT_TRUE(tp.valid());
    ASSERT_TRUE(tq.valid());
    ASSERT_EQ(in.kind(tp), TypeKind::Ptr);
    EXPECT_EQ(in.kind(in.operands(tp)[0]), TypeKind::I32);
    EXPECT_EQ(in.kind(tq), TypeKind::I32);
    EXPECT_NE(tp.v, tq.v);
}

// The nested-group form end-to-end (the M2 walk under the full pipeline):
// `base (*(*x)(base))(base)` — x : Ptr<FnSig([I32] ->
// Ptr<FnSig([I32] -> I32)>)>.
TEST(DeclaratorInversion, NestedGroupTypesInnermostName) {
    auto m = analyzeDecl("base (*(*x)(base))(base);");
    auto const& in = m.lattice().interner();
    TypeId const t = typeOf(m, "x");
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(in.kind(t), TypeKind::Ptr);
    TypeId const outerFn = in.operands(t)[0];
    ASSERT_EQ(in.kind(outerFn), TypeKind::FnSig);
    auto const outerOps = in.operands(outerFn);
    ASSERT_EQ(outerOps.size(), 2u);
    ASSERT_EQ(in.kind(outerOps[0]), TypeKind::Ptr);   // result: Ptr<FnSig>
    TypeId const innerFn = in.operands(outerOps[0])[0];
    ASSERT_EQ(in.kind(innerFn), TypeKind::FnSig);
    EXPECT_EQ(in.kind(in.operands(innerFn)[0]), TypeKind::I32);
}

// A param with its OWN declarator: `base *g(base *p)` — the fn-suffix
// harvest resolves the param row through the SAME machinery (head +
// declarator fold), and the param's own symbol is typed too.
TEST(DeclaratorInversion, ParamWithDeclaratorHarvestsItsFoldedType) {
    auto m = analyzeDecl("base *g(base *p);");
    auto const& in = m.lattice().interner();
    TypeId const tg = typeOf(m, "g");
    ASSERT_TRUE(tg.valid());
    ASSERT_EQ(in.kind(tg), TypeKind::FnSig);
    auto const ops = in.operands(tg);
    ASSERT_EQ(ops.size(), 2u);
    ASSERT_EQ(in.kind(ops[0]), TypeKind::Ptr);   // result Ptr<I32>
    ASSERT_EQ(in.kind(ops[1]), TypeKind::Ptr);   // param  Ptr<I32>
    // The named param minted its own symbol with the same folded type.
    TypeId const tp = typeOf(m, "p");
    ASSERT_TRUE(tp.valid());
    EXPECT_EQ(tp.v, ops[1].v);
}

// Abstract param (`base` with no declarator at all) still contributes its
// head type to the signature — and mints NO symbol.
TEST(DeclaratorInversion, AbstractParamContributesTypeMintsNoSymbol) {
    auto m = analyzeDecl("base (*fp)(base);");
    EXPECT_EQ(m.symbols().size() - 1, 1u);   // fp only
}

// Per-C 6.7.5.2 validation rides the declarator array suffix exactly like
// the legacy facet: a non-positive length fails loud, the symbol's type
// stays unresolved.
TEST(DeclaratorInversion, ZeroArrayLengthFailsLoud) {
    auto cu = buildDeclCu("base x[0];");
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_ArrayLengthOutOfRange), 1u);
    EXPECT_FALSE(typeOf(m, "x").valid());
}

// Two declarators of the same name in one list — the per-declarator bind
// path reports the redeclaration exactly once.
TEST(DeclaratorInversion, SameNameTwiceInOneListRedeclares) {
    auto cu = buildDeclCu("base x, x;");
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu);
    EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_RedeclaredSymbol),
              1u);
}

// ── M5: gated markers fire positioned ───────────────────────────────────

TEST(GatedMarkers, MarkerTokenFiresDeclaredCodeAtTokenSpan) {
    auto cu = buildDeclCu("vol base x;");
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu);
    ASSERT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_VolatileNotSupported), 1u);
    for (auto const& d : m.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_VolatileNotSupported) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
        // Positioned AT the marker token itself.
        EXPECT_EQ(cu->trees()[0].source().slice(d.span), "vol");
    }
    // The gate is a WALL, not a typing change: the head still resolves and
    // the symbol still types (the compile fails on the error count).
    auto const& in = m.lattice().interner();
    EXPECT_EQ(in.kind(typeOf(m, "x")), TypeKind::I32);
}

TEST(GatedMarkers, AbsentMarkerFiresNothing) {
    auto cu = buildDeclCu("base x;");
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu);
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_VolatileNotSupported), 0u);
}

TEST(GatedMarkers, OneDiagnosticPerDeclarationEvenWithListDecl) {
    auto cu = buildDeclCu("vol base x, y;\nvol base z;");
    assertNoBuilderErrors(*cu);
    auto m = analyze(cu);
    // One per DECLARATION (two declarations carry `vol`), not per symbol.
    EXPECT_EQ(countCode(m.diagnostics(),
                        DiagnosticCode::S_VolatileNotSupported), 2u);
}
