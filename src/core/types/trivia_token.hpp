#pragma once

#include "core/types/grammar_schema.hpp"
#include "core/types/token.hpp"
#include "core/types/tree_node.hpp"

namespace dss {

// ── IS THIS TOKEN TRIVIA? ONE ANSWER, FOR THE PARSER AND FOR THE TREE BUILDER ──
//
// Trivia is what the grammar never sees: whitespace, a newline, a comment. The
// PARSER skips it (it pushes the token through the builder without consulting its
// cursor, and re-checks the slot against the next meaningful token); the TREE
// BUILDER keeps it as a leaf, so the tree still covers every byte of the input,
// but marks the leaf `EmptySpace` and never lets it widen a node's span (see
// `TreeBuilder::closeFrame_`). Both tiers ask THIS function, so they cannot
// disagree about which tokens are trivia.
//
// ★★★ THE LANGUAGE DECIDES, AND THE CORE KIND IS ONLY THE FALLBACK. This used
// to be a bare `switch (tok.coreKind)` returning true for Whitespace/Newline/
// comments unconditionally — a language SEMANTIC baked into shared substrate,
// invisible for as long as every shipped language agreed with it.
// ✔MEASURED 2026-08-12 by the first one that does not: an assembly dialect is
// LINE-ORIENTED (the newline IS the statement terminator) and declares
// `"\n": [{ "kind": "LineEnd" }]` with no `EmptySpace` flag. The parser skipped
// it anyway, so `ret \n ret` parsed as ONE instruction taking the next line's
// mnemonic as its operand — a WRONG PARSE, not a parse error, produced by a
// config declaration the loader had accepted. The same defect the tokenizer's
// hardcoded newline lexeme had, one tier up: the knob that lies.
//
// ★★ WHY THE THREE-WAY TEST AND NOT SIMPLY `schema.isEmptySpace(kind)`:
// `emptySpaceTokens` is populated only from DECLARED meanings, so a built-in
// kind a language never mentioned is absent from it for the same reason a
// deliberately-significant one is. Reading absence as "significant" would make
// every synthetic test schema that omits `"\n"` start seeing newline tokens.
// So: a DECLARED kind gets the language's answer; an undeclared one keeps the
// historical core-kind default, byte-for-byte; and a body-mode token the
// tokenizer flagged `EmptySpace` (a comment's body) is trivia by that flag.
[[nodiscard]] inline bool isTriviaToken(Token const& tok,
                                        GrammarSchema const& schema) noexcept {
    if (tok.schemaKind.valid() && schema.declaresLexemeToken(tok.schemaKind)) {
        return schema.isEmptySpace(tok.schemaKind);
    }
    switch (tok.coreKind) {
    case CoreTokenKind::Whitespace:
    case CoreTokenKind::Newline:
    case CoreTokenKind::LineComment:
    case CoreTokenKind::BlockComment:
        return true;
    default:
        break;
    }
    return isEmptySpace(tok.flags);
}

} // namespace dss
