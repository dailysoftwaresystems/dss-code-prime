#pragma once

#include "analysis/syntactic/binder_sketch.hpp"
#include "analysis/syntactic/pratt_walker.hpp"
#include "core/export.hpp"
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
struct DSS_EXPORT ParseResult {
    Tree                                    tree;
    std::vector<AmbiguousTypeNameCandidate> typeNameCandidates;
    std::vector<std::string>                globalTypeNames;
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
    // Per-AltChoice nesting cap for speculative backtracking.
    // Adversarial speculation that never converges would otherwise
    // loop unbounded. Default 8: deep enough for hand-written
    // grammar's plausible lookahead; bounds the parser's own probe
    // nesting independently of the builder's checkpoint cap.
    std::size_t maxSpeculationDepth = 8;

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
    // (1) the c-subset `operand` rule's speculative cast-vs-paren probe is
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
    // compilation_unit.cpp — c-subset declares 1024; a language that omits the
    // key keeps this 256 fallback. The value is BOUNDED, not unbounded,
    // because ONE recursion remains: the parser's paren/postfix-body arm (the
    // deferred plan-24 Stage 5b) still costs a host frame per nested `(` on the
    // 64 MiB worker. The cap is the fail-loud SAFETY BACKSTOP for that arm — it
    // must trip BEFORE the worker overflows on the worst supported build (the
    // measured paren-crash floor is ~3000 on MSVC Debug; c-subset's 1024 sits
    // ~3x below it). A nest past the configured cap emits a positioned
    // `P_ExpressionTooDeep` with graceful recovery — NEVER a raw stack overflow.
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
    Parser(std::shared_ptr<SourceBuffer>        src,
           std::shared_ptr<GrammarSchema const> schema,
           TokenStream                          tokens,
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
