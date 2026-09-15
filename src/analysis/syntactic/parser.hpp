#pragma once

#include "analysis/syntactic/binder_sketch.hpp"
#include "analysis/syntactic/pratt_walker.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace dss {

// Outcome of `Parser::parse() &&`. Owns the assembled tree; the
// tree's diagnostic stream is reachable via `tree.diagnostics()`.
//
// The two FC2 sidecars travel WITH the tree (not on it — Tree stays a
// pure frozen CST):
//   * `typeNameCandidates` — ambiguous lone-identifier type-name sites
//     the parser rolled back to the value reading (triage rule 4).
//     The compilation unit's oracle (UnitBuilder::finish) resolves them
//     against cross-file type names and reparses once on a hit.
//   * `globalTypeNames` — the GLOBAL-scope TYPE names this parse bound
//     (typedefs / struct tags / …, per `semantics.declarations`), the
//     export surface the oracle harvests. Empty for binder-less
//     languages.
//   * `globalTypeBindings` — the same GLOBAL-scope TYPE names paired with
//     the source span of their declaring name-token. The oracle's SELF-
//     DEFINITION channel: a candidate whose span equals its own binding
//     span is a typedef's own defining occurrence (C 6.2.1p7) and must
//     NOT be seeded (D-CSUBSET-FN-TYPE-TYPEDEF-PAREN-NAME).
struct DSS_EXPORT ParseResult {
    Tree                                    tree;
    std::vector<AmbiguousTypeNameCandidate> typeNameCandidates;
    std::vector<std::string>                globalTypeNames;
    std::vector<std::pair<std::string, SourceSpan>> globalTypeBindings;
    // c108 (D-PARSE-FLAT-CHAIN-WORK-LINEAR): total token-stream accesses (peek +
    // advance, incl. speculative re-scans) the parse performed — the DETERMINISTIC
    // total-work proxy that replaced a flaky wall-clock O(N²) guard. O(N) for a
    // correct parse; a backtracking blowup makes it super-linear.
    std::uint64_t                           tokenAccessCount = 0;
};

// How the dispatch loop's recovery sites behave when they see an
// unrecognized token. `SingleToken` consumes exactly one token and
// continues (legacy single-step behavior, kept for regression-bisect
// parity). `PanicMode` scans forward until peek hits
// `schema->syncTokens()` ∪ `followSetOf(nearest compiled ancestor)`
// ∪ EOF ∪ lexer Error, capped at `maxSyncScanTokens`.
enum class RecoveryStrategy : std::uint8_t {
    SingleToken,
    PanicMode,
};

// Per-instance knobs for the parser.
struct DSS_EXPORT ParserConfig {
    // Per-AltChoice nesting cap for speculative backtracking. ★ Since P60 a
    // level costs one heap `SpeculationSite` on the driver's `specStack` and
    // NO host frame: the speculation drive and the expression re-entry it used
    // to nest inside were converted onto ONE loop (`Parser::Impl::driveParse_`)
    // (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED).
    // So this is NOT a stack guard any more — it bounds the MEMORY and TIME a
    // nest of live probes may cost, each of which holds an O(depth) builder and
    // schema-walker checkpoint. It survives as a COUNTER THAT FAILS
    // LOUD: over-cap the parser emits a positioned, self-naming
    // `P_MaxSpeculationDepth` and recovers, never a crash and never a
    // fabricated syntax error against the user's own token.
    //
    // CONFIG-DRIVEN: this is the FALLBACK default. The CU build OVERRIDES it
    // from the language's `.lang.json` (`parser.maxSpeculationDepth`) via
    // `parserConfigFor` in compilation_unit.cpp. `Parser::parse` also DERIVES
    // the TreeBuilder's `BuilderConfig::maxSpeculationDepth` from whatever
    // value lands here, so the builder's checkpoint cap can never bind first
    // and invisibly (one probe opens exactly one checkpoint — they are the
    // same number).
    //
    // ⚠ THE OLD DEFAULT WAS 8, AND 8 WAS A CONFORMANCE DEFECT, not a
    // conservative choice. c's `operand` alt speculates once per nested cast,
    // so `(int)` x8 compiled and x9 was refused, while gcc 13.3.0, mingw-w64
    // gcc 13.2.0, clang 18.1.3 and MSVC 19.51 all accept 8, 9, 16, 64, 256 and
    // far beyond — DSS was two orders of magnitude below the union
    // (D-PARSE-NINE-NESTED-CASTS-ARE-REFUSED-BY-THE-SPECULATION-CAP-WITH-A-FABRICATED-SYNTAX-ERROR).
    //
    // WHY THE FALLBACK IS 64 WHILE c DECLARES 2048. Two different questions.
    // c's number is MEASURED against that language's own speculation shape
    // and the memory a live probe costs (see the `$parserComment` in
    // `c.lang.json`); this one has to be safe for a grammar whose author said
    // nothing at all, so it is the ISO C23 5.2.4.1 floor of 63 nesting levels
    // rounded to the next power of two — comfortably above any hand-written
    // grammar's plausible disambiguation nesting. ★ Since P60 a probe lives on
    // the heap (`Parser::Impl::specStack`), not on a host frame, so this is a
    // SEMANTIC limit on nesting, never a stack backstop
    // (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED).
    //
    // ★ AND THE TWO MUST NOT BE EQUAL, WHICH IS A TESTABILITY PROPERTY, NOT A
    // TASTE. If the fallback matched c's value, deleting the `.lang.json` key
    // — or the `parserConfigFor` line that reads it — would change NOTHING
    // observable, and "config-driven" would be a claim no red-on-disable arm
    // could ever refute. With 64 here and 2048 there, removing either makes
    // the corpus example's 65-cast chain fail loud, which is exactly the
    // negative a REMOVE-direction mutant needs to produce.
    //
    // A language that declares no speculative alt at all (toy, and every
    // shipped assembly dialect — ✔MEASURED: zero `"speculative": true` alts in
    // each) never reaches this counter, so the fallback moves nothing for them.
    std::size_t maxSpeculationDepth = 64;

    // Multiplier for the per-probe speculative TOKEN budget: one speculative
    // probe may consume `speculationBudgetFactor x <the alt's declared
    // lookahead>` tokens before it is abandoned. The declared lookahead is a
    // DISAMBIGUATION distance, not a total-cost bound, so the factor is what
    // lets a branch make legitimate progress (descents, token consumption)
    // while still abandoning adversarial input that never converges.
    //
    // ⚠ THIS IS A CEILING ON THE ADMISSIBLE LANGUAGE, and until it became a
    // config key it was a bare `16` inside the probe constructor with NO
    // diagnostic of its own. ✔MEASURED with both depth caps lifted: `(int)`
    // x341 compiled rc 0 and x342 was refused — 341 casts x 3 tokens = 1023,
    // exactly one under c `operand`'s 64 x 16 = 1024 — and the refusal
    // surfaced as `P_NoAlternativeMatched … got 'int'`, a fabricated syntax
    // error against correct source. Over-budget now fails loud as
    // `P_SpeculationBudgetExhausted`, positioned and self-naming.
    //
    // CONFIG-DRIVEN: the FALLBACK is 16 — exactly the constant it replaced, so
    // a language that omits `parser.speculationBudgetFactor` parses
    // bit-identically to before the key existed.
    std::size_t speculationBudgetFactor = 16;

    // Nesting-depth cap for the Pratt walker's expression descent. EVERY
    // expression-deepening path funnels through one chokepoint — a PUSH onto
    // the parser's explicit `ExprFrame` work-stack: nested parens (via the
    // atom re-entry), right-assoc RHS chains, prefix operands, and ternary
    // clauses. The descent is FLAT (D-PARSE-DEEP-NEST-RECURSION-MEMORY, Stage
    // 5): it carries O(1) host-stack cost, so this cap is no longer a stack-
    // overflow backstop but a SEMANTIC limit on nesting depth (the work-stack
    // would otherwise grow heap-unbounded on adversarial input, and the
    // DOWNSTREAM frontend below still recurses per level). When the cap is
    // reached the walker FAILS LOUD with a positioned `P_ExpressionTooDeep`
    // diagnostic at the offending token and RECOVERS (Error leaf + panic-scan
    // + graceful unwind) — it does NOT abort. (Left/None-assoc chains build
    // ITERATIVELY in the climb loop and never deepen, so they do not count
    // against this cap regardless of length.)
    //
    // The cap is a real SEMANTIC limit on nesting depth, not a host-stack
    // artifact. Both former blockers to a high cap are resolved:
    // (1) the c `operand` rule's speculative cast-vs-paren probe is
    // no longer super-linear — a config-driven LL(k) predictive prune makes
    // the parse O(N) in nesting depth (D-PARSE-SPECULATION-OPERAND-QUADRATIC,
    // closed), so the guard is reached promptly even at this cap; (2) the
    // parse's DOWNSTREAM consumer that overflowed the host's ~1 MB main
    // thread stack at ~25 levels — semantic `analyze` — now runs on a
    // dedicated 64 MiB worker stack, as do the CU build (parse) and the
    // HIR/MIR lowering (the `buildCuMir` BUILD half); these frontend stages
    // are now FLAT (plan-24: explicit work-stacks, O(1) host-stack per level),
    // so operator/ternary/prefix/infix chains are bounded ONLY by this cap.
    //
    // CONFIG-DRIVEN (plan-24 Stage 7): this is the FALLBACK default. The CU
    // build OVERRIDES it from the language's `.lang.json`
    // (`parser.maxExpressionDepth`) via `parserConfigFor` in
    // compilation_unit.cpp — c declares 16384; a language that omits the
    // key keeps this 256 fallback. ★ Since P60 the parser holds NO host frame
    // per nesting level — the last residual recursion, the paren/postfix-body
    // atom re-entry (the deferred plan-24 Stage 5b), is an `ExprFrame` parked
    // on the heap work-stack while the schema dispatch closes the atom
    // (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED)
    // — so this value is BOUNDED for the reasons a semantic limit is (the
    // work-stack's heap growth on adversarial input, the union of what the
    // reference compilers WORK at), never as a stack backstop. A nest past
    // the configured cap emits a positioned `P_ExpressionTooDeep` with
    // graceful recovery — NEVER a raw stack overflow.
    std::size_t maxExpressionDepth = 256;

    // Recovery strategy for unrecognized tokens. Default scans to
    // the next sync/follow point; `SingleToken` is the legacy single-
    // step behavior, kept for regression-bisect parity and for tests
    // that pin the old shape.
    RecoveryStrategy recoveryStrategy = RecoveryStrategy::PanicMode;

    // Upper bound on how many tokens panic-mode may scan past a
    // recovery site before giving up and accepting whatever peek
    // currently is. Caps adversarial input (very long stretches with
    // no sync/follow token) and pathological misuses. 64 covers
    // every recovery scenario shipped grammars exercise; raise per-
    // grammar via this config if a corpus shows otherwise.
    std::size_t maxSyncScanTokens = 64;

    // Optional override for the operator-precedence walker. Null
    // (the default) means the parser constructs and owns a
    // `DefaultPrattWalker`. Callers can inject their own walker for
    // tests or for languages whose expression dispatch needs
    // bespoke logic. Move-only ownership.
    std::unique_ptr<PrattWalker> prattWalker;

    // FC2: names seeded into the binder sketch's GLOBAL scope as TYPE
    // bindings before parsing — the compilation-unit oracle's cross-file
    // typedef channel. A reparse triggered by an ambiguous type-name
    // candidate passes the resolved candidate names here so the
    // type-name commit triage sees them as types (case 2) instead of
    // unknowns (case 4). Empty (the default) for every first parse.
    std::vector<std::string> seedGlobalTypeNames;
};

// Schema-driven recursive-descent parser.
//
// Iterative dispatch loop (no C++ call-stack recursion) over the
// compiled position graph. Owns a `TreeBuilder` and an independent
// `SchemaWalker` driven in lock-step with the builder's internal
// walker — divergence between the two is a load-bearing bug-catcher
// (manifests as `P_SchemaCursorDesync`).
//
// Single-use: `parse() &&` consumes the parser and returns a frozen
// `ParseResult`.
class DSS_EXPORT Parser {
public:
    // Preconditions (fatal-asserted in the body): `src` and `schema`
    // must both be non-null. Single-use: the parser is constructed,
    // `parse() &&` is called exactly once, then the parser is gone.
    //
    // `lexerDiagnostics` (optional): the tokenizer's diagnostic reporter
    // from `Tokenizer::tokenize()`. When provided, the parser folds those
    // lexer diagnostics into the resulting Tree's reporter so the Tree
    // owns lexer + parser diagnostics in one stream (08-compilation-unit-
    // plan §2.6 C2-L1). Defaulted to nullptr — existing callers are
    // unaffected.
    // `budget` is REQUIRED and sits BEFORE the two defaulted parameters so a
    // caller cannot reach them without stating it. The parser owns no reporter
    // — it hands this straight to the `TreeBuilder` it builds in `parse()`,
    // which owns the operator-visible parse+lex stream. Before this parameter
    // existed, `TreeBuilder`'s own `diagConfig = {}` default was taken here and
    // the tree capped at the library's 1000/50 whatever the operator
    // configured (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE). Callers
    // with no operator budget in scope pass
    // `DiagnosticBudget::libraryDefault()`.
    Parser(std::shared_ptr<SourceBuffer>        src,
           std::shared_ptr<GrammarSchema const> schema,
           TokenStream                          tokens,
           DiagnosticBudget                     budget,
           ParserConfig                         config = {},
           std::unique_ptr<DiagnosticReporter>  lexerDiagnostics = nullptr);

    Parser(Parser const&)            = delete;
    Parser& operator=(Parser const&) = delete;
    Parser(Parser&&)                 = delete;
    Parser& operator=(Parser&&)      = delete;
    ~Parser();

    // Drive the builder from the root rule until EOF or fatal-abort
    // via the forward-progress watchdog.
    [[nodiscard]] ParseResult parse() &&;

    // Forward-declared so file-scope helpers in `parser.cpp` (where
    // `Impl`'s definition lives) can name the type without being
    // friends. The struct's full definition is private to `parser.cpp`;
    // external callers can hold a `Parser::Impl*` but can't see its
    // members. `DefaultPrattWalker` accesses internals via the friend
    // declaration on `Parser` (below).
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;

    // `DefaultPrattWalker` drives the parser's token stream, builder,
    // schema walker, and frame stack via friend access to `Impl`.
    // User-supplied walkers (passed via `ParserConfig::prattWalker`)
    // don't get this access — the API for them is YAGNI until a
    // real consumer asks.
    friend class DefaultPrattWalker;
};

// Parser must stay non-movable + non-copyable: it's a single-use
// builder; copies would silently fork ownership of the token stream;
// moves would invalidate the `Impl`'s self-references should any be
// added in the future. Matches the discipline applied to
// `TreeBuilder` in PA0.
static_assert(!std::is_move_constructible_v<Parser>,
              "Parser must stay non-movable — single-use by design");
static_assert(!std::is_copy_constructible_v<Parser>,
              "Parser must stay non-copyable — single-use by design");

} // namespace dss
