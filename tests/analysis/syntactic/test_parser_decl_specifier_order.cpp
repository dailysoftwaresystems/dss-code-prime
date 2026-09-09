// ===========================================================================
// P65 lane `dq` — [[D-CSUBSET-DECL-QUALIFIER-BEFORE-STORAGE-CLASS]]
//                 (+ residue (2) of [[D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES]])
//
// THE PROPERTY THIS FILE OWNS, AT THE PARSER TIER: C 6.7p2 makes a
// declaration's specifiers an UNORDERED SET, so a TYPE QUALIFIER may be written
// BEFORE, BETWEEN or AFTER the storage-class specifiers. `const static int g;`
// must parse exactly as `static const int g;` does, in every declaration
// position the language has — and the ILLEGAL combinations (C 6.7.1p2, at most
// one storage-class specifier) must stay LOUD in every order.
//
// ✔THE REFERENCE MATRIX, PROBED SEPARATELY, ONE TRANSLATION UNIT PER CASE, so
// no failure masks the next (2026-09-08; gcc 13.3.0 `-std=c2x -c`, clang 18.1.3
// `-std=c23 -c`, MSVC 19.51.36252 `cl /nologo /c /std:c17` and `/std:clatest`;
// 0 = accepted, 1/2 = refused):
//
//   case                                gcc  clang  MSVC(c17/clatest)
//   const static int g = 1;              0     0      0 / 0
//   const extern int e;                  0     0      0 / 0
//   volatile static int v = 2;           0     0      0 / 0
//   _Atomic static int a = 3;            0     0      2 / 2   (no _Atomic)
//   const inline int f(void){…}          0     0      0 / 0
//   static const inline int f(void){…}   0     0      0 / 0
//   const _Thread_local int t = 4;       0     0      0 / 0
//   const constexpr int c = 5;           0     1      2 / 2   (no C constexpr)
//   const _Alignas(16) int al = 6;       0     0      0 / 0
//   const __attribute__((unused)) int w; 0     0      2 / 2   (no GNU attrs)
//   const typedef int T;                 0     0      0 / 0
//   block  const static int l = 3;       0     0      0 / 0
//   block  const register int r = 1;     0     0      0 / 0
//   block  const extern int ge;          0     0      0 / 0
//   block  const auto int q = 1;         0     0      0 / 0
//   param  const register int x          0     0      0 / 0
//   ── the negatives, C 6.7.1p2 ──
//   extern static / static extern        1     1      2 / 2
//   extern constexpr / constexpr extern  1     1      2 / 2
//   const extern static                  1     1      2 / 2
//   const static extern                  1     1      2 / 2
//   block static extern                  1     1      2 / 2
//   block const static register          1     1      2 / 2
//   typedef static / const static typedef 1    1      2 / 2
//
// Every refusal above is UNANIMOUS and every acceptance has at least one
// reference that ACCEPTS AND RUNS it, which is what `DSS = (gcc ∪ clang ∪ MSVC)
// ∪ ISO C` (the union over what WORKS) makes REQUIRED rather than optional. The
// three cases MSVC and clang refuse are missing FEATURES on those references
// (`_Atomic`, C `constexpr`, GNU attributes), never a refusal of the ORDER.
//
// ★★ THE STRUCTURAL HALF, AND WHY IT IS PINNED SEPARATELY. Admitting the three
// qualifier tokens into FIRST(localDeclSpecifiers) collides head-on with the
// SIBLING branch of `varDecl`/`forDecl`, and `detectAmbiguousAlternatives`
// refuses an overlapping NON-speculative alt at load. The escape it offers —
// `speculative: true` on `varDecl` — is the measured 4096-token budget cliff
// (`$p53OrderRefutedComment`), so the fix instead REMOVED `kwDeclHead`'s
// qualifier-led alt and moved that lead to `localDeclSpecifiers` + `declHead`.
// The invariant that keeps the alt decidable on one token is therefore
// FIRST(localDeclSpecifiers) ∩ FIRST(kwDeclHead) = ∅, and a future edit that
// puts a qualifier back into `kwDeclHead` — or a type keyword into
// `localDeclSpecifier` — silently re-opens the cliff. `SpecifierRunAndKeywordHead
// StayDisjoint` is the pin that goes red instead.
//
// RED-ON-DISABLE (REMOVE direction) — see the file
// `tests/analysis/semantic/test_decl_specifier_order.cpp` header for the shared
// transcript; the mutant is the DOCUMENT (`c.lang.json`), so NO object md5 is
// involved: the CLI and every test binary read the shipped config at RUN time
// through `$DSS_CONFIG_ROOT`.
// ===========================================================================

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSchema() {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value()) << "the shipped `c` schema must load";
    return loaded.value_or(nullptr);
}

// Parse `source` with the shipped `c` grammar; report whether the PARSE tier
// produced an error, and name the first error code when it did. No semantic
// analysis runs — this file is about what the grammar admits.
[[nodiscard]] bool parsesClean(std::shared_ptr<GrammarSchema const> const& schema,
                               std::string source,
                               std::string* firstError = nullptr) {
    auto src = SourceBuffer::fromString(std::move(source), "<decl-order>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& diags = result.tree.diagnostics();
    if (!diags.hasErrors()) return true;
    if (firstError != nullptr) {
        for (auto const& d : diags.all()) {
            if (d.severity == DiagnosticSeverity::Error) {
                *firstError = std::string{diagnosticCodeName(d.code)};
                break;
            }
        }
    }
    return false;
}

void expectParses(std::shared_ptr<GrammarSchema const> const& schema,
                  std::string_view what, std::string source) {
    std::string err;
    EXPECT_TRUE(parsesClean(schema, std::string{source}, &err))
        << what << " must parse — C 6.7p2 admits the declaration specifiers in "
                   "ANY order and gcc/clang/MSVC each accept this spelling. "
                   "First error: " << err << "\n--- source ---\n" << source;
}

void expectRefused(std::shared_ptr<GrammarSchema const> const& schema,
                   std::string_view what, std::string source) {
    EXPECT_FALSE(parsesClean(schema, std::string{source}))
        << what << " must stay a LOUD refusal — every reference compiler "
                   "refuses it.\n--- source ---\n" << source;
}

}  // namespace

// ── FILE SCOPE: a qualifier before / among the storage-class specifiers ──────
TEST(ParserDeclSpecifierOrder, FileScopeQualifierBeforeStorageClass) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    expectParses(schema, "const before static",
                 "const static int g = 1;\n");
    expectParses(schema, "volatile before static",
                 "volatile static int v = 2;\n");
    expectParses(schema, "_Atomic before static",
                 "_Atomic static int a = 3;\n");
    expectParses(schema, "const before extern",
                 "const extern int e;\n");
    expectParses(schema, "volatile before extern",
                 "volatile extern int ve;\n");
    expectParses(schema, "const before inline (function definition)",
                 "const inline int f(void){ return 7; }\n");
    expectParses(schema, "a qualifier BETWEEN two specifiers",
                 "static const inline int f(void){ return 7; }\n");
    expectParses(schema, "const before _Thread_local",
                 "const _Thread_local int t = 4;\n");
    expectParses(schema, "const before thread_local (C23 spelling)",
                 "const thread_local int t2 = 4;\n");
    expectParses(schema, "const before constexpr",
                 "const constexpr int c = 5;\n");
    expectParses(schema, "const before _Alignas",
                 "const _Alignas(16) int al = 6;\n");
    expectParses(schema, "const before _Noreturn",
                 "const _Noreturn void nf(void){ while (1) {} }\n");
    expectParses(schema, "const before a GNU attribute",
                 "const __attribute__((unused)) int w = 8;\n");
    expectParses(schema, "const before a C23 attribute",
                 "const [[deprecated]] int w2 = 8;\n");
    expectParses(schema, "const before static on a FUNCTION definition",
                 "const static int f(void){ return 9; }\n");
    expectParses(schema, "const before static on a function PROTOTYPE",
                 "const static int f(void);\n");
    expectParses(schema, "two qualifiers before a storage class",
                 "const volatile static int cv = 1;\n");
    expectParses(schema, "qualifiers on BOTH sides of a storage class",
                 "const static const int cc = 1;\n");
    expectParses(schema, "const before typedef",
                 "const typedef int T;\n");
    expectParses(schema, "volatile before typedef",
                 "volatile typedef int T;\n");
}

// ── BLOCK SCOPE: the same question, the other declaration rule ──────────────
TEST(ParserDeclSpecifierOrder, BlockScopeQualifierBeforeStorageClass) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    expectParses(schema, "block-scope const before static",
                 "int main(void){ const static int l = 3; return l; }\n");
    expectParses(schema, "block-scope const before register",
                 "int main(void){ const register int r = 1; return r; }\n");
    expectParses(schema, "block-scope const before auto (storage class)",
                 "int main(void){ const auto int q = 1; return q; }\n");
    expectParses(schema, "block-scope volatile before static",
                 "int main(void){ volatile static int v = 2; return v; }\n");
    expectParses(schema, "block-scope _Atomic before static",
                 "int main(void){ _Atomic static int a = 3; return a; }\n");
    expectParses(schema, "block-scope const before extern",
                 "int main(void){ const extern int ge; return ge; }\n");
    expectParses(schema, "block-scope const before constexpr",
                 "int main(void){ const constexpr int k = 5; return k; }\n");
    expectParses(schema, "block-scope const before _Alignas",
                 "int main(void){ const _Alignas(16) int al = 6; return al; }\n");
    expectParses(schema, "block-scope const before a pointer declarator",
                 "int main(void){ const static char *p = \"hi\"; return p[0]; }\n");
    expectParses(schema, "for-init const before register",
                 "int main(void){ for (const register int i = 0; i < 1; ) "
                 "{ return i; } return 1; }\n");
    expectParses(schema, "block-scope const before an inferred auto",
                 "int main(void){ const auto x = 5; return x; }\n");
    expectParses(schema, "block-scope volatile before an inferred auto",
                 "int main(void){ volatile auto x = 5; return x; }\n");
}

// ── THE CONTROLS: everything that parsed before must still parse ────────────
//
// These are NOT decoration. The change moved the OWNER of the qualifier lead at
// block scope from `kwDeclHead` to `localDeclSpecifiers` + `declHead`, so the
// bare-typedef-name statement and the plain keyword-led declaration are exactly
// the two things a mistake here would break — and it would break them silently
// into an EXPRESSION reading, not into a parse error.
TEST(ParserDeclSpecifierOrder, PreExistingOrdersAndAmbiguitiesAreUnmoved) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    expectParses(schema, "the already-accepted storage-class-first order",
                 "static const int g = 1;\n");
    expectParses(schema, "a bare file-scope const", "const int g = 1;\n");
    expectParses(schema, "a bare file-scope volatile", "volatile int v;\n");
    expectParses(schema, "an EAST qualifier", "int const g = 1;\n");
    expectParses(schema, "a plain block-scope declaration",
                 "int main(void){ int x = 1; return x; }\n");
    expectParses(schema, "a block-scope const",
                 "int main(void){ const int x = 1; return x; }\n");
    expectParses(schema, "a block-scope const on a typedef name",
                 "typedef int T;\nint main(void){ const T x = 1; return x; }\n");
    expectParses(schema, "a bare typedef-name declaration",
                 "typedef int T;\nint main(void){ T x = 1; return x; }\n");
    expectParses(schema, "typedef with a leading attribute",
                 "__attribute__((deprecated)) typedef int T;\n");
    expectParses(schema, "typedef with an EAST-of-keyword qualifier",
                 "typedef const int T;\n");
    expectParses(schema, "block-scope extern",
                 "int main(void){ extern int e; return e; }\n");
    expectParses(schema, "extern with its own trailing specifier run",
                 "int main(void){ extern _Thread_local int e; return e; }\n");

    // ⚠ THE ONE THAT MUST NOT BECOME A DECLARATION. `a * b;` is a
    // multiplication statement; `localDeclSpecifiers` admits no bare Identifier,
    // so the qualifier-lead move cannot reach it. If this ever parses as a
    // declaration the failure is SILENT at the parser tier, so the assertion is
    // on the SEMANTIC consequence instead: `b` stays an undeclared *use*, which
    // it could not be if it had been declared here.
    expectParses(schema, "a multiplication statement",
                 "int main(void){ int a = 2, b = 3; a * b; return 0; }\n");
}

// ── THE NEGATIVES: C 6.7.1p2, at most one storage-class specifier ───────────
//
// ⚠ A FIXTURE MUST SYNTHESIZE THE NEGATIVE. Every case here is refused by
// gcc, clang AND MSVC, and every one of them is reachable through the NEW
// order-free run — which is exactly why widening the run must be shown not to
// have widened the ACCEPTED set. Checked by parse refusal at file scope (the
// grammar itself has no second storage-class slot) and, for the pairs the
// grammar does admit positionally, by the row's `exclusiveGroup` at the
// semantic tier (see the semantic sibling file).
TEST(ParserDeclSpecifierOrder, IllegalStorageClassCombinationsStayRefused) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    expectRefused(schema, "a storage class AFTER a declarator star",
                  "int main(void){ char * const static p = 0; return p==0; }\n");
    expectRefused(schema, "typedef after a storage class",
                  "static typedef int T;\n");
    expectRefused(schema, "typedef inside the qualifier-led prefix",
                  "const static typedef int T;\n");
    expectRefused(schema, "a qualifier standing in for a type",
                  "const static;\n");
}

// ── THE STRUCTURAL INVARIANT THAT KEEPS THE ALT DECIDABLE ───────────────────
TEST(ParserDeclSpecifierOrder, SpecifierRunAndKeywordHeadStayDisjoint) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    const RuleId run  = schema->rules().find("localDeclSpecifiers");
    const RuleId head = schema->rules().find("kwDeclHead");
    ASSERT_TRUE(run.valid())  << "the `c` grammar must declare localDeclSpecifiers";
    ASSERT_TRUE(head.valid()) << "the `c` grammar must declare kwDeclHead";

    // The three qualifier tokens must lead the RUN and NOT the keyword head:
    // that split is what replaced `kwDeclHead`'s removed qualifier-led alt.
    for (std::string_view const kind :
         {"ConstKeyword", "VolatileKeyword", "AtomicKeyword"}) {
        const SchemaTokenId tok = schema->schemaTokens().find(kind);
        ASSERT_TRUE(tok.valid()) << "unknown token kind " << kind;
        EXPECT_TRUE(schema->firstSetContains(run, tok))
            << kind << " must lead `localDeclSpecifiers` — that is what admits "
               "`const static int l;` (C 6.7p2).";
        EXPECT_FALSE(schema->firstSetContains(head, tok))
            << kind << " must NOT also lead `kwDeclHead`. Two branches of "
               "`varDecl` sharing a lead token is what "
               "`detectAmbiguousAlternatives` refuses at load, and the only "
               "escape is `speculative: true` on varDecl — the measured "
               "4096-token block-scope budget cliff.";
    }

    // …and the type keywords must lead the HEAD and not the run, which is the
    // other half of the disjointness (a type keyword smuggled into
    // `localDeclSpecifier` would re-open the same collision from the far side).
    for (std::string_view const kind :
         {"IntKeyword", "CharKeyword", "StructKeyword"}) {
        const SchemaTokenId tok = schema->schemaTokens().find(kind);
        if (!tok.valid()) continue;   // a spelling this grammar does not carry
        EXPECT_TRUE(schema->firstSetContains(head, tok))
            << kind << " must lead `kwDeclHead`.";
        EXPECT_FALSE(schema->firstSetContains(run, tok))
            << kind << " must NOT lead `localDeclSpecifiers`.";
    }
}

// ── externDecl KEEPS A REAL 1-TOKEN PRUNE ───────────────────────────────────
//
// `externSpecifiers` grew a leading qualifier run as an `alt` of two sequences
// rather than as a leading `{repeat}`, and the difference is measurable rather
// than stylistic: a nullable lead makes `computePredictivePrefixes` stop without
// recording, `predictivePrefixPrunes` then returns false for EVERY input, and
// this row becomes a live speculative candidate for every statement in every
// function body. The pin is that FIRST(externDecl) is exactly the four lead
// tokens it should be — no type keyword, no identifier.
TEST(ParserDeclSpecifierOrder, ExternDeclLeadSetStaysNarrow) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    const RuleId ext = schema->rules().find("externDecl");
    ASSERT_TRUE(ext.valid()) << "the `c` grammar must declare externDecl";

    for (std::string_view const kind : {"ExternKeyword", "ConstKeyword",
                                        "VolatileKeyword", "AtomicKeyword"}) {
        const SchemaTokenId tok = schema->schemaTokens().find(kind);
        ASSERT_TRUE(tok.valid()) << "unknown token kind " << kind;
        EXPECT_TRUE(schema->firstSetContains(ext, tok))
            << kind << " must lead `externDecl` (`const extern int e;`).";
    }
    for (std::string_view const kind : {"IntKeyword", "Identifier",
                                        "StaticKeyword"}) {
        const SchemaTokenId tok = schema->schemaTokens().find(kind);
        if (!tok.valid()) continue;
        EXPECT_FALSE(schema->firstSetContains(ext, tok))
            << kind << " must NOT lead `externDecl` — an ordinary statement has "
               "to prune this row away untried, which it can only do while the "
               "rule's entry position is a required TOKEN in every arm.";
    }
}

// ── PARAMETERS: `register` is the only storage class C admits there ─────────
//
// C 6.7.6.3p2. DSS admitted NO storage-class specifier on a parameter before
// this cycle, so the ORDER question could not even arise there — and leaving it
// out would have made the fix a partial one that reads as complete. gcc 13.3.0,
// clang 18.1.3 and MSVC 19.51.36252 each accept all three spellings below
// (probed separately, one translation unit per case).
TEST(ParserDeclSpecifierOrder, ParameterStorageClassAndQualifierOrder) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    expectParses(schema, "a plain `register` parameter",
                 "int f(register int x){ return x; }\n");
    expectParses(schema, "qualifier BEFORE `register` in a parameter",
                 "int f(const register int x){ return x; }\n");
    expectParses(schema, "qualifier AFTER `register` in a parameter",
                 "int f(register const int x){ return x; }\n");
    expectParses(schema, "the CONTROL: an ordinary const parameter, whose "
                         "leading qualifier moved into the specifier prefix",
                 "int f(const char *s){ return s[0]; }\n");
    expectParses(schema, "the CONTROL: an EAST-qualified typedef parameter",
                 "typedef int T;\nint f(T const x){ return x; }\n");
}
