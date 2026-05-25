#pragma once

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
#include <type_traits>

namespace dss {

// Outcome of `Parser::parse() &&`. Owns the assembled tree; the
// tree's diagnostic stream is reachable via `tree.diagnostics()`.
struct DSS_EXPORT ParseResult {
    Tree tree;
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

    // Recursion-depth cap for the Pratt walker's C++-stack recursion
    // on the right-hand side of infix operators. Pathological input
    // (deeply nested parens or right-assoc chains) would otherwise
    // blow the call stack. The walker fatal-aborts when this cap is
    // exceeded — same posture as the dispatch loop's forward-progress
    // watchdog. 256 fits both real C-style code and the test corpus
    // with plenty of headroom.
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
