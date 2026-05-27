// HR8/HR9 genericity proof (plan 09 §2.1): the CST→HIR engine lowers a SYNTHETIC,
// never-shipped, never-special-cased language — defined entirely by a `.lang.json`
// loaded from a string here — purely from its `semantics` + `hirLowering` config.
// The engine has no `schema.name()` branch, so a language it never saw still
// lowers. This is the "no language assumptions leaked" guarantee made concrete.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

using namespace dss;

namespace {

// A tiny imperative language unlike any shipped one: `let NAME : int = EXPR ;`
// top-level globals, integer literals, `+`, identifier references. Enough to
// exercise Module / Global / typed decl / BinaryOp / Literal / Ref lowering.
constexpr char const* kSynthSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synthetic", "version": "0.0.1", "fileExtensions": [".syn"] },
  "artifactProfiles": ["cli"],

  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "(":  [{ "kind": "LParen", "opensScope": "Paren" }],
    ")":  [{ "kind": "RParen", "closesScope": true   }],
    ":":  [{ "kind": "Colon" }],
    ";":  [{ "kind": "Semi"  }],
    "=":  [{ "kind": "Assign" }],
    "+":  [{ "kind": "Plus"   }]
  },

  "numberStyle": { "decimal": true, "emitKind": { "integer": "IntLiteral" } },

  "syncTokens": ["Semi"],

  "keywords": [
    { "word": "let", "kind": "LetKw" },
    { "word": "int", "kind": "IntKw" }
  ],

  "shapes": {
    "root":     { "sequence": [ { "repeat": "decl" } ] },
    "decl":     { "sequence": [ "LetKw", "Identifier", "Colon", "typeRef", "Assign", "expression", "Semi" ] },
    "typeBase": { "alt": [ "IntKw" ] },
    "typeRef":  { "sequence": [ "typeBase" ] },
    "expression": {
      "expr": {
        "atom": "operand",
        "wrapperRules": { "binary": "binExpr", "unary": "unExpr", "postfix": "postExpr" }
      }
    },
    "operand": { "alt": [ "Identifier", "IntLiteral", { "sequence": [ "LParen", "expression", "RParen" ] } ] }
  },

  "operators": {
    "groups": [ { "precedence": 65, "associativity": "left", "operators": ["+"] } ]
  },

  "semantics": {
    "identifierToken": "Identifier",
    "declarations": [ { "rule": "decl", "name": 1, "type": 3, "init": 5, "kind": "variable" } ],
    "references":   [ { "rule": "operand" } ],
    "builtinTypes": [ { "name": "int", "core": "I32" } ],
    "literalTypes": [ { "literal": "IntLiteral", "core": "I32" } ]
  },

  "hirLowering": {
    "ruleMappings": [ { "rule": "decl", "hirKind": "Decl" } ],
    "binaryExprRule":  "binExpr",
    "unaryExprRule":   "unExpr",
    "postfixExprRule": "postExpr",
    "operandRule":     "operand",
    "binaryOps": [ { "token": "Plus", "target": "Add" } ]
  }
})JSON";

} // namespace

TEST(HirLoweringGeneric, SyntheticLanguageLowersWithoutNameDependence) {
    auto loaded = GrammarSchema::loadFromText(kSynthSchema);
    ASSERT_TRUE(loaded) << "synthetic schema failed to load";

    UnitBuilder builder{*loaded};
    builder.addInMemory("let a : int = 1 + 2;\nlet b : int = a + 3;\n", "<syn>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu);
    ASSERT_FALSE(model.hasErrors()) << "synthetic program failed semantic analysis";

    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    ASSERT_TRUE(res->ok) << "synthetic language did not lower/verify cleanly"
                         << (r.all().empty() ? "" : ("\n" + r.all()[0].actual));

    // Module of two Globals; the second's initializer is `a + 3` (a BinaryOp).
    HirNodeId root = res->hir.root();
    ASSERT_EQ(res->hir.kind(root), HirKind::Module);
    auto decls = res->hir.moduleDecls(root);
    ASSERT_EQ(decls.size(), 2u);
    EXPECT_EQ(res->hir.kind(decls[0]), HirKind::Global);
    EXPECT_EQ(res->hir.kind(decls[1]), HirKind::Global);
    HirNodeId initB = *res->hir.globalInit(decls[1]);
    EXPECT_EQ(res->hir.kind(initB), HirKind::BinaryOp);
    // Two integer literals were decoded into the pool (1, 2) plus the `3`.
    EXPECT_GE(res->literalPool.size(), 3u);
}
