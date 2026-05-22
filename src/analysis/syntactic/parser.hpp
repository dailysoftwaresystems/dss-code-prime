#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"

#include <cstddef>
#include <memory>
#include <type_traits>

namespace dss {

// Outcome of `Parser::parse() &&`. Owns the assembled tree; the
// tree's diagnostic stream is reachable via `tree.diagnostics()`.
struct DSS_EXPORT ParseResult {
    Tree tree;
};

// Per-instance knobs for the parser.
struct DSS_EXPORT ParserConfig {
    // Per-AltChoice nesting cap for speculative backtracking.
    // Adversarial speculation that never converges would otherwise
    // loop unbounded. Default 8: deep enough for hand-written
    // grammar's plausible lookahead; bounds the parser's own probe
    // nesting independently of the builder's checkpoint cap.
    std::size_t maxSpeculationDepth = 8;
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
    // Pratt expression dispatch is wired in PA2; until then the
    // parser handles `expr` shapes via the schema-driven dispatch.
    Parser(std::shared_ptr<SourceBuffer>        src,
           std::shared_ptr<GrammarSchema const> schema,
           TokenStream                          tokens,
           ParserConfig                         config = {});

    Parser(Parser const&)            = delete;
    Parser& operator=(Parser const&) = delete;
    Parser(Parser&&)                 = delete;
    Parser& operator=(Parser&&)      = delete;
    ~Parser();

    // Drive the builder from the root rule until EOF or fatal-abort
    // via the forward-progress watchdog.
    [[nodiscard]] ParseResult parse() &&;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
