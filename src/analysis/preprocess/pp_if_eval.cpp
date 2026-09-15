#include "analysis/preprocess/pp_if_eval.hpp"

#include "core/types/attribute_naming.hpp"   // stripDunder (shared with the packed scan)
#include "core/types/char_decode.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/integer_literal_ladder.hpp"   // C 6.4.4.1 ladder (D-PP-IF-UNSIGNED-INTMAX)
#include "core/types/literal_close_token.hpp"   // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN
#include "core/types/number_decode.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "hir/const_eval.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/const_eval_operators.hpp"
#include "hir/hir_op.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

using detail::applyBinaryInt;
using detail::applyUnaryInt;
using detail::asBool;
using detail::asInt64;
using detail::asIntBits;
using detail::makeBoolLiteral;

// ── C 6.10.1p4: THE PHASE-4 EVALUATION TYPE (D-PP-IF-UNSIGNED-INTMAX) ────────
//
// A `#if`/`#elif` controlling expression is evaluated with every operand acting
// as `intmax_t` or `uintmax_t` -- NOT as the operand's own C type. This mints an
// operand carrying that type, and it is the ONE site that says what a `#if`
// operand's (width, signedness) is.
//
// ★★ BOTH HALVES ARE LOAD-BEARING, AND BOTH WERE BROKEN. Every leaf used to be
// stamped `TypeKind::I32` with the value cast to int64. Because `intOpDomain`
// derives the operation's domain from the operands' CORES, that made the whole
// evaluator run in **32-bit signed** -- not the "signed int64" the row and the
// old comment here both claimed:
//   * WIDTH: `wrapToIntTarget` truncated both operands to 32 bits before every
//     comparison, so `#if 9223372036854775807 > 0` -- "is INT64_MAX positive"
//     -- took the FALSE arm, as did `#if 3000000000 > 0` and
//     `#if 2147483647 + 1 > 0`. Ordinary signed code, no unsigned suffix in
//     sight.
//   * SIGNEDNESS: the literal's unsignedness was discarded at the leaf, so
//     `#if -1 < 0u` answered with a SIGNED comparison and took the wrong arm.
// Both produced a perfectly successful compile of a DIFFERENT program, which is
// why the defect survived four months labelled "fail-loud": a pin that asserts
// "compiles clean" is structurally blind to it. See the pin harness, which reads
// the taken arm out of the emitted object rather than from an exit code.
//
// ⚠ WIDTH IS ALWAYS 64 AND IS NOT THE LADDER'S ANSWER. C 6.4.4.1 types `1` as a
// 32-bit `int`, which is the right answer to a question phase 4 is not asking.
// Take the ladder's SIGNEDNESS and evaluate at width 64, or the 32-bit domain
// comes straight back.
//
// The value is stored in the int64 arm as the raw two's-complement BIT PATTERN,
// which is the representation `asIntBits` reads behind an established domain and
// the one `applyBinaryInt`/`applyUnaryInt` already produce for their own
// results. A `uint64_t` arm would instead make `asInt64` -- which `applyUnaryInt`
// calls -- nullopt for exactly the large unsigned values this exists to carry.
[[nodiscard]] HirLiteralValue intmaxOperand(std::uint64_t bits, bool isSigned) {
    HirLiteralValue lv;
    lv.core  = isSigned ? TypeKind::I64 : TypeKind::U64;
    lv.value = static_cast<std::int64_t>(bits);
    return lv;
}

// Trivia recognizers (mirroring the anon-namespace helpers in
// preprocessor.cpp; duplicated here because they are TU-local statics there and
// the rule is trivial). A `#if` operand never contains a newline (the operand
// is `[directive-word .. lineEnd)`), but comments/whitespace can appear.
[[nodiscard]] bool isTriviaTok(Token const& t) {
    return t.coreKind == CoreTokenKind::Whitespace
        || t.coreKind == CoreTokenKind::LineComment
        || t.coreKind == CoreTokenKind::BlockComment
        || t.coreKind == CoreTokenKind::Newline
        || isEmptySpace(t.flags);
}
[[nodiscard]] bool isWordTok(Token const& t) {
    return t.coreKind == CoreTokenKind::Word;
}

// ★ THE ONE SLICING RULE, AND IT IS `MacroExpander::text`'s RULE. A token's
// bytes have exactly one home: a SYNTHETIC token (a `defined`/`__has_*` result
// minted by the rewrite) slices `scratch`; every REAL token slices the PREFIX
// (`prefix`) when its span starts inside it, and the PRODUCT TAIL when it starts
// at-or-past the prefix's end (FC15a's A2 layout: a product token's span is
// `[prefixLen + productOffset, …)`). Sharing the rule rather than materializing
// `prefix + tail` as one buffer is what removed a whole-TU copy from every `#if`
// — see D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION at the
// `evaluateIfExpression` call site.
// The `e <= tail.size()` bound mirrors the expander's own defensive arm: a
// malformed product span returns EMPTY rather than reading out of range (an empty
// spelling then fails loud in `decodeInteger`, never a silent wrong value).
// `scratch` may be null where the caller provably never asks about a synthetic
// token (D-PP-DEFINED-VIA-MACRO-EXPANSION's post-expansion rewrite reads WORD
// tokens only, and a minted result is an IntLiteral).
[[nodiscard]] std::string_view ppTokenText(Token const& t,
                                           SourceBuffer const& prefix,
                                           SourceBuffer const* scratch,
                                           std::string_view tail) {
    if (has(t.flags, NodeFlags::Synthetic)) {
        return scratch ? scratch->slice(t.span) : std::string_view{};
    }
    const ByteOffset prefixLen = static_cast<ByteOffset>(prefix.text().size());
    if (t.span.start() >= prefixLen) {
        const ByteOffset s = t.span.start() - prefixLen;
        const ByteOffset e = t.span.end() - prefixLen;
        if (e <= tail.size() && s <= e) return tail.substr(s, e - s);
        return {};
    }
    return prefix.slice(t.span);
}

// Emit a positioned preprocessor diagnostic on the synth buffer.
void emit(DiagnosticReporter& rep, DiagnosticCode code, BufferId buffer,
          SourceSpan span, std::string msg) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.buffer   = buffer;
    d.span     = span;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// The set of token KINDS the schema types as INTEGER literals (config-driven:
// the `literalTypes` rows whose core is an integer kind, EXCLUDING the
// `stringArray` string rows). Built from the schema so a language adding a new
// integer-literal kind is picked up without touching this code -- mirrors
// `integerLiteralTokenSet` in the semantic analyzer. The float/string/char
// literal kinds are gathered separately so the evaluator can REJECT a float or
// string literal in `#if` (C 6.10.1p1: the operand is an INTEGER constant
// expression).
struct LiteralKinds {
    std::unordered_set<std::uint32_t> integer;   // accepted -> decodeInteger
    std::unordered_set<std::uint32_t> floating;  // rejected (P_..Unsupported)
    std::unordered_set<std::uint32_t> string;    // rejected (P_..Unsupported)
    // A keyword literal (C23 true/false) maps a token to a FIXED value, so it
    // is admitted with that value rather than decoded as text.
    std::unordered_set<std::uint32_t>               fixedKinds;
    std::vector<std::pair<std::uint32_t, std::int64_t>> fixedValues;
};

[[nodiscard]] LiteralKinds gatherLiteralKinds(GrammarSchema const& schema) {
    LiteralKinds lk;
    SemanticConfig const& sem = schema.semantics();
    for (LiteralTypeMapping const& m : sem.literalTypes) {
        if (!m.literal.valid()) continue;
        std::uint32_t const tok = m.literal.v;
        if (m.fixedValue.has_value()) {
            // C23 keyword literal (true/false): a fixed-value integer.
            lk.fixedKinds.insert(tok);
            lk.fixedValues.emplace_back(tok, *m.fixedValue);
            continue;
        }
        if (m.stringArray) {
            lk.string.insert(tok);
            continue;
        }
        TypeKind const k = m.core;
        if (k == TypeKind::F16 || k == TypeKind::F32 || k == TypeKind::F64
            || k == TypeKind::F80 || k == TypeKind::F128) {
            lk.floating.insert(tok);
        } else {
            // Char / I8 / U8 / Bool / I16 / I32 / ... -> integer literal.
            // (c maps CharLiteral -> I32, so a char constant in `#if` is
            // an integer per C 6.4.4.4 -- but its VALUE decode is the char
            // path; for now a non-integer-LITERAL token text reaching
            // decodeInteger fails the malformed-literal fail-loud, which is the
            // safe behavior. Plain IntLiteral is the common `#if` case.)
            lk.integer.insert(tok);
        }
    }
    return lk;
}

// ── The internal atom model ──────────────────────────────────────────────────
//
// `defined X` is resolved BEFORE macro expansion (C 6.10.1p4) into a synthetic
// integer literal token. A synthetic token is minted from an owned `scratch`
// buffer holding the digit text and tagged `NodeFlags::Synthetic`; the ICE
// parser slices a Synthetic-tagged token against `scratch`, every other token
// against `synth`. Crucially the preprocessor's `expand()` only ever SLICES
// `Word` tokens (to read their name) -- it copies every non-Word token by value
// -- so a synthetic IntLiteral survives macro expansion untouched and is never
// mis-sliced.

// The Pratt evaluator. Reads a flat `Token` vector; precedence + associativity
// + operator mapping all come from the schema (config-driven). Reuses the
// shared `applyBinaryInt`/`applyUnaryInt`/`asBool`/`makeBoolLiteral` core.
class IceParser {
public:
    IceParser(std::vector<Token> toks, GrammarSchema const& schema,
              SourceBuffer const& synth, SourceBuffer const& scratch,
              LiteralKinds const& lits, DiagnosticReporter& rep,
              BufferId diagBufferId, std::string_view productTail,
              std::optional<bool> charIsUnsigned)
        : toks_(std::move(toks)),
          schema_(schema),
          synth_(synth),
          scratch_(scratch),
          productTail_(productTail),
          // FC15b: diagnostics attribute to the prefix synth buffer
          // (`diagBufferId`). A real token's span is either a valid PREFIX offset
          // or a PRODUCT offset past the prefix's end; the prefix id keeps the
          // diagnostic on the buffer `preprocess()` remaps, which is the only
          // registered one (the product tail is not a buffer at all).
          diagBufferId_(diagBufferId),
          lits_(lits),
          rep_(rep),
          binaryOps_(schema.hirLowering().binaryOps),
          unaryOps_(schema.hirLowering().unaryOps),
          opTable_(schema.operatorTable()),
          numberStyle_(schema.numberStyle()),
          // D-PP-IF-UNSIGNED-INTMAX: the language's C 6.4.4.1 candidate ladder.
          // ⓘ NO DATA MODEL ACCOMPANIES IT, and that is a property of C 6.10.1p4
          // rather than an omission: at phase-4 widths every candidate is 64
          // bits, so a WIDTH model cannot reach the signedness answer. See
          // `preprocessorLiteralSignedness`.
          intLadder_(schema.semantics().integerLiteralTyping),
          charIsUnsigned_(charIsUnsigned) {
        // The string-literal OPENER (C's `"`). A string literal lexes as an
        // opener token (`StringStart`) + a coalesced body; the body's schema
        // kind is in `lits_.string`, but the FIRST token the parser meets is the
        // opener, so reject THAT too. The opener is the config `quoteIncludeToken`
        // (C reuses `"` for both string literals and quote includes), read from
        // config -- never a hard-coded "StringStart".
        stringOpenKind_ =
            schema_.schemaTokens().find(schema_.preprocess().quoteIncludeToken);
        // c12: the char-literal OPENER (`'`) + coalesced BODY kinds, read from
        // `hirLowering` config (never a hard-coded "CharStart"/"CharLiteral"). A
        // char constant in `#if` is an INT whose value is the (escape-decoded)
        // single byte (C 6.10.1p4 + 6.4.4.4). Both invalid ⇒ the language has no
        // char-literal form (toy/tsql) and the arm never fires.
        // CYCLE B (C11/C23 6.4.4.4 wide/UTF chars): ONLY the narrow `'` opener is
        // recognized here. A WIDE opener (`L'`/`u'`/`U'`/`u8'`) in `#if` is left
        // UNHANDLED ON PURPOSE — it is not `charOpenKind_`, not an integer, and not a
        // Word token, so `parsePrimary` falls through to its fail-loud "unexpected
        // token in #if" (VERIFIED: `#if L'A'` → P_PreprocessorDirective, never a
        // silent 0). Wide char constants in a `#if` controlling expression (their
        // int value + the execution-charset mapping) are a later cycle; until then
        // the honest behavior is a hard error, NOT a silent misevaluation.
        charOpenKind_ = schema_.hirLowering().charStartToken;
        charBodyKind_ = schema_.hirLowering().charBodyToken;
        // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: the closing `'` is a real token
        // now, so the char arm must CONSUME it or `evaluate()`'s end-of-run check
        // reports "trailing tokens after #if controlling expression" for the
        // ordinary `#if 'A' == 65`. Its kind is resolved from the schema via the
        // BODY kind (the mode that declares this coalesced body declares the
        // closer) — never the spelling `"CharEnd"`. Invalid for a language whose
        // char body mode declares no closer; the arm then consumes nothing extra.
        charCloseKind_ = closeTokenForCoalescedBody(schema_, charBodyKind_);
    }

    // Parse the WHOLE token run to one value. nullopt on any fail-loud condition
    // (already reported): an empty run and a TRAILING unconsumed token are both
    // malformed. The ONE parse both entry points below share, so the truth
    // value and the phase-4 value can never come from different expressions.
    [[nodiscard]] std::optional<HirLiteralValue> parseWhole() {
        if (atEnd()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "#if with an empty controlling expression");
            return std::nullopt;
        }
        auto v = parseExpr(/*minPrec=*/std::numeric_limits<std::int32_t>::min(),
                           /*eval=*/true);
        if (!v.has_value()) return std::nullopt;
        if (!atEnd()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "trailing tokens after #if controlling expression");
            return std::nullopt;
        }
        return v;
    }

    // The phase-4 VALUE (C 6.10.2p13: `intmax_t` / `uintmax_t`) of the whole
    // run -- what an embed `limit` needs (D-PP-EMBED-PARAMS). The same
    // integer-ness gate `evaluate()` applies (a float or string is refused with
    // the same message), then the raw bits behind the domain `intmaxOperand`
    // established: `asIntBits` is the reader entitled to them, and the core
    // says which of the two types they are.
    [[nodiscard]] std::optional<PpIfValue> evaluateValue() {
        auto v = parseWhole();
        if (!v.has_value()) return std::nullopt;
        if (!asBool(*v, /*allowFloat=*/false).has_value()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "#if expression is not an integer constant");
            return std::nullopt;
        }
        auto const bits = asIntBits(*v);
        if (!bits.has_value()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "#if expression is not an integer constant");
            return std::nullopt;
        }
        return PpIfValue{static_cast<std::uint64_t>(*bits),
                         v->core != TypeKind::U64};
    }

    // Evaluate the whole token run to its TRUTH VALUE. nullopt on any fail-loud
    // condition (already reported).
    [[nodiscard]] std::optional<bool> evaluate() {
        auto v = parseWhole();
        if (!v.has_value()) return std::nullopt;
        // ── D-PP-IF-UNSIGNED-INTMAX ──────────────────────────────────────────
        // The question C 6.10.1p2 asks of the result is TRUTHINESS ("if it
        // compares unequal to 0"), never int64 representability. This used to
        // ask `asInt64`.
        //
        // ⓘ HONESTY NOTE, BECAUSE THE ROW ORIGINALLY CLAIMED OTHERWISE AND THE
        // MEASUREMENT SAID NO. The plan for this fix predicted that changing the
        // leaf ALONE would turn the wrong branch into a spurious "not an integer
        // constant" refusal of `#if UINT64_MAX`, making this change load-bearing.
        // ✔MEASURED (red-on-disable arm M3, 2026-08-27): reverting THIS line
        // alone, with the leaf fix in place, is **GREEN** -- all 11
        // `PreprocessorIfIntmax` pins pass and `examples/c/c_pp_if_intmax` still
        // exits 42, indistinguishable from the line-inserting CONTROL arm.
        // The prediction was correct for the representation the plan assumed (the
        // large unsigned value in the `uint64_t` arm, where `asInt64` genuinely
        // nullopts) and wrong for the one that shipped: `intmaxOperand` stores the
        // raw bit pattern in the INT64 arm -- it must, or `applyUnaryInt`'s own
        // `asInt64` bridge would refuse unary operators on large unsigned values
        // -- and `asInt64`'s int64 arm always succeeds.
        //
        // ★ IT STAYS ANYWAY, as CONTRACT correctness rather than a behaviour fix:
        // `asBool` is the question the standard actually asks, and it is the only
        // spelling that stays right if any future producer in this evaluator mints
        // a `uint64_t` or `BitIntValue` arm -- at which point `asInt64` would
        // start refusing valid code silently. Do not "simplify" it back.
        auto const truth = asBool(*v, /*allowFloat=*/false);
        if (!truth.has_value()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "#if expression is not an integer constant");
            return std::nullopt;
        }
        return *truth;
    }

private:
    std::vector<Token>            toks_;
    GrammarSchema const&          schema_;
    SourceBuffer const&           synth_;
    SourceBuffer const&           scratch_;
    // FC15b: the expander's accumulated `#`/`##`/predefined PRODUCT bytes, which
    // conceptually sit immediately AFTER `synth_`'s bytes. Borrowed, never copied
    // — see `textOf` and, at the call site, the note anchored
    // D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION.
    // Empty for a language with no products.
    std::string_view              productTail_;
    BufferId                      diagBufferId_{};
    LiteralKinds const&           lits_;
    DiagnosticReporter&           rep_;
    std::vector<HirOperatorEntry> const& binaryOps_;
    std::vector<HirOperatorEntry> const& unaryOps_;
    OperatorTable const&          opTable_;
    NumberStyle const*            numberStyle_ = nullptr;
    // D-PP-IF-UNSIGNED-INTMAX: C 6.4.4.1's ordered candidate ladder, borrowed
    // from the schema (which outlives this parser). EMPTY for a language that
    // declares none -- see `parsePrimary`'s literal arm for what that means.
    std::span<IntegerLiteralTypingRule const> intLadder_{};
    SchemaTokenId                 stringOpenKind_{};
    SchemaTokenId                 charOpenKind_{};   // c12: `'` opener
    SchemaTokenId                 charBodyKind_{};   // c12: coalesced char body
    SchemaTokenId                 charCloseKind_{};  // the `'` closer's own token
    // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]] / [[D-CSUBSET-CHAR-HIGHBYTE-ICE-SIGNEDNESS]]:
    // the ACTIVE (target × object format)'s plain-`char` signedness, from the ONE
    // accessor `TargetSchema::charIsUnsigned(ObjectFormatKind)`. C 6.10.1p4 makes
    // a char constant in `#if` an `int`, and 6.4.4.4p10 makes THAT int negative
    // for a high byte on a signed-`char` target — so this evaluator needs the
    // target fact even though it runs before any type checking. `nullopt` (a
    // caller with no target: the LSP, the direct-API tests) is honest and the
    // 0–127 bodies that are every real program still fold; only a high byte
    // refuses, loud.
    std::optional<bool>           charIsUnsigned_{};
    std::size_t                   pos_ = 0;
    bool                          failed_ = false;

    // Cursor over NON-trivia tokens.
    [[nodiscard]] bool atEnd() const { return pos_ >= toks_.size(); }
    [[nodiscard]] Token const& peek() const { return toks_[pos_]; }
    void advance() { ++pos_; }

    // ★ THE ONE SLICING RULE — it lives in `ppTokenText` (top of this file), and
    // the post-expansion `defined` rewrite (D-PP-DEFINED-VIA-MACRO-EXPANSION)
    // reads WORD spellings through the SAME function, so the two readers of a
    // three-homed token cannot drift.
    [[nodiscard]] std::string_view textOf(Token const& t) const {
        return ppTokenText(t, synth_, &scratch_, productTail_);
    }

    void fail(DiagnosticCode code, std::string msg) {
        if (failed_) return;   // first failure wins (one positioned diagnostic)
        failed_ = true;
        // Position on the offending token's synth span. A SYNTHETIC token (a
        // `defined`-result minted in the scratch buffer) has no real source
        // location, so it is positioned at synth offset 0; every real token
        // carries its valid synth span.
        bool const onSynthetic =
            !atEnd() && has(peek().flags, NodeFlags::Synthetic);
        SourceSpan const span =
            onSynthetic ? SourceSpan::empty(0)
            : atEnd() ? (toks_.empty() ? SourceSpan::empty(0) : toks_.back().span)
                      : peek().span;
        emit(rep_, code, diagBufferId_, span, std::move(msg));
    }

    // A token KIND -> binary operator entry (config). nullptr if not a binary
    // operator in this position.
    [[nodiscard]] HirOperatorEntry const* asBinary(Token const& t) const {
        return opEntryFor(binaryOps_, t.schemaKind);
    }
    [[nodiscard]] HirOperatorEntry const* asUnary(Token const& t) const {
        return opEntryFor(unaryOps_, t.schemaKind);
    }

    // Reject the non-`#if` subset on a token: a float/string LITERAL, or an
    // assignment/comma operator (handled in `parseExpr`). NOTE: `sizeof` is NOT
    // special-cased here -- the C preprocessor does not know keywords (C
    // 6.10.1p4), so `sizeof` folds as an ordinary identifier to 0, and a
    // `sizeof(int)` shape then fails loud as a malformed expression (a trailing
    // `(` with no operator) -- exactly as a real C preprocessor reports it.
    // Recognising `sizeof` by a hard-coded token NAME would also break
    // agnosticism (the name is c-specific), so the float/string rejection
    // below is keyed on the CONFIG `literalTypes` kinds, never a token name.
    [[nodiscard]] bool rejectIfUnsupported(Token const& t) {
        if (lits_.floating.count(t.schemaKind.v) != 0) {
            fail(DiagnosticCode::P_PreprocessorUnsupported,
                 "a floating literal is not permitted in a #if expression");
            return true;
        }
        // A string literal: either its coalesced BODY kind (lits_.string) or its
        // OPENER (`StringStart`). The opener is the first token the parser meets.
        if (lits_.string.count(t.schemaKind.v) != 0
            || (stringOpenKind_.valid() && t.schemaKind == stringOpenKind_)) {
            fail(DiagnosticCode::P_PreprocessorUnsupported,
                 "a string literal is not permitted in a #if expression");
            return true;
        }
        return false;
    }

    // Precedence-climbing expression parser.
    //   primary := unary-op primary | '(' expr ')' | literal | identifier(=>0)
    //   expr    := primary ( binary-op expr )*   with config precedence/assoc
    // `&&`/`||` short-circuit; `?:` is the ternary mixfix.
    //
    // `eval` controls whether arithmetic is FOLDED. In an UN-evaluated context
    // (the dead side of a short-circuit `&&`/`||` or the not-taken arm of a
    // `?:`), the parser still consumes + structurally validates the tokens (an
    // unevaluated operand must be syntactically valid, C 6.6) but does NOT fold
    // -- so `0 && (1/0)` does not raise a div-by-zero. Unevaluated folds yield a
    // dummy 0.
    [[nodiscard]] std::optional<HirLiteralValue> parseExpr(std::int32_t minPrec,
                                                           bool eval) {
        auto lhsOpt = parsePrimary(eval);
        if (!lhsOpt.has_value()) return std::nullopt;
        HirLiteralValue lhs = std::move(*lhsOpt);

        while (!atEnd()) {
            Token const opTok = peek();
            if (rejectIfUnsupported(opTok)) return std::nullopt;

            // Classify the token in INFIX position. A binary operator is an
            // `hirLowering.binaryOps` entry (Add/.../LogicalAnd/Assign/Comma);
            // the ternary `?` is NOT a binaryOps entry -- it is the operator
            // table's Ternary-arity entry. A token is never both.
            HirOperatorEntry const* be = asBinary(opTok);
            auto ternEntry =
                opTable_.lookup(opTok.schemaKind, OperatorArity::Ternary);
            if (be == nullptr && !ternEntry.has_value()) break;  // not infix

            // Determine precedence/assoc from the operator table (config), so
            // `1+2*3` folds with C precedence (a left-fold would give 9).
            std::int32_t  prec = 0;
            OperatorAssoc assoc = OperatorAssoc::Left;
            bool const isTernary = (be == nullptr);
            if (isTernary) {
                prec  = ternEntry->precedence;
                assoc = ternEntry->associativity;
            } else {
                auto e = opTable_.lookup(opTok.schemaKind, OperatorArity::Infix);
                if (!e.has_value()) {
                    fail(DiagnosticCode::P_PreprocessorDirective,
                         "operator has no infix precedence in a #if expression");
                    return std::nullopt;
                }
                prec  = e->precedence;
                assoc = e->associativity;
            }

            if (prec < minPrec) break;

            // Assignment / compound-assign are not ICE operators (checked AFTER
            // the precedence gate so a too-low-precedence non-op simply breaks).
            if (!isTernary
                && (be->target == "Assign" || !be->compoundBase.empty())) {
                fail(DiagnosticCode::P_PreprocessorUnsupported,
                     "assignment is not permitted in a #if expression");
                return std::nullopt;
            }

            advance();   // consume the operator token

            if (isTernary) {
                auto res = parseTernaryTail(std::move(lhs), *ternEntry, prec, eval);
                if (!res.has_value()) return std::nullopt;
                lhs = std::move(*res);
                continue;
            }

            // Short-circuit logical operators (config target LogicalAnd/Or).
            bool const isAnd = (be->target == "LogicalAnd");
            bool const isOr  = (be->target == "LogicalOr");

            // Right operand precedence threshold (left-assoc: prec+1).
            std::int32_t const nextMin =
                (assoc == OperatorAssoc::Right) ? prec : prec + 1;

            if (isAnd || isOr) {
                // The LHS truthiness decides whether the RHS is EVALUATED. In an
                // already-unevaluated context, the RHS stays unevaluated too.
                bool aTrue = false;
                if (eval) {
                    auto aBool = asBool(lhs, /*allowFloat=*/false);
                    if (!aBool.has_value()) {
                        fail(DiagnosticCode::P_PreprocessorDirective,
                             "logical operand is not an integer in #if");
                        return std::nullopt;
                    }
                    aTrue = *aBool;
                }
                bool const shortCircuits = eval && (isAnd ? !aTrue : aTrue);
                // Parse the RHS, evaluating it only when this context evaluates
                // AND the LHS did not short-circuit (C 6.5.13/6.5.14).
                bool const rhsEval = eval && !shortCircuits;
                auto rhsOpt = parseExpr(nextMin, rhsEval);
                if (!rhsOpt.has_value()) return std::nullopt;
                if (!eval) { lhs = makeBoolLiteral(0); continue; }
                if (shortCircuits) {
                    lhs = makeBoolLiteral(aTrue ? 1 : 0);
                    continue;
                }
                auto bBool = asBool(*rhsOpt, /*allowFloat=*/false);
                if (!bBool.has_value()) {
                    fail(DiagnosticCode::P_PreprocessorDirective,
                         "logical operand is not an integer in #if");
                    return std::nullopt;
                }
                lhs = makeBoolLiteral(*bBool ? 1 : 0);
                continue;
            }

            // Plain arithmetic / bitwise / comparison operator.
            auto opK = opFromName(be->target);
            if (!opK.has_value()) {
                fail(DiagnosticCode::P_PreprocessorUnsupported,
                     "operator '" + be->target
                         + "' is not permitted in a #if expression");
                return std::nullopt;
            }
            auto rhsOpt = parseExpr(nextMin, eval);
            if (!rhsOpt.has_value()) return std::nullopt;
            if (!eval) { lhs = makeBoolLiteral(0); continue; }  // dummy; not folded
            // Both operands must be integers (no float in #if). `asIntBits`, not
            // `asInt64` -- see the unary gate above (D-PP-IF-UNSIGNED-INTMAX).
            if (!asIntBits(lhs).has_value() || !asIntBits(*rhsOpt).has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "non-integer operand in #if expression");
                return std::nullopt;
            }
            ConstEvalFailure why = ConstEvalFailure::None;
            EvalOptions opts;
            opts.refuseOnDivByZero       = true;
            opts.refuseOnShiftOutOfRange = true;
            // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: the char leaf already
            // resolved its own value, but the shared core still reads
            // `intKindInfo` for any Char-cored operand — carry the ONE answer so
            // the `#if` fold and the const-expr fold cannot diverge.
            opts.charIsUnsigned          = charIsUnsigned_;
            auto folded = applyBinaryInt(*opK, lhs, *rhsOpt, opts, why);
            if (!folded.has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     foldFailureMessage(why));
                return std::nullopt;
            }
            if (isComparison(*opK)) folded->core = TypeKind::Bool;
            lhs = std::move(*folded);
        }
        return lhs;
    }

    // `cond ? then : else` with the configured middle separator `:`. Only the
    // SELECTED arm is evaluated (C 6.5.15); the other arm is parsed unevaluated.
    [[nodiscard]] std::optional<HirLiteralValue>
    parseTernaryTail(HirLiteralValue cond, OperatorTable::Entry const& tern,
                     std::int32_t prec, bool eval) {
        bool condTrue = false;
        if (eval) {
            auto condBool = asBool(cond, /*allowFloat=*/false);
            if (!condBool.has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "#if ternary condition is not an integer");
                return std::nullopt;
            }
            condTrue = *condBool;
        }
        // Parse the THEN clause up to the middle separator (evaluated only when
        // this context evaluates AND the condition is true).
        auto thenOpt =
            parseExpr(std::numeric_limits<std::int32_t>::min(), eval && condTrue);
        if (!thenOpt.has_value()) return std::nullopt;
        // Expect the configured middle token (C's `:`).
        if (!tern.ternaryMiddle.has_value()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "ternary operator has no ':' separator configured");
            return std::nullopt;
        }
        if (atEnd() || peek().schemaKind.v != tern.ternaryMiddle->v) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "expected ':' in #if ternary expression");
            return std::nullopt;
        }
        advance();   // consume `:`
        // The ELSE clause binds at the ternary's own precedence (right-assoc).
        auto elseOpt = parseExpr(prec, eval && !condTrue);
        if (!elseOpt.has_value()) return std::nullopt;
        if (!eval) return makeBoolLiteral(0);   // dummy; arm not selected
        return condTrue ? std::move(*thenOpt) : std::move(*elseOpt);
    }

    // ── D-PP-IF-UNSIGNED-INTMAX: is this integer literal SIGNED? ─────────────
    //
    // Delegated whole to `preprocessorLiteralSignedness` -- the language's own
    // C 6.4.4.1 candidate ladder, run at C 6.10.1p4's phase-4 widths. The suffix
    // match, the radix classification and the candidate order all come from
    // `semantics.integerLiteralTyping`; nothing about which suffixes exist or
    // what they admit is known here.
    [[nodiscard]] std::optional<bool>
    literalSignedness(std::string_view text, std::uint64_t magnitude) {
        // A language that declares no ladder (toy / tsql) keeps the signed
        // reading it has always had -- now at intmax width rather than 32 bits.
        // The identity property: no ladder, no change in signedness.
        if (intLadder_.empty()) return true;

        auto const sgn = preprocessorLiteralSignedness(text, numberStyle_,
                                                       intLadder_, magnitude);
        if (!sgn.has_value()) {
            // No rule covers the matched suffix, or a candidate's signedness is
            // not model-invariant. The loader cross-checks both, so this is
            // substrate drift. The semantic tier ABORTS here; a preprocessor
            // reports instead -- but it still REFUSES rather than guessing a
            // signedness, because a guess selects a wrong branch in silence,
            // which is the entire defect this row exists to remove.
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "integer literal in #if matched no integerLiteralTyping rule "
                 "(config invariant violated): " + std::string{text});
            return std::nullopt;
        }
        return *sgn;
    }

    // ── D-PP-IF-LARGE-DECIMAL-LITERAL-HAS-NO-WARNING (C 6.10.1p4) ────────────
    // The literal was REINTERPRETED: a decimal, unsuffixed spelling whose every
    // ladder candidate is signed, taken as UNSIGNED because phase 4 has nothing
    // wider than intmax_t to put it in. Both references say so out loud, on by
    // default, and then evaluate exactly as DSS does — so this is a warning, not
    // a refusal, and the branch is unaffected.
    //
    // ★ THE CONDITION IS RE-DERIVED FROM THE LADDER'S OWN VERBS, never from a
    // hand-parsed suffix: `matchIntegerSuffix` and `integerLiteralIsPrefixed`
    // are the SAME two the ladder used to reach its answer, so "was this
    // reinterpreted" cannot drift from "what signedness did we use". A `u`-
    // suffixed or hexadecimal literal reaches unsigned through a rule that
    // ADMITS unsigned candidates — nothing was reinterpreted, and neither
    // reference warns (both measured; see the diagnostic's note).
    //
    // ⓘ A language with no ladder never gets here: `literalSignedness` returns
    // signed for that case and this predicate is false.
    void warnIfImplicitlyUnsigned(std::string_view text, bool isSigned,
                                  SourceSpan span) {
        if (isSigned) return;
        if (intLadder_.empty()) return;
        if (!matchIntegerSuffix(text, numberStyle_).empty()) return;  // suffixed
        if (integerLiteralIsPrefixed(text, numberStyle_)) return;     // non-decimal
        ParseDiagnostic d;
        d.code     = DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned;
        d.severity = DiagnosticSeverity::Warning;
        d.buffer   = diagBufferId_;
        d.span     = span;
        d.actual   = "integer constant '" + std::string{text}
                   + "' is too large for a signed intmax_t and is interpreted as "
                     "UNSIGNED in this #if (C 6.10.1p4). A comparison against a "
                     "negative operand therefore converts that operand to "
                     "uintmax_t and can select the opposite branch; add a 'u' "
                     "suffix to say so, or use a value that fits intmax_t.";
        rep_.report(std::move(d));
    }

    // primary := unary primary | '(' expr ')' | integer-literal | ident(=>0)
    [[nodiscard]] std::optional<HirLiteralValue> parsePrimary(bool eval) {
        if (atEnd()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "expected an operand in #if expression");
            return std::nullopt;
        }
        Token const t = peek();
        if (rejectIfUnsupported(t)) return std::nullopt;

        // Parenthesized sub-expression. The `(`/`)` are config tokens.
        if (isOpenParen(t)) {
            advance();
            auto inner = parseExpr(std::numeric_limits<std::int32_t>::min(), eval);
            if (!inner.has_value()) return std::nullopt;
            if (atEnd() || !isCloseParen(peek())) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "expected ')' in #if expression");
                return std::nullopt;
            }
            advance();
            return inner;
        }

        // Prefix unary operator (config unaryOps: !, ~, -).
        if (HirOperatorEntry const* ue = asUnary(t)) {
            // AddressOf/Deref are not ICE unary ops.
            if (ue->target == "AddressOf" || ue->target == "Deref") {
                fail(DiagnosticCode::P_PreprocessorUnsupported,
                     "operator '" + ue->target
                         + "' is not permitted in a #if expression");
                return std::nullopt;
            }
            advance();
            auto operand = parsePrimary(eval);
            if (!operand.has_value()) return std::nullopt;
            if (!eval) return makeBoolLiteral(0);   // dummy; not folded
            // Unary `+` (if a language declares it) is identity.
            if (ue->target == "Pos") return operand;
            auto opK = opFromName(ue->target);
            if (!opK.has_value()) {
                fail(DiagnosticCode::P_PreprocessorUnsupported,
                     "unary operator '" + ue->target
                         + "' is not permitted in a #if expression");
                return std::nullopt;
            }
            // "Is this an INTEGER?", not "does it fit an int64?"
            // (D-PP-IF-UNSIGNED-INTMAX). `asIntBits` is the verb `applyUnaryInt`
            // itself reads the operand with, so the gate cannot disagree with the
            // fold it guards; `asInt64` would refuse a legitimate `uintmax_t`
            // above INT64_MAX -- turning the wrong branch into a spurious refusal
            // instead of fixing it.
            if (!asIntBits(*operand).has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "non-integer operand in #if expression");
                return std::nullopt;
            }
            auto folded = applyUnaryInt(*opK, *operand);
            if (!folded.has_value()) {
                fail(DiagnosticCode::P_PreprocessorUnsupported,
                     "unary operator not foldable in #if expression");
                return std::nullopt;
            }
            return *folded;
        }

        // A fixed-value keyword literal (C23 true/false).
        if (lits_.fixedKinds.count(t.schemaKind.v) != 0) {
            for (auto const& [tok, val] : lits_.fixedValues) {
                if (tok == t.schemaKind.v) {
                    advance();
                    HirLiteralValue lv;
                    lv.core  = TypeKind::Bool;
                    lv.value = std::int64_t{val != 0 ? 1 : 0};
                    return lv;
                }
            }
        }

        // c12: char constant `'A'` / `'\n'` / `'\301'`. It lexes as the OPENER
        // (`charOpenKind_` = `'`) followed by the COALESCED body token
        // (`charBodyKind_`) and then the CLOSER (`charCloseKind_`) —
        // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN gave the closing `'` a token of
        // its own, so this arm consumes THREE tokens, not two. The body
        // text is the raw bytes between the quotes (escapes unresolved); decode it
        // to a single byte via the SHARED `decodeCharLiteralBody` (the same decoder
        // char-literal lowering uses — so an escape means the same thing here). The
        // value is the INT execution-charset code (C 6.10.1p4: char in `#if` is an
        // int; ASCII 'A' == 65). An empty / multi-byte / malformed body fails loud.
        if (charOpenKind_.valid() && t.schemaKind == charOpenKind_) {
            advance();   // consume the `'` opener
            if (atEnd() || !charBodyKind_.valid()
                || peek().schemaKind != charBodyKind_) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "malformed character constant in #if expression");
                return std::nullopt;
            }
            Token const bodyTok = peek();
            auto cp = decodeCharLiteralBody(textOf(bodyTok));
            if (!cp.has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "character constant in #if is empty, multi-character, or has "
                     "an unsupported escape: " + std::string{textOf(bodyTok)});
                return std::nullopt;
            }
            advance();   // consume the body
            // Consume the closing `'` token. Guarded on the kind MATCHING rather
            // than asserted: a language whose char body mode declares no closer
            // leaves `charCloseKind_` invalid and consumes nothing (unchanged
            // behaviour), and a genuinely unterminated `'a` never reaches here —
            // the tokenizer already failed loud with P_UnterminatedString. A
            // closer that is somehow absent still fails loud, one frame out, as
            // `evaluate()`'s trailing-token check.
            if (charCloseKind_.valid() && !atEnd()
                && peek().schemaKind == charCloseKind_) {
                advance();
            }
            // ── [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]] (the P0 arm) ────────
            // C 6.10.1p4 makes this an `int`; C 6.4.4.4p10 says WHICH int, and
            // it is not the code unit. `decodeCharLiteralBody` answers 0..255;
            // the constant's value is that unit read as a plain `char`, so
            // `'\xff'` is −1 where the target declares `char` signed. This arm
            // used to hand the raw unit straight to `intmaxOperand`, which made
            // `#if '\xff' < 0` take the `#else` arm on x86_64 — rc 0, zero
            // diagnostics — where gcc 13.3.0 and clang 18.1.3 both take the
            // `#if` arm. ✔MEASURED at b1f31420. The turn is the SHARED helper,
            // the same one `cst_const_eval` and `lowerCharLiteral` call, so the
            // three tiers cannot answer differently.
            //
            // A 0–127 body is the same integer under either signedness, which is
            // why a caller with no target still folds every real-world `#if 'a'`;
            // a high byte with no answer FAILS LOUD rather than picking one.
            if (narrowCharConstantSignednessMatters(*cp)
                && !charIsUnsigned_.has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "a character constant above 0x7F in #if has a "
                     "target-dependent value (C 6.2.5p15 leaves plain `char`'s "
                     "signedness implementation-defined) and no target was "
                     "supplied to this preprocessor run: "
                     + std::string{textOf(bodyTok)});
                return std::nullopt;
            }
            return intmaxOperand(
                static_cast<std::uint64_t>(narrowCharConstantValue(
                    *cp, charIsUnsigned_.value_or(false))),
                /*isSigned=*/true);
        }

        // Integer literal (real, or a synthetic `defined`-result).
        //
        // ── D-PP-IF-UNSIGNED-INTMAX ──────────────────────────────────────────
        // The literal's SIGNEDNESS is C 6.4.4.1's -- the first candidate in the
        // language's ordered ladder whose range holds the decoded magnitude,
        // keyed by suffix spelling and radix class. ★ ASK THE LADDER; DO NOT
        // PARSE THE SUFFIX HERE. `semantics.integerLiteralTyping` already owns
        // this question for the CST leaf, complete with a loader cross-check
        // that every declared `numberStyle` suffix is covered by exactly one
        // rule -- a second, hand-rolled suffix reader in the preprocessor would
        // be a parallel owner of "what type is this literal", free to drift.
        // This arm being the ONE site that did not consult it was the defect.
        //
        // ⚠ The WIDTH is then discarded and replaced by 64 (`intmaxOperand`),
        // because C 6.10.1p4 evaluates in intmax_t/uintmax_t rather than in the
        // literal's own type. Keeping the ladder's width would type `1` as a
        // 32-bit `int` -- 6.4.4.1's correct answer and phase 4's wrong one.
        bool const synthetic = has(t.flags, NodeFlags::Synthetic);
        if (synthetic || lits_.integer.count(t.schemaKind.v) != 0
            || t.coreKind == CoreTokenKind::IntLiteral) {
            std::string_view const text = textOf(t);
            auto iv = decodeInteger(text, numberStyle_);
            if (!iv.has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "malformed or out-of-range integer literal in #if "
                     "expression: " + std::string{text});
                return std::nullopt;
            }
            // The span must be taken BEFORE `advance()` — the warning below
            // positions on the LITERAL, and after the advance `peek()` is the
            // next token (the reference carets sit under the digits).
            SourceSpan const litSpan = t.span;
            advance();
            auto const signedness = literalSignedness(text, *iv);
            if (!signedness.has_value()) return std::nullopt;   // already reported
            warnIfImplicitlyUnsigned(text, *signedness, litSpan);
            return intmaxOperand(*iv, *signedness);
        }

        // Any other identifier that survived expansion -> 0 (C 6.10.1p4), which
        // that same paragraph then evaluates as a SIGNED intmax_t.
        if (isWordTok(t)) {
            advance();
            return intmaxOperand(0, /*isSigned=*/true);
        }

        fail(DiagnosticCode::P_PreprocessorDirective,
             "unexpected token in #if expression: " + std::string{textOf(t)});
        return std::nullopt;
    }

    // Config paren recognizers (the function-like open/close tokens double as
    // the grouping parens -- C uses the same `(`/`)`).
    [[nodiscard]] bool isOpenParen(Token const& t) const {
        SchemaTokenId const open =
            schema_.schemaTokens().find(schema_.preprocess().functionLikeOpenToken);
        return open.valid() && t.schemaKind == open;
    }
    [[nodiscard]] bool isCloseParen(Token const& t) const {
        SchemaTokenId const close =
            schema_.schemaTokens().find(schema_.preprocess().functionLikeCloseToken);
        return close.valid() && t.schemaKind == close;
    }

    [[nodiscard]] static std::string foldFailureMessage(ConstEvalFailure why) {
        switch (why) {
            case ConstEvalFailure::DivisionByZero:
                return "division by zero in #if expression";
            case ConstEvalFailure::ShiftCountOutOfRange:
                return "shift count out of range in #if expression";
            case ConstEvalFailure::Overflow:
                return "integer overflow in #if expression";
            default:
                return "could not evaluate #if expression";
        }
    }
};

} // namespace

// FC15c: `stripDunder` (the `__name__` dunder normalizer) now lives in the shared
// `core/types/attribute_naming.hpp` so the composite type-attribute scan
// (D-CSUBSET-PACKED) uses the SAME normalizer — the two can never drift. Behavior
// is byte-identical to the former local definition.

// FC15c: look up `attr` in the schema's KNOWN-attribute set, trying both the
// raw spelling and the dunder-stripped form. Returns the reported version int,
// or 0 when the attribute is not known (C23 6.10.1p4).
[[nodiscard]] std::int64_t cAttributeVersion(GrammarSchema const& schema,
                                             std::string_view     attr) {
    std::string_view const bare = stripDunder(attr);
    for (CAttributeDef const& ka : schema.preprocess().knownCAttributes) {
        if (ka.name == attr || ka.name == bare) {
            return static_cast<std::int64_t>(ka.version);
        }
    }
    return 0;
}

// ══ D-PP-HAS-EXTENSION-BUILTIN-ABSENT: THE FEATURE-QUERY ANSWER ══════════════
//
// ★★★ EVERY ARM READS AN ALREADY-DECLARED CAPABILITY SET. Nothing below is a
// list of names; each branch is a LOOKUP into the table that already owns the
// truth, chosen by the operator's own config-declared `answers` verb. That is
// what keeps `__has_builtin` from becoming a second, drifting copy of
// `semantics.builtinFunctions` — the defect the ruling names explicitly.
//
// ⚠ AN UNKNOWN CAPABILITY ANSWERS 0 AND IS NOT AN ERROR. ✔MEASURED on every
// reference that implements these operators (gcc 13.3.0, gcc 13.2.0 mingw,
// clang 18.1.3): a name nothing declares answers 0, silently. That is the
// CONTROL half of the operator, and it is the half a portable header depends on
// — the whole idiom is "ask about something you may not have".
[[nodiscard]] bool languageDeclaresAttribute(GrammarSchema const& schema,
                                             std::string_view     name) {
    std::string_view const bare = stripDunder(name);
    for (auto const& row : schema.semantics().attributeEffects) {
        for (auto const& n : row.names) {
            if (n == name || n == bare) return true;
        }
    }
    // The C23 standard attributes the language declares a VERSION for are
    // attributes it knows too; `__has_c_attribute` already answers from this
    // set, and an attribute in exactly one of the two tables is still an
    // attribute this implementation honours.
    for (CAttributeDef const& ka : schema.preprocess().knownCAttributes) {
        if (ka.name == name || ka.name == bare) return true;
    }
    return false;
}

[[nodiscard]] bool languageDeclaresBuiltin(GrammarSchema const& schema,
                                           std::string_view     name) {
    for (auto const& b : schema.semantics().builtinFunctions) {
        if (b.name == name) return true;
    }
    // The GNU compile-time builtins are declared as grammar KEYWORDS rather
    // than `builtinFunctions` rows (they are operators wearing a call's
    // punctuation — their operands are type-names, not values). The config
    // names their KINDS; the WORD is read back out of the language's own
    // keyword table here, so no spelling is stored twice and rebinding a
    // keyword moves this answer with it.
    if (schema.preprocess().builtinQueryKeywordTokens.empty()) return false;
    for (LexemeMeaning const& m : schema.lookupLexeme(name)) {
        for (std::string const& kindName :
             schema.preprocess().builtinQueryKeywordTokens) {
            SchemaTokenId const want = schema.schemaTokens().find(kindName);
            if (want.valid() && m.id == want) return true;
        }
    }
    return false;
}

[[nodiscard]] std::int64_t featureQueryAnswer(GrammarSchema const&     schema,
                                              FeatureQueryAnswerSource src,
                                              std::string_view         arg) {
    switch (src) {
        case FeatureQueryAnswerSource::DeclaredAttributes:
            return languageDeclaresAttribute(schema, arg) ? 1 : 0;
        case FeatureQueryAnswerSource::DeclaredBuiltins:
            return languageDeclaresBuiltin(schema, arg) ? 1 : 0;
        case FeatureQueryAnswerSource::DeclaredLanguageFeatures:
            for (LanguageFeatureDef const& f :
                 schema.preprocess().languageFeatures) {
                if (f.name == arg
                    && f.availability
                           == LanguageFeatureAvailability::Standard) {
                    return 1;
                }
            }
            return 0;
        case FeatureQueryAnswerSource::DeclaredLanguageExtensions:
            // The SUPERSET arm, ✔MEASURED as clang's own behaviour: every
            // feature is also an extension, so this reads the whole table.
            for (LanguageFeatureDef const& f :
                 schema.preprocess().languageFeatures) {
                if (f.name == arg) return 1;
            }
            return 0;
    }
    return 0;   // unreachable — every source handled above
}

// D-PP-DEFINED-VIA-MACRO-EXPANSION: read the RAW BYTES `[start, end)` of an
// operand that spans SEVERAL tokens -- an angle header name, whose spelling
// includes whatever sat between its tokens (`<a b.h>` is the header `a b.h`), so
// it cannot be rebuilt by concatenating token texts.
//
// ★★ THIS IS THE FUNCTION THAT REFUSES TO GUESS, and it exists because moving
// the `__has_include` fold PAST macro expansion is exactly what makes guessing
// possible. Before the move, both ends of the range always came from the same
// directive line. After it, the tokens can arrive from a replacement list, from
// the product tail, or from two different constructs spliced together -- and a
// range read across such a splice is not a malformed header name, it is a
// PLAUSIBLE one made of unrelated bytes, which the resolver would then answer
// confidently. That is a silent wrong answer, strictly worse than the refusal
// this change removed.
//
// Returns nullopt -- and EVERY caller then fails LOUD -- when the range cannot
// be read without guessing:
//   * the two ends live in DIFFERENT buffers (prefix vs product tail);
//   * the range runs backwards, or past the end of its buffer;
//   * the range CROSSES A LINE. A header name never does. A range that does is
//     the tell that expansion joined two constructs: the `<` from a `#define`
//     line and the `>` from the directive, with the whole file between them.
//
// Exported (declared in pp_if_eval.hpp) because the `#embed <resource>`
// directive reads its angle name through THIS function too -- D-PP-EMBED-ANGLE
// gave the directive the same refusal `__has_embed(<r>)` already had, rather
// than a second slicer that could disagree with it.
std::optional<std::string_view>
ppRawRun(ByteOffset start, ByteOffset end, SourceBuffer const& prefix,
         std::string_view tail) {
    if (end < start) return std::nullopt;
    const ByteOffset prefixLen = static_cast<ByteOffset>(prefix.text().size());
    std::string_view text;
    if (start < prefixLen) {
        if (end > prefixLen) return std::nullopt;   // straddles both buffers
        text = prefix.text().substr(start, end - start);
    } else {
        const ByteOffset s = start - prefixLen;
        const ByteOffset e = end - prefixLen;
        if (e > tail.size()) return std::nullopt;
        text = tail.substr(s, e - s);
    }
    if (text.find('\n') != std::string_view::npos) return std::nullopt;
    return text;
}

// ── D-PP-DEFINED-VIA-MACRO-EXPANSION: the shared `#if`-operand barrier ───────
// The state machine and the MEASUREMENT behind each arm are documented on the
// class (pp_if_eval.hpp). Every slot that does not match its expected shape
// returns to Idle WITHOUT protecting the token, so a malformed operator reaches
// the evaluator's rewrite unchanged and fails loud there (`defined ( 1 )` ->
// "operator 'defined' requires an identifier operand", which is what gcc, clang
// and MSVC all do for that input).
PpIfOperandBarrier::PpIfOperandBarrier(GrammarSchema const& schema) {
    PreprocessConfig const& pp = schema.preprocess();
    definedKw_    = pp.definedOperator;
    hasIncludeKw_ = pp.hasIncludeOperator;
    hasEmbedKw_   = pp.hasEmbedOperator;
    openParen_    = schema.schemaTokens().find(pp.functionLikeOpenToken);
    closeParen_   = schema.schemaTokens().find(pp.functionLikeCloseToken);
    angleOpen_    = schema.schemaTokens().find(pp.hasIncludeAngleOpenToken);
    angleClose_   = schema.schemaTokens().find(pp.hasIncludeAngleCloseToken);
    stringOpen_   = schema.schemaTokens().find(pp.quoteIncludeToken);
}

bool PpIfOperandBarrier::protects(Token const& t, std::string_view word) {
    lastWasDefinedKeyword_ = false;
    // Trivia sits BETWEEN an operator and its operand (`defined ( X )` may be
    // spelled with any amount of white space, and a macro replacement can put a
    // comment in the middle). It neither advances the state nor is protected --
    // trivia is not expandable, so protecting it would be a no-op that only made
    // the state machine harder to reason about.
    if (isTriviaTok(t)) return false;
    bool const isWordT = isWordTok(t);
    auto isKind = [&](SchemaTokenId k) {
        return k.valid() && t.schemaKind == k;
    };
    switch (state_) {
        case State::Idle:
            // A language declaring none of these operators leaves the spelling
            // EMPTY, and an empty spelling can never equal a Word's text, so the
            // whole machine is provably inert for it.
            if (isWordT && !definedKw_.empty() && word == definedKw_) {
                state_ = State::DefKeyword;
                lastWasDefinedKeyword_ = true;
            } else if (isWordT
                       && ((!hasIncludeKw_.empty() && word == hasIncludeKw_)
                           || (!hasEmbedKw_.empty() && word == hasEmbedKw_))) {
                state_ = State::HdrKeyword;
            }
            return false;   // the OPERATOR itself is never protected

        // ── `defined`: the operand is ALWAYS protected ──
        case State::DefKeyword:
            if (isKind(openParen_)) { state_ = State::DefOpen; return true; }
            state_ = State::Idle;
            return isWordT;   // the NO-PAREN form: `defined X`
        case State::DefOpen:
            if (isWordT) {
                state_ = State::DefOperand;
                return true;   // ★ THE PROTECTED OPERAND
            }
            state_ = State::Idle;
            return false;
        case State::DefOperand:
            state_ = State::Idle;
            return isKind(closeParen_);

        // ── `__has_include` / `__has_embed`: protected ONLY in the delimited
        // forms; anything else is macro-expanded and re-examined (C's own
        // `#include MACRO` rule, ✔MEASURED unanimous — see the class doc). ──
        case State::HdrKeyword:
            if (isKind(openParen_)) {
                state_        = State::HdrOpen;
                operandDepth_ = 0;
                return true;
            }
            state_ = State::Idle;
            return false;
        case State::HdrOpen:
            if (isKind(angleOpen_))  { state_ = State::InAngle; return true; }
            if (isKind(stringOpen_)) { state_ = State::InQuote; return true; }
            // ★ THE RE-EXAMINE ARM. Not protected on purpose: `#define H
            // <stdio.h>` + `#if __has_include(H)` answers 1 on all three
            // references, which it can only do if `H` expands here.
            state_ = State::Idle;
            return false;
        case State::InAngle:
            // Everything between the angle delimiters is part of the header
            // NAME and must reach the resolver as written -- `<HDR.h>` looks for
            // a header called `HDR.h` even when `HDR` is a macro.
            if (isKind(angleClose_)) { state_ = State::HdrOperand; return true; }
            return true;
        case State::InQuote:
            // A quote operand is the opener, ONE coalesced body token and the
            // closer, then (for `__has_embed`, C23 6.10.2p7) an embed-parameter
            // sequence whose clauses carry their own parens; none of it is
            // expanded, and the run ends at the operator's DEPTH-0 `)` -- so the
            // paren kinds are counted and no second spelling of the
            // literal-close token rule is needed.
            if (isKind(openParen_))  { ++operandDepth_; return true; }
            if (isKind(closeParen_)) {
                if (operandDepth_ == 0) { state_ = State::Idle; return true; }
                --operandDepth_;
                return true;
            }
            return true;
        case State::HdrOperand:
            // Past the angle name: the same depth-counted run to the operator's
            // own `)` -- `__has_embed(<r> limit(N))` holds `limit(N)` back
            // exactly as the quote form does (6.10.2p7 protects the WHOLE
            // parenthesized operand; only the `limit` clause is expanded, by the
            // evaluator itself, 6.10.4.2p3).
            if (isKind(openParen_))  { ++operandDepth_; return true; }
            if (isKind(closeParen_)) {
                if (operandDepth_ == 0) { state_ = State::Idle; return true; }
                --operandDepth_;
                return true;
            }
            return true;
    }
    return false;   // unreachable -- every State handled above
}

// ── C23 6.10.1p4–p9 / 6.10.4.1p2: the shared embed-parameter-sequence parser ──
// (contract on the declaration in pp_if_eval.hpp)
std::optional<PpEmbedParameterSequence>
parseEmbedParameterSequence(std::span<Token const> toks, std::size_t begin,
                            GrammarSchema const& schema,
                            PpTokenTextFn const& textOf, PpEmbedFail const& fail,
                            bool stopAtOperatorClose) {
    using Role = PreprocessConfig::EmbedParameterRole;
    PreprocessConfig const& pp = schema.preprocess();
    SchemaTokenId const openParen =
        schema.schemaTokens().find(pp.functionLikeOpenToken);
    SchemaTokenId const closeParen =
        schema.schemaTokens().find(pp.functionLikeCloseToken);
    // The `::` of a prefixed parameter, by CONFIG kind. Invalid when the
    // language declares no parameter surface: then no token is ever read as
    // the separator and `vendor::name` fails as "not an identifier" at `::`.
    SchemaTokenId const prefixSeparator =
        pp.embedParameters.has_value()
            ? schema.schemaTokens().find(pp.embedParameters->prefixSeparatorToken)
            : InvalidSchemaToken;
    auto isKind = [](Token const& t, SchemaTokenId k) {
        return k.valid() && t.schemaKind == k;
    };
    auto skip = [&](std::size_t i) {
        while (i < toks.size() && isTriviaTok(toks[i])) ++i;
        return i;
    };
    // The standard verb `name` is bound to, or nullopt. C23 6.10.1p5: a
    // standard parameter `x` and `__x__` "shall behave the same ... except for
    // the spelling", so BOTH sides are dunder-folded through the ONE shared
    // normaliser before comparing (a row may itself be spelled either way).
    auto roleOf = [&](std::string_view name) -> std::optional<Role> {
        if (!pp.embedParameters.has_value()) return std::nullopt;
        std::string_view const bare = stripDunder(name);
        for (PreprocessConfig::EmbedParameterDef const& def :
             pp.embedParameters->standard) {
            if (name == def.name || bare == stripDunder(def.name)) return def.role;
        }
        return std::nullopt;
    };

    PpEmbedParameterSequence seq;
    std::size_t i = skip(begin);
    while (i < toks.size()) {
        Token const& nameTok = toks[i];
        if (stopAtOperatorClose && isKind(nameTok, closeParen)) break;
        if (!isWordTok(nameTok)) {
            fail(nameTok, std::string{"#embed parameter must be an identifier "
                                      "(C23 6.10.1p4 pp-parameter); got '"}
                              + std::string{textOf(nameTok)} + "'");
            return std::nullopt;
        }
        PpEmbedParameter param;
        param.nameIndex = i;
        param.spelling  = std::string{textOf(nameTok)};
        std::size_t j = skip(i + 1);
        if (j < toks.size() && isKind(toks[j], prefixSeparator)) {
            // `identifier :: identifier` -- an implementation-defined
            // (prefixed) parameter. DSS defines none, so `role` stays empty;
            // the CALLER decides whether that is a violation (`#embed`,
            // 6.10.1p9) or the NOT_FOUND signal (`__has_embed`, 6.10.2p8).
            std::size_t const k = skip(j + 1);
            if (k >= toks.size() || !isWordTok(toks[k])) {
                fail(nameTok, std::string{"prefixed embed parameter '"}
                                  + param.spelling + "::' requires an identifier "
                                    "after '::' (C23 6.10.1p4 "
                                    "pp-prefixed-parameter)");
                return std::nullopt;
            }
            param.prefixed = true;
            param.spelling += std::string{textOf(toks[j])};
            param.spelling += std::string{textOf(toks[k])};
            j = skip(k + 1);
        } else {
            param.role = roleOf(param.spelling);
        }
        if (j < toks.size() && isKind(toks[j], openParen)) {
            // pp-parameter-clause: `( pp-balanced-token-sequence_opt )`,
            // balanced on the paren kinds; the clause's own `)` is the first
            // one at depth 0.
            param.hasClause   = true;
            param.clauseBegin = j + 1;
            int         depth  = 0;
            bool        closed = false;
            std::size_t k      = j + 1;
            for (; k < toks.size(); ++k) {
                if (isKind(toks[k], openParen)) { ++depth; continue; }
                if (isKind(toks[k], closeParen)) {
                    if (depth == 0) { closed = true; break; }
                    --depth;
                }
            }
            if (!closed) {
                fail(nameTok, std::string{"embed parameter '"} + param.spelling
                                  + "' has an unterminated '(' clause (C23 "
                                    "6.10.1p4 pp-parameter-clause)");
                return std::nullopt;
            }
            param.clauseEnd = k;
            j = skip(k + 1);
        }
        if (param.role.has_value()) {
            if (!param.hasClause) {
                fail(nameTok, std::string{"standard embed parameter '"}
                                  + param.spelling + "' requires a parenthesized "
                                    "clause (C23 6.10.4.2-6.10.4.5: \"shall be "
                                    "present\")");
                return std::nullopt;
            }
            if (seq.find(*param.role) != nullptr) {
                fail(nameTok, std::string{"standard embed parameter '"}
                                  + param.spelling + "' appears more than once "
                                    "(C23 6.10.4.2-6.10.4.5: \"may appear zero "
                                    "times or one time\")");
                return std::nullopt;
            }
        }
        seq.parameters.push_back(std::move(param));
        i = j;
    }
    seq.next = i;
    return seq;
}

// ── C23 6.10.4.2: the shared `limit` clause evaluator ───────────────────────
// (contract on the declaration in pp_if_eval.hpp)
std::optional<std::uint64_t>
evaluateEmbedLimit(std::span<Token const>   clause,
                   Token const&             anchor,
                   GrammarSchema const&     schema,
                   PpMacroExpand const&     macroExpand,
                   PpIsDefined const&       isDefined,
                   PpHasInclude const&      hasInclude,
                   SourceBuffer const&      synth,
                   PpProductText const&     productText,
                   DiagnosticReporter&      rep,
                   PpHasEmbed const&        hasEmbed,
                   PpOperatorRevoked const& operatorRevoked,
                   std::optional<bool>      charIsUnsigned,
                   PpTokenTextFn const&     textOf,
                   PpEmbedFail const&       fail) {
    std::string const& definedKw = schema.preprocess().definedOperator;
    // p2: "The token defined shall not appear within the constant expression."
    // Checked on the clause as WRITTEN and again on its EXPANSION: a macro that
    // produces `defined` is 6.10.2p13 undefined behaviour, refused rather than
    // folded.
    auto refusesDefined = [&](std::span<Token const> run, char const* where) {
        if (definedKw.empty()) return false;
        for (Token const& t : run) {
            if (isWordTok(t) && textOf(t) == definedKw) {
                fail(t, std::string{"the token '"} + definedKw
                            + "' shall not appear within an embed limit(...) "
                              "constant expression" + where
                            + " (C23 6.10.4.2p2)");
                return true;
            }
        }
        return false;
    };
    if (refusesDefined(clause, "")) return std::nullopt;
    std::vector<Token> raw;
    raw.reserve(clause.size());
    for (Token const& t : clause) {
        if (!isTriviaTok(t)) raw.push_back(t);
    }
    if (raw.empty()) {
        fail(anchor, "embed limit(...) requires an integer constant expression "
                     "(C23 6.10.4.2p1: the clause \"shall be present and have the "
                     "form ( constant-expression )\")");
        return std::nullopt;
    }
    // p3: "the constant expression is evaluated after the balanced
    // preprocessing token sequence is processed as in normal text" -- through
    // the caller's expander, which is the ONE MacroExpander engine.
    std::vector<Token> const expanded = macroExpand(raw);
    if (refusesDefined(expanded, " after macro expansion")) return std::nullopt;
    // p3 (cont.): "using the rules specified for conditional inclusion" -- the
    // `#if` evaluator itself, under an IDENTITY expander so the run above is not
    // expanded a second time (a self-referential macro would otherwise grow).
    PpMacroExpand const identity =
        [](std::vector<Token> const& run) { return run; };
    auto const value = evaluateIfExpressionValue(
        expanded, schema, identity, isDefined, hasInclude, synth, productText,
        rep, hasEmbed, operatorRevoked, charIsUnsigned);
    if (!value.has_value()) {
        fail(anchor, "embed limit(...) is not an integer constant expression "
                     "(C23 6.10.4.2p1); see the preceding diagnostic");
        return std::nullopt;
    }
    // p1: "shall not evaluate to a value less than 0". Only the SIGNED reading
    // can be negative; `limit(0u - 1)` is a legal, merely enormous, bound.
    if (value->isSigned && static_cast<std::int64_t>(value->bits) < 0) {
        fail(anchor, std::string{"embed limit(...) shall not evaluate to a value "
                                 "less than 0 (C23 6.10.4.2p1); got "}
                         + std::to_string(static_cast<std::int64_t>(value->bits)));
        return std::nullopt;
    }
    return value->bits;
}

std::optional<bool>
evaluateIfExpression(std::span<Token const> operandTokens,
                     GrammarSchema const&   schema,
                     PpMacroExpand const&   macroExpand,
                     PpIsDefined const&     isDefined,
                     PpHasInclude const&    hasInclude,
                     SourceBuffer const&    synth,
                     PpProductText const&   productText,
                     DiagnosticReporter&    rep,
                     PpHasEmbed const&      hasEmbed,
                     PpOperatorRevoked const& operatorRevoked,
                     std::optional<bool>    charIsUnsigned) {
    // C 6.10.2p12: the question asked of the value is whether it "evaluates to
    // nonzero" -- the value engine below is the whole implementation.
    auto const value = evaluateIfExpressionValue(
        operandTokens, schema, macroExpand, isDefined, hasInclude, synth,
        productText, rep, hasEmbed, operatorRevoked, charIsUnsigned);
    if (!value.has_value()) return std::nullopt;
    return value->bits != 0;
}

std::optional<PpIfValue>
evaluateIfExpressionValue(std::span<Token const> operandTokens,
                          GrammarSchema const&   schema,
                          PpMacroExpand const&   macroExpand,
                          PpIsDefined const&     isDefined,
                          PpHasInclude const&    hasInclude,
                          SourceBuffer const&    synth,
                          PpProductText const&   productText,
                          DiagnosticReporter&    rep,
                          PpHasEmbed const&      hasEmbed,
                          PpOperatorRevoked const& operatorRevoked,
                          std::optional<bool>    charIsUnsigned) {
    LiteralKinds const lits = gatherLiteralKinds(schema);
    // D-PP-HAS-EXTENSION-BUILTIN-ABSENT: every operator arm below is gated on
    // this. An operator the program has `#undef`'d is no longer an operator —
    // it is an ordinary undefined identifier, which the ICE parser's primary
    // folds to 0 (and which then fails LOUD in the function-like position,
    // exactly as gcc 13.3.0, gcc 13.2.0 and clang 18.1.3 do: "missing binary
    // operator before token '('" / "function-like macro '__has_include' is not
    // defined", both rc 1). Null-callback tolerant.
    auto operatorLive = [&](std::string_view word) {
        return !operatorRevoked || !operatorRevoked(word);
    };

    PreprocessConfig const& pp = schema.preprocess();
    SchemaTokenId const openParen =
        schema.schemaTokens().find(pp.functionLikeOpenToken);
    SchemaTokenId const closeParen =
        schema.schemaTokens().find(pp.functionLikeCloseToken);
    std::string const& definedKw = pp.definedOperator;
    // FC15c operator spellings (matched by TEXT, like `defined`) + the angle
    // delimiter token KINDS for `__has_include(<h>)` (matched by SCHEMA KIND,
    // never the `<`/`>` bytes -- the make-or-break agnosticism rule). All empty
    // when the language declares no such operator -> the arm never fires and the
    // identifier folds to 0 (the opt-out identity property). The string OPENER
    // (`"` -> StringStart) for the quote form is `quoteIncludeToken`.
    std::string const& hasIncludeKw = pp.hasIncludeOperator;
    std::string const& hasCAttrKw   = pp.hasCAttributeOperator;
    // FC17.9(h): `__has_embed` reuses the SAME angle-delimiter KINDS + quote
    // opener as `__has_include` (they are the language's angle/quote vocabulary),
    // matched by KIND never the `<`/`>`/`"` bytes.
    std::string const& hasEmbedKw   = pp.hasEmbedOperator;
    SchemaTokenId const angleOpen =
        schema.schemaTokens().find(pp.hasIncludeAngleOpenToken);
    SchemaTokenId const angleClose =
        schema.schemaTokens().find(pp.hasIncludeAngleCloseToken);
    SchemaTokenId const stringOpen =
        schema.schemaTokens().find(pp.quoteIncludeToken);

    // ══ Step 1: MACRO-EXPAND THE WHOLE OPERAND FIRST ═════════════════════════
    // D-PP-DEFINED-VIA-MACRO-EXPANSION. Expansion used to run SECOND, after the
    // operators had been folded off the raw directive line, and that order is
    // precisely what made an operator PRODUCED BY expansion unreachable. It now
    // runs first, with a `PpIfOperandBarrier` (pp_if_eval.hpp) holding back the
    // operands the references hold back — so every operator, however it was
    // produced, meets ONE rewrite pass below and they cannot answer differently.
    //
    // Trivia is dropped on the way in, exactly as the pre-expansion pass used to
    // drop it, so the expander sees the same run it always did.
    std::vector<Token> operandNoTrivia;
    operandNoTrivia.reserve(operandTokens.size());
    for (Token const& t : operandTokens) {
        if (!isTriviaTok(t)) operandNoTrivia.push_back(t);
    }
    std::vector<Token> const expanded = macroExpand(operandNoTrivia);

    // FC15b: a predefined / `#` / `##` PRODUCT materialized during the expansion
    // carries a span in the synth buffer's product TAIL (`[prefixLen + ..)`) --
    // bytes NOT present in the prefix-only `synth`. Read AFTER `macroExpand` and
    // BORROWED for the rest of this function: the rewrite below slices operand
    // spellings out of it and the ICE parser slices product tokens out of it.
    //
    // ★★★ D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION. This
    // used to assemble a COMBINED buffer per evaluation -- `std::string
    // combined{synth.text()}; combined.append(tail);` then a `SourceBuffer` around
    // it -- i.e. it COPIED THE ENTIRE TRANSLATION UNIT, twice, once for every
    // `#if`/`#elif` line evaluated after the TU's first `#`/`##`/predefined
    // product. ✔MEASURED 2026-08-25 on the 103-TU sqlite full-source corpus
    // (`--project … --config=release --jobs 1`): 13,242 evaluations copied
    // 12,424 MB and cost 10.05 s -- 10.0 s of the 13.3 s `preprocess-expand`
    // phase, on a build whose ENTIRE preprocessor cost 47 s. The cost is
    // QUADRATIC in the shape that matters: a bigger TU makes each copy bigger AND
    // gives it more `#if` lines to be copied at.
    //
    // ★ THE FIX IS NOT A CACHE OF THE COMBINED BUFFER -- it is to stop
    // materializing a concatenation nothing needs. A token's bytes already have
    // exactly one well-defined home, and `MacroExpander::text` (the expander's own
    // slicer) has always known the rule: a span at-or-past `prefixLen` is a
    // PRODUCT and slices the product tail at `start - prefixLen`; every other span
    // slices the prefix. `ppTokenText` applies that SAME rule for every reader
    // here, so none of them can drift -- and the per-`#if` copy is gone entirely.
    //
    // ⓘ `prefixLen` is not a new parameter because it is not new information:
    // `synth` IS the prefix buffer (`preprocess()` builds it from `synthText`
    // BEFORE appending `productText()`, and hands the same byte count to
    // `MacroExpander` as its `prefixLen`), so `synth.text().size()` IS that
    // length. Deriving it here rather than threading a second spelling of the
    // same number is what keeps the two from disagreeing.
    // NOT const: an embed `limit` clause inside a `__has_embed` operand is
    // macro-expanded DURING the rewrite below (6.10.4.2p3), and that nested
    // expansion may append to -- and reallocate -- the product tail this view
    // names. The arm that expands re-takes the view afterwards; every read that
    // may happen INSIDE the nested call goes through `freshWordOf` below, which
    // re-asks the provider each time and can never hold a stale view.
    std::string_view tail = productText ? productText() : std::string_view{};

    // ── Step 2: rewrite the conditional-inclusion OPERATORS to their values
    // (MF-1: the parens are the CONFIG function-like-open/close tokens, never
    // hard-coded). Each result is a synthetic IntLiteral token sliced from
    // `scratch` and tagged Synthetic. ──
    std::string scratchText;            // accumulates the synthetic digit bytes
    std::vector<Token> afterDefined;
    afterDefined.reserve(expanded.size());
    // The run this pass walks. Named so every helper below reads ONE source and
    // no line can accidentally reach back to the pre-expansion tokens.
    std::span<Token const> const toks{expanded};

    // Mint a Synthetic IntLiteral token whose decimal spelling is `digits`,
    // spanning the bytes appended to the scratch buffer (assembled after the
    // loop). Used for `defined`->0/1, `__has_include`->0/1, and
    // `__has_c_attribute`->version (a multi-digit value). The scratch slice
    // lets the value reach the ICE parser as a single Number token.
    auto mintNumber = [&](std::int64_t value) -> Token {
        std::string const digits = std::to_string(value);
        ByteOffset const start = static_cast<ByteOffset>(scratchText.size());
        scratchText.append(digits);
        ByteOffset const end = static_cast<ByteOffset>(scratchText.size());
        Token t;
        t.coreKind   = CoreTokenKind::IntLiteral;
        t.flags      = NodeFlags::Synthetic;
        t.schemaKind = InvalidSchemaToken;
        t.span       = SourceSpan::of(start, end);
        return t;
    };

    // Advance past trivia from `j`.
    auto skipFwd = [&](std::size_t j) {
        while (j < toks.size() && isTriviaTok(toks[j])) ++j;
        return j;
    };

    // Read a WORD token's spelling through the ONE slicing rule. `scratch` is
    // null because a Word is never a token this pass minted (those are
    // IntLiterals), so the synthetic arm is unreachable from here.
    auto wordOf = [&](Token const& t) -> std::string_view {
        return ppTokenText(t, synth, /*scratch=*/nullptr, tail);
    };

    // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: `body` indexes a coalesced literal
    // BODY token; return the index just past it AND past the CLOSE-delimiter
    // token that now follows it. The closer is a SIGNIFICANT token (the
    // tokenizer deliberately gives it no `EmptySpace` flag, so a shape can name
    // the slot), which means every `body + 1` step in the operator arms below
    // would otherwise land ON the closer instead of on the operator's `)`.
    // For `__has_include` that is the LOUD "expected ')'"; for `__has_embed` it
    // was SILENT — the closer counted as an unsupported PARAMETER and the
    // operator answered NOT_FOUND(0), so a guarded `#embed` never ran.
    // The closer KIND comes from the schema (keyed on the body token's own
    // kind), never the spelling `"StringEnd"`. Tolerant when the language
    // declares no closer: the index then simply steps past the body, which is
    // the pre-closer behaviour.
    auto skipBodyAndCloser = [&](std::size_t body) -> std::size_t {
        SchemaTokenId const closeKind =
            closeTokenForCoalescedBody(schema, toks[body].schemaKind);
        std::size_t n = body + 1;
        if (closeKind.valid() && n < toks.size()
            && toks[n].schemaKind == closeKind) {
            ++n;
        }
        return n;
    };

    bool rewriteFailed = false;
    // FC15c fail-loud helper for a malformed `__has_include` (positioned on the
    // operator token, a DISTINCT code -- never a generic ICE fallthrough).
    auto failHasInclude = [&](SourceSpan span, std::string msg) {
        emit(rep, DiagnosticCode::P_PreprocessorHasInclude, synth.id(), span,
             std::move(msg));
        rewriteFailed = true;
    };
    // FC17.9(h) fail-loud helper for a malformed `__has_embed` (positioned on the
    // operator token; the DISTINCT P_PreprocessorEmbed code -- never a generic ICE
    // fallthrough). An UNSUPPORTED PARAMETER is NOT a malformed shape -- it mints
    // NOT_FOUND(0) per C23, so it never routes here.
    auto failHasEmbed = [&](SourceSpan span, std::string msg) {
        emit(rep, DiagnosticCode::P_PreprocessorEmbed, synth.id(), span,
             std::move(msg));
        rewriteFailed = true;
    };

    // D-PP-DEFINED-VIA-MACRO-EXPANSION: a token produced by macro expansion
    // carries a PRODUCT-TAIL span (at or past the prefix buffer's end) or a
    // SCRATCH span, and neither is a position in the registered buffer the
    // diagnostic names. Attribute those to the directive's OWN first operand
    // token -- which is where gcc and clang position the very same error
    // (`#if HAS_FOO` for a `HAS_FOO` that expands to a malformed `defined`).
    auto diagSpanFor = [&](Token const& t) -> SourceSpan {
        const ByteOffset prefixLen = static_cast<ByteOffset>(synth.text().size());
        if (!has(t.flags, NodeFlags::Synthetic) && t.span.start() < prefixLen) {
            return t.span;
        }
        return operandTokens.empty() ? SourceSpan::empty(0)
                                     : operandTokens.front().span;
    };

    // ★★★ THE `defined` OPERATOR — ONE IMPLEMENTATION, ONE REWRITE POINT, AND IT
    // IS *AFTER* EXPANSION (D-PP-DEFINED-VIA-MACRO-EXPANSION requirement (2):
    // one operator, one evaluation, never a second copy for the expanded case).
    //
    // ★ WHY AFTER, WHICH IS THE WHOLE ORDERING DESIGN. `defined` used to be
    // folded BEFORE expansion, on the raw directive line, and that order is what
    // made a macro-produced `defined` unreachable. Moving the fold AFTER
    // expansion — with the operand protected DURING expansion by
    // `PpIfOperandBarrier` — is the reference toolchains' own order: gcc, clang and
    // MSVC all read the `#if` expression through a macro-expanding reader and
    // recognise `defined` in the token stream that reader produces, whichever
    // construct produced it. Both forms then take the SAME path, so they cannot
    // answer differently.
    // ✔MEASURED, and this is what the ordering buys beyond the row: with the
    // pre-expansion fold, `#if ID(defined(FOO))` (a LITERAL `defined` inside a
    // function-like macro's ARGUMENT list) was folded before `ID` was ever
    // invoked, so DSS answered 1 and compiled. gcc 13.3.0, clang 18.1.3 AND MSVC
    // 19.51 ALL REJECT it (`operator "defined" requires an identifier` / `macro
    // name must be an identifier` / `C2004: expected 'defined(id)'`) — the
    // argument is pre-expanded to `defined(1)` first. Accepting what NOT ONE
    // reference accepts is §A.3b's other direction, and the single rewrite point
    // closes it by construction rather than by a second special case.
    //
    // `toks[i]` is the `defined` keyword. On a well-formed shape the 1/0 result
    // is appended to `out`, `i` advances past the whole construct, true is
    // returned. On a malformed shape the positioned diagnostic is emitted,
    // `rewriteFailed` is set and false is returned. `tail` is the product-tail
    // view the operand's spelling may live in (empty before expansion).
    auto foldDefined = [&](std::span<Token const> toks, std::size_t& i,
                           std::string_view tail,
                           std::vector<Token>& out) -> bool {
        Token const kwTok = toks[i];
        auto skip = [&](std::size_t k) {
            while (k < toks.size() && isTriviaTok(toks[k])) ++k;
            return k;
        };
        std::size_t j = skip(i + 1);
        bool paren = false;
        if (j < toks.size() && openParen.valid()
            && toks[j].schemaKind == openParen) {
            paren = true;
            j = skip(j + 1);
        }
        if (j >= toks.size() || !isWordTok(toks[j])) {
            emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                 diagSpanFor(kwTok),
                 "operator 'defined' requires an identifier operand");
            rewriteFailed = true;
            return false;
        }
        std::string const name{ppTokenText(toks[j], synth, nullptr, tail)};
        ++j;
        if (paren) {
            j = skip(j);
            if (j >= toks.size() || closeParen.valid() == false
                || toks[j].schemaKind != closeParen) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     diagSpanFor(kwTok),
                     "expected ')' after 'defined(' in #if expression");
                rewriteFailed = true;
                return false;
            }
            ++j;
        }
        out.push_back(mintNumber(isDefined(name) ? 1 : 0));
        i = j;
        return true;
    };

    // ★★ THE SINGLE OPERATOR-REWRITE PASS, over the EXPANDED run.
    // D-PP-DEFINED-VIA-MACRO-EXPANSION. Every conditional-inclusion operator is
    // folded HERE and nowhere else, so a `defined` / `__has_include` /
    // `__has_embed` / `__has_c_attribute` written in the directive and one
    // PRODUCED BY macro expansion are the same token to this loop and cannot
    // answer differently.
    //
    // ⚠ THE BARRIER RUNS AGAIN HERE, and it is not redundant with the one the
    // expander ran. It answers a DIFFERENT question in this pass: which tokens
    // are somebody's OPERAND and therefore must not be read as an operator in
    // their own right. `#if defined(__has_include)` asks whether the NAME is
    // defined — it does not invoke the operator (the shape
    // `examples/c/has_include_operator_not_shadowable` exists for). Reusing the
    // SAME class is what makes the expander's notion of an operand and this
    // pass's notion identical by construction rather than by review.
    PpIfOperandBarrier operandBarrier{schema};
    for (std::size_t i = 0; i < toks.size(); ) {
        Token const& t = toks[i];
        if (isTriviaTok(t)) { ++i; continue; }
        std::string_view const word =
            isWordTok(t) ? wordOf(t) : std::string_view{};
        if (operandBarrier.protects(t, word)) {
            afterDefined.push_back(t);
            ++i;
            continue;
        }

        // `defined X` / `defined(X)` -> 1/0. Both the literal form and the one
        // that arrived via a replacement list reach this arm; the operand
        // survived expansion intact because the expander ran with the same
        // barrier (see the class doc for the per-operator measurement).
        if (isWordTok(t) && !definedKw.empty() && word == definedKw) {
            if (!foldDefined(toks, i, tail, afterDefined)) break;
            continue;
        }

        // ── FC15c: `__has_include(<h>)` / `__has_include("h")` (C23 6.10.1p4).
        // The operand is NOT macro-expanded (like `defined`); the angle
        // delimiters are matched by CONFIG token KIND, never the `<`/`>` bytes.
        // EVERY malformed shape fails loud with P_PreprocessorHasInclude. ──
        if (isWordTok(t) && !hasIncludeKw.empty() && word == hasIncludeKw
            && operatorLive(word)) {
            std::size_t j = skipFwd(i + 1);
            if (j >= toks.size() || !openParen.valid()
                || toks[j].schemaKind != openParen) {
                failHasInclude(diagSpanFor(t),
                    "operator '__has_include' requires a parenthesized header");
                break;
            }
            j = skipFwd(j + 1);
            std::string filename;
            bool isAngle = false;
            if (j < toks.size() && angleOpen.valid()
                && toks[j].schemaKind == angleOpen) {
                // ANGLE form `<h>`: the raw filename is the bytes between the
                // angle-open and angle-close tokens (matched by KIND). Scan to
                // the close token, accumulating the spelling of the interior
                // tokens (a header name like `stdio.h` lexes as several tokens).
                isAngle = true;
                std::size_t k = j + 1;
                ByteOffset const innerStart =
                    toks[j].span.end();   // just past `<`
                bool sawClose = false;
                ByteOffset innerEnd = innerStart;
                for (; k < toks.size(); ++k) {
                    if (angleClose.valid()
                        && toks[k].schemaKind == angleClose) {
                        sawClose = true;
                        break;
                    }
                    innerEnd = toks[k].span.end();
                }
                if (!sawClose) {
                    failHasInclude(diagSpanFor(t),
                        "expected '>' to close '__has_include(<...'");
                    break;
                }
                // The filename verbatim, escapes NOT decoded (the include
                // resolver reads the raw path too).
                // D-PP-DEFINED-VIA-MACRO-EXPANSION:
                // `ppRawRun` REFUSES a range it cannot read without
                // guessing -- one that straddles the prefix and the product
                // tail, or that crosses a line because expansion spliced two
                // constructs together. A guessed header name is a PLAUSIBLE one
                // made of unrelated bytes, which the resolver would answer
                // confidently; that is the silent wrong answer this operator's
                // refusal used to prevent, and it stays prevented.
                if (innerEnd > innerStart) {
                    auto raw = ppRawRun(innerStart, innerEnd, synth, tail);
                    if (!raw.has_value()) {
                        failHasInclude(diagSpanFor(t),
                            "operator '__has_include' header name could not be "
                            "read as one contiguous run after macro expansion");
                        break;
                    }
                    filename = std::string{*raw};
                }
                j = k + 1;   // past the close token
            } else if (j < toks.size() && stringOpen.valid()
                       && toks[j].schemaKind == stringOpen) {
                // QUOTE form `"h"`: the StringStart opener consumed only the
                // opening `"`; the coalesced StringLiteral BODY is the next token
                // and its raw text is the filename (escapes NOT decoded, like the
                // include resolver). An empty body (`""`) leaves filename empty.
                // The body is FOLLOWED by the literal's CLOSE-delimiter token
                // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN); without stepping past
                // it the `)` check below sees the `"` and fails "expected ')'".
                std::size_t k = j + 1;
                if (k < toks.size() && !isTriviaTok(toks[k])
                    && toks[k].span.start()
                           == toks[j].span.end()) {
                    filename = std::string{wordOf(toks[k])};
                    k = skipBodyAndCloser(k);
                }
                j = k;
            } else {
                failHasInclude(diagSpanFor(t),
                    "operator '__has_include' requires <header> or \"header\"");
                break;
            }
            if (filename.empty()) {
                failHasInclude(diagSpanFor(t),
                    "operator '__has_include' has an empty header name");
                break;
            }
            j = skipFwd(j);
            if (j >= toks.size() || !closeParen.valid()
                || toks[j].schemaKind != closeParen) {
                failHasInclude(diagSpanFor(t),
                    "expected ')' to close '__has_include('");
                break;
            }
            ++j;
            bool const found = hasInclude && hasInclude(filename, isAngle);
            afterDefined.push_back(mintNumber(found ? 1 : 0));
            i = j;
            continue;
        }

        // ── FC17.9(h): `__has_embed(<r>)` / `__has_embed("r")` (C23 6.10.2p7).
        // Mirror the `__has_include` extraction (operand NOT macro-expanded; the
        // angle delimiters + quote opener matched by CONFIG token KIND), then
        // read the embed-parameter-sequence through the ONE parser the
        // directive uses, evaluate a `limit` through the ONE evaluator the
        // directive uses, and mint the C23 TRICHOTOMY 0/1/2 from the width the
        // directive would embed (D-PP-EMBED-PARAMS / -ANGLE). Malformed shapes
        // (no `(`, empty name, unterminated, a token that is not a
        // pp-parameter, a duplicate or clause-less standard parameter, a bad
        // `limit`) fail loud P_PreprocessorEmbed; an unrecognized PREFIXED
        // parameter is NOT an error -- it is the NOT_FOUND signal (6.10.1p9
        // footnote 196, 6.10.2p7 + p8 NOTE 1), and it is answered BEFORE the
        // `limit` clause is evaluated. An unknown STANDARD-SHAPED name gets no
        // such exemption and is loud here exactly as on a `#embed` line. ──
        if (isWordTok(t) && !hasEmbedKw.empty() && word == hasEmbedKw
            && operatorLive(word)) {
            std::size_t j = skipFwd(i + 1);
            if (j >= toks.size() || !openParen.valid()
                || toks[j].schemaKind != openParen) {
                failHasEmbed(diagSpanFor(t),
                    "operator '__has_embed' requires a parenthesized resource");
                break;
            }
            j = skipFwd(j + 1);
            std::string filename;
            bool isAngle = false;
            if (j < toks.size() && angleOpen.valid()
                && toks[j].schemaKind == angleOpen) {
                // ANGLE form `<r>`: accumulate the raw bytes between the angle
                // delimiters (matched by KIND). The callback runs the angle
                // search the directive runs (C23 6.10.4.1p8).
                isAngle = true;
                std::size_t k = j + 1;
                ByteOffset const innerStart = toks[j].span.end();
                bool sawAngleClose = false;
                ByteOffset innerEnd = innerStart;
                for (; k < toks.size(); ++k) {
                    if (angleClose.valid()
                        && toks[k].schemaKind == angleClose) {
                        sawAngleClose = true;
                        break;
                    }
                    innerEnd = toks[k].span.end();
                }
                if (!sawAngleClose) {
                    failHasEmbed(diagSpanFor(t),
                        "expected '>' to close '__has_embed(<...'");
                    break;
                }
                // D-PP-DEFINED-VIA-MACRO-EXPANSION: same refusal as the
                // `__has_include` angle arm -- `ppRawRun` will not guess a
                // resource name out of bytes that expansion spliced together.
                if (innerEnd > innerStart) {
                    auto raw = ppRawRun(innerStart, innerEnd, synth, tail);
                    if (!raw.has_value()) {
                        failHasEmbed(diagSpanFor(t),
                            "operator '__has_embed' resource name could not be "
                            "read as one contiguous run after macro expansion");
                        break;
                    }
                    filename = std::string{*raw};
                }
                j = k + 1;
            } else if (j < toks.size() && stringOpen.valid()
                       && toks[j].schemaKind == stringOpen) {
                // QUOTE form `"r"`: the coalesced StringLiteral BODY is the
                // adjacent next token; its raw text is the resource name (escapes
                // NOT decoded, like the include/embed resolver). An empty body
                // (`""`) leaves the name empty -> loud below. The body is then
                // FOLLOWED by the literal's CLOSE-delimiter token
                // (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN) — stepping past it here
                // is what keeps the parameter scan below from counting the `"` as
                // a parameter and SILENTLY answering NOT_FOUND.
                std::size_t k = j + 1;
                if (k < toks.size() && !isTriviaTok(toks[k])
                    && toks[k].span.start()
                           == toks[j].span.end()) {
                    filename = std::string{wordOf(toks[k])};
                    k = skipBodyAndCloser(k);
                }
                j = k;
            } else {
                failHasEmbed(diagSpanFor(t),
                    "operator '__has_embed' requires <resource> or \"resource\"");
                break;
            }
            if (filename.empty()) {
                failHasEmbed(diagSpanFor(t),
                    "operator '__has_embed' has an empty resource name");
                break;
            }
            // ── C23 6.10.2p7: the embed-parameter-sequence, through the ONE
            // parser the directive uses, so the operator and `#embed` read a
            // sequence identically. `freshWordOf` re-asks the product provider
            // on EVERY read: the `limit` clause is macro-expanded below and that
            // may grow (and move) the tail `wordOf` holds a view of. ──
            PpTokenTextFn const freshWordOf =
                [&](Token const& u) -> std::string_view {
                return ppTokenText(u, synth, /*scratch=*/nullptr,
                                   productText ? productText()
                                               : std::string_view{});
            };
            PpEmbedFail const failParam = [&](Token const& at, std::string msg) {
                failHasEmbed(diagSpanFor(at), std::move(msg));
            };
            auto const seq = parseEmbedParameterSequence(
                toks, j, schema, freshWordOf, failParam,
                /*stopAtOperatorClose=*/true);
            if (!seq.has_value()) break;   // reported through failParam
            j = seq->next;
            if (j >= toks.size() || !closeParen.valid()
                || toks[j].schemaKind != closeParen) {
                failHasEmbed(diagSpanFor(t), "expected ')' to close '__has_embed('");
                break;
            }
            ++j;   // past the operator's close paren
            // ── ★★ THE TWO UNSUPPORTED VERDICTS, IN THE ORDER C23 FIXES, AND
            // BOTH BEFORE `limit` IS EVALUATED. ──
            //
            // 6.10.1p9 footnote 196 carves out exactly ONE shape: "An
            // unrecognized preprocessor PREFIXED parameter is a constraint
            // violation, EXCEPT within has_embed expressions". So an unknown
            // STANDARD-SHAPED name still owes a diagnostic here, exactly as on
            // a `#embed` line -- there is nothing in 6.10.2 that exempts it.
            if (PpEmbedParameter const* unknown =
                    seq->firstUnsupportedStandardShaped()) {
                std::string declared;
                if (pp.embedParameters.has_value()) {
                    for (PreprocessConfig::EmbedParameterDef const& def :
                         pp.embedParameters->standard) {
                        if (!declared.empty()) declared += ", ";
                        declared += def.name;
                    }
                }
                if (declared.empty()) declared = "none declared by this language";
                failParam(toks[unknown->nameIndex],
                          std::string{"'"} + unknown->spelling
                              + "' is not a standard embed parameter (C23 "
                                "6.10.1p9; footnote 196 exempts only a PREFIXED "
                                "`vendor::name` inside __has_embed. The standard "
                                "parameters are: "
                              + declared + ")");
                break;
            }
            // ⚠ AND NOW THE SHORT-CIRCUIT, WHICH MUST PRECEDE THE `limit`
            // EVALUATION. p7: NOT_FOUND(0) "if the search fails or if any of the
            // embed parameters ... are not supported by the implementation";
            // p8 NOTE 1: such a parameter is "not a constraint violation and
            // instead cause[s] the expression to be evaluated to 0". Evaluating
            // a sibling `limit` first would refuse a translation unit the
            // standard requires to answer 0 and take its `#elif` -- C23 6.10.2
            // EXAMPLE 5's vendor guard (`ds9000::element_type(short)`) crossed
            // with EXAMPLE 6's `limit` is exactly that program.
            // ★ WHY SKIPPING THE CLAUSE IS CONFORMING, not a dropped check: p7
            // imposes only the SYNTACTIC requirements of a `#embed` directive on
            // the notional directive, and `parseEmbedParameterSequence` above
            // has already enforced every one of them (pp-parameter shape,
            // balanced and terminated clause, a required clause on a standard
            // parameter, at most one of each). A limit's VALUE (6.10.4.2p1
            // "shall not evaluate to a value less than 0", p2 "The token
            // defined shall not appear") is a CONSTRAINT, not syntax, so it is
            // evaluated only for a sequence this implementation supports --
            // where it stays loud, as the directive's own arm does.
            if (seq->anyUnsupportedPrefixed()) {
                afterDefined.push_back(mintNumber(0));
                i = j;
                continue;
            }
            // 6.10.2 EXAMPLE 6 + 6.10.4.2: `limit` decides EMPTY "including in
            // __has_embed expressions", so its clause is evaluated here by the
            // shared evaluator -- a constraint violation in it is loud, exactly
            // as on the directive.
            std::optional<std::uint64_t> limit;
            if (PpEmbedParameter const* lim =
                    seq->find(PreprocessConfig::EmbedParameterRole::Limit)) {
                limit = evaluateEmbedLimit(
                    toks.subspan(lim->clauseBegin,
                                 lim->clauseEnd - lim->clauseBegin),
                    toks[lim->nameIndex], schema, macroExpand, isDefined,
                    hasInclude, synth, productText, rep, hasEmbed,
                    operatorRevoked, charIsUnsigned, freshWordOf, failParam);
                // The nested expansion may have grown (and moved) the tail.
                tail = productText ? productText() : std::string_view{};
                if (!limit.has_value()) break;   // reported through failParam
            }
            // p7: every parameter here is supported, so the answer is the SEARCH
            // and the WIDTH -- NOT_FOUND(0) if the search fails; else EMPTY(2)
            // iff the width after `limit` is zero, else FOUND(1). The width
            // formula is the directive's own (`embedResourceWidthBytes`), so
            // the two can never disagree about emptiness.
            int value = 0;
            {
                std::optional<std::uint64_t> const implementationBytes =
                    hasEmbed ? hasEmbed(filename, isAngle, diagSpanFor(t))
                             : std::nullopt;
                if (implementationBytes.has_value()) {
                    value = embedResourceWidthBytes(*implementationBytes, limit)
                                    == 0
                                ? 2
                                : 1;
                }
            }
            afterDefined.push_back(mintNumber(value));
            i = j;
            continue;
        }

        // ── FC15c: `__has_c_attribute(attr)` (C23 6.10.1p4). Match the operator
        // by TEXT, extract the attr NAME (a Word), look it up in the config's
        // known-attribute set (raw + dunder-stripped), mint the version or 0. ──
        if (isWordTok(t) && !hasCAttrKw.empty() && word == hasCAttrKw
            && operatorLive(word)) {
            std::size_t j = skipFwd(i + 1);
            if (j >= toks.size() || !openParen.valid()
                || toks[j].schemaKind != openParen) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     diagSpanFor(t),
                     "operator '__has_c_attribute' requires a parenthesized "
                     "attribute name");
                rewriteFailed = true;
                break;
            }
            j = skipFwd(j + 1);
            if (j >= toks.size() || !isWordTok(toks[j])) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     diagSpanFor(t),
                     "operator '__has_c_attribute' requires an attribute name");
                rewriteFailed = true;
                break;
            }
            std::string const attr{wordOf(toks[j])};
            j = skipFwd(j + 1);
            if (j >= toks.size() || !closeParen.valid()
                || toks[j].schemaKind != closeParen) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     diagSpanFor(t),
                     "expected ')' to close '__has_c_attribute('");
                rewriteFailed = true;
                break;
            }
            ++j;
            afterDefined.push_back(mintNumber(cAttributeVersion(schema, attr)));
            i = j;
            continue;
        }

        // ── ★★★ D-PP-HAS-EXTENSION-BUILTIN-ABSENT: the FEATURE-QUERY family —
        // `__has_attribute` / `__has_builtin` / `__has_feature` /
        // `__has_extension` (operator ruling 2026-09-03).
        //
        // ONE arm for all four, because they differ only in WHICH declared
        // capability set the answer is read from — which is `answers`, config
        // DATA on the operator's own row. A per-operator arm here would be four
        // copies of the same shape, and the fourth would be the one that drifts.
        //
        // The SHAPE is `__has_c_attribute`'s, deliberately: operator word, `(`,
        // one identifier, `)`. ✔MEASURED, that is what all three implementing
        // references accept, and a malformed shape fails LOUD on all three
        // rather than folding to 0 — silence here would be a CLAIM ("I checked,
        // and no") where an error is an admission ("I cannot read this").
        if (isWordTok(t)) {
            FeatureQueryOperatorDef const* fq =
                findFeatureQueryOperator(word, pp);
            if (fq != nullptr && operatorLive(word)) {
                std::size_t j = skipFwd(i + 1);
                if (j >= toks.size() || !openParen.valid()
                    || toks[j].schemaKind != openParen) {
                    emit(rep, DiagnosticCode::P_PreprocessorDirective,
                         synth.id(), diagSpanFor(t),
                         "operator '" + fq->name
                             + "' requires a parenthesized name");
                    rewriteFailed = true;
                    break;
                }
                j = skipFwd(j + 1);
                if (j >= toks.size() || !isWordTok(toks[j])) {
                    emit(rep, DiagnosticCode::P_PreprocessorDirective,
                         synth.id(), diagSpanFor(t),
                         "operator '" + fq->name + "' requires a name");
                    rewriteFailed = true;
                    break;
                }
                std::string const queried{wordOf(toks[j])};
                j = skipFwd(j + 1);
                if (j >= toks.size() || !closeParen.valid()
                    || toks[j].schemaKind != closeParen) {
                    emit(rep, DiagnosticCode::P_PreprocessorDirective,
                         synth.id(), diagSpanFor(t),
                         "expected ')' to close '" + fq->name + "('");
                    rewriteFailed = true;
                    break;
                }
                ++j;
                afterDefined.push_back(
                    mintNumber(featureQueryAnswer(schema, fq->answers, queried)));
                i = j;
                continue;
            }
        }

        afterDefined.push_back(t);
        ++i;
    }
    if (rewriteFailed) return std::nullopt;

    // Everything from here reads ONE rewritten run. There is no second
    // expansion and no second operator pass: the expansion happened at the top,
    // the operators were folded once above, and `tail` was taken between them.

    // Assemble the scratch buffer NOW that scratchText is final -- the rewrite
    // pass has minted into it and the synthetic tokens' spans index into it.
    // `SourceBuffer::fromString` copies the text.
    auto scratchBuf = SourceBuffer::fromString(scratchText, "<pp-if-scratch>");

    // ── Step 3: drop trivia, then parse + fold (a surviving identifier ->
    // 0 happens inside the parser's primary). ──
    std::vector<Token> nonTrivia;
    nonTrivia.reserve(afterDefined.size());
    for (Token const& t : afterDefined) {
        if (!isTriviaTok(t)) nonTrivia.push_back(t);
    }

    IceParser parser{std::move(nonTrivia), schema, synth,    *scratchBuf, lits,
                     rep,                  synth.id(), tail, charIsUnsigned};
    return parser.evaluateValue();
}

} // namespace dss
