#pragma once

// D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN — the CONSUMER-side resolver for a
// coalesced literal's CLOSE-delimiter token kind.
//
// The tokenizer emits a value-bearing literal as THREE tokens: an OPENER
// (`"` / `'` / `<`), a COALESCED BODY covering only the bytes between the
// delimiters, and — since this anchor closed — a CLOSER of the kind the mode
// declares in `lexerModes.<name>.defaultToken.closeToken`. The body's span and
// text are deliberately UNCHANGED by that work, so every decoder is unaffected;
// what changed is that a token now FOLLOWS the body. Every consumer that used to
// assume "nothing follows the body" — an index step (`body + 1` is the `)`), a
// child-count shape check, a source-extent cut, a hand re-consume of the
// delimiter BYTE — has to be taught about it.
//
// Consumers must reach that kind THROUGH THE SCHEMA, never by naming
// `"StringEnd"` / `"CharEnd"` / `"HeaderEnd"`: the closer kind is per-language
// config exactly like the opener and body kinds are, and a hard-coded name is
// the same source-agnosticism break the engine forbids everywhere else.
//
// The lookup key is the BODY token's own kind, which is the one thing every
// consumer already has in hand: a consumer either reads the body kind from
// config (`hirLowering.charBodyToken` / `stringBodyToken`) or holds the body
// TOKEN it just located positionally (the preprocessor's include arms, whose
// header-path body kind has no config name of its own).
//
// A body kind does NOT identify a single mode — tsql's `single-string` and
// `unicode-string` both emit `StringLiteral`, and only the OPENER tells `'a'`
// from `N'a'`. What makes the body kind a sound lookup key is an invariant the
// LOADER ENFORCES: coalesced modes that share a `defaultToken.kind` must declare
// the SAME `closeToken` (grammar_schema_json.cpp, right after the `lexerModes`
// parse loop; `C_ConflictingField` otherwise). So the first-match scan below is
// order-independent by construction rather than by luck — which matters, because
// an un-enforced version of this invariant would make the resolver hand out the
// first-declared mode's closer for literals lexed by the second, silently and
// with mode-declaration order as the deciding factor.
//
// RESIDUAL GAP, DELIBERATELY UNGUARDED — DO NOT "FIX" IT INTO A DEFAULT. The
// loader enforces `coalesce ⇒ closeToken`, but NOT the grammar-side corollary
// that every shape spelling the body also spells the closer. That gap is
// LOUD, not silent, and MEASURED so: dropping `StringEnd` from
// `stringLiteralExpr`'s head sequence in c-subset makes the very next string
// literal fail to parse — `P0009 expected 'EndStatement' or 'BlockOpen' —
// got '"'`, then `P0002 expected 'ParenClose' — got '"'` — because the closer
// token is really in the stream and the schema cursor has no slot for it. A
// missing closer slot cannot produce a wrong answer, only a refused parse, so
// the check would buy nothing that the parser does not already deliver at the
// exact offending token. Anyone tempted to close it by having the shape
// matcher SKIP or auto-insert an unspelled closer would be converting a loud
// parse error into a silently mis-shaped tree — the precise failure class this
// anchor exists to eliminate.

#include "core/types/grammar_schema.hpp"
#include "core/types/lexer_mode.hpp"
#include "core/types/strong_ids.hpp"

namespace dss {

// The CLOSE-delimiter token kind for the coalesced body kind `bodyKind`, or an
// INVALID id when `bodyKind` is not a coalesced body kind in this schema (a
// language with no such literal, or a per-codepoint body mode — those emit
// their closer through the ordinary body path and declare no `closeToken`).
//
// Callers treat an invalid result as "this language emits no closer token" and
// must stay correct in that case — that is what keeps a non-C schema (toy/tsql)
// byte-identical rather than newly fail-loud.
[[nodiscard]] inline SchemaTokenId
closeTokenForCoalescedBody(GrammarSchema const& schema,
                           SchemaTokenId        bodyKind) noexcept {
    if (!bodyKind.valid()) return {};
    // First match wins, and that is SAFE only because the loader rejects two
    // coalesced modes that share `bodyKind` with different closers (see the
    // header note above). Do not relax that check without replacing this scan.
    for (LexerMode const& m : schema.lexerModes()) {
        if (!m.defaultToken.has_value() || !m.defaultToken->coalesce) continue;
        if (m.defaultToken->kind == bodyKind) return m.defaultToken->closeToken;
    }
    return {};
}

} // namespace dss
