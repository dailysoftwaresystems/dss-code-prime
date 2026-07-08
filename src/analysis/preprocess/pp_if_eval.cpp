#include "analysis/preprocess/pp_if_eval.hpp"

#include "core/types/attribute_naming.hpp"   // stripDunder (shared with the packed scan)
#include "core/types/char_decode.hpp"
#include "core/types/hir_lowering_config.hpp"
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
using detail::makeBoolLiteral;

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
            || k == TypeKind::F128) {
            lk.floating.insert(tok);
        } else {
            // Char / I8 / U8 / Bool / I16 / I32 / ... -> integer literal.
            // (c-subset maps CharLiteral -> I32, so a char constant in `#if` is
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
              BufferId diagBufferId)
        : toks_(std::move(toks)),
          schema_(schema),
          synth_(synth),
          scratch_(scratch),
          // FC15b: diagnostics attribute to the ORIGINAL prefix synth buffer
          // (`diagBufferId`), even when real tokens are sliced against a COMBINED
          // (prefix + product) buffer whose id differs. A real token's span is a
          // valid PREFIX offset, so it positions correctly under either id; using
          // the prefix id keeps the diagnostic on the buffer `preprocess()`
          // remaps (the combined buffer is local + unregistered).
          diagBufferId_(diagBufferId),
          lits_(lits),
          rep_(rep),
          binaryOps_(schema.hirLowering().binaryOps),
          unaryOps_(schema.hirLowering().unaryOps),
          opTable_(schema.operatorTable()),
          numberStyle_(schema.numberStyle()) {
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
    }

    // Evaluate the whole token run. nullopt on any fail-loud condition (already
    // reported). A TRAILING unconsumed token is malformed.
    [[nodiscard]] std::optional<std::int64_t> evaluate() {
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
        auto iv = asInt64(*v);
        if (!iv.has_value()) {
            fail(DiagnosticCode::P_PreprocessorDirective,
                 "#if expression is not an integer constant");
            return std::nullopt;
        }
        return *iv;
    }

private:
    std::vector<Token>            toks_;
    GrammarSchema const&          schema_;
    SourceBuffer const&           synth_;
    SourceBuffer const&           scratch_;
    BufferId                      diagBufferId_{};
    LiteralKinds const&           lits_;
    DiagnosticReporter&           rep_;
    std::vector<HirOperatorEntry> const& binaryOps_;
    std::vector<HirOperatorEntry> const& unaryOps_;
    OperatorTable const&          opTable_;
    NumberStyle const*            numberStyle_ = nullptr;
    SchemaTokenId                 stringOpenKind_{};
    SchemaTokenId                 charOpenKind_{};   // c12: `'` opener
    SchemaTokenId                 charBodyKind_{};   // c12: coalesced char body
    std::size_t                   pos_ = 0;
    bool                          failed_ = false;

    // Cursor over NON-trivia tokens.
    [[nodiscard]] bool atEnd() const { return pos_ >= toks_.size(); }
    [[nodiscard]] Token const& peek() const { return toks_[pos_]; }
    void advance() { ++pos_; }

    [[nodiscard]] std::string_view textOf(Token const& t) const {
        // A synthetic `defined`-result token slices against the scratch buffer;
        // every real operand token slices against the synth buffer.
        if (has(t.flags, NodeFlags::Synthetic)) return scratch_.slice(t.span);
        return synth_.slice(t.span);
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
    // agnosticism (the name is c-subset-specific), so the float/string rejection
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
            // Both operands must be integers (no float in #if).
            if (!asInt64(lhs).has_value() || !asInt64(*rhsOpt).has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "non-integer operand in #if expression");
                return std::nullopt;
            }
            ConstEvalFailure why = ConstEvalFailure::None;
            EvalOptions opts;
            opts.refuseOnDivByZero       = true;
            opts.refuseOnShiftOutOfRange = true;
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
            if (!asInt64(*operand).has_value()) {
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
        // (`charBodyKind_`), the closing `'` already consumed on mode-pop. The body
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
            HirLiteralValue lv;
            lv.core  = TypeKind::I32;
            lv.value = static_cast<std::int64_t>(*cp);
            return lv;
        }

        // Integer literal (real, or a synthetic `defined`-result). NOTE
        // (D-PP-IF-UNSIGNED-INTMAX): `#if` arithmetic is evaluated in signed
        // int64 (the shared `applyBinaryInt` core), NOT C's intmax_t/uintmax_t.
        // An unsigned literal whose value exceeds INT64_MAX therefore FAILS LOUD
        // (`asInt64` -> "not an integer constant") rather than evaluating with
        // unsigned semantics. A conformance edge; fail-loud, never silently
        // wrong; tracked for a future widen-to-intmax pass.
        bool const synthetic = has(t.flags, NodeFlags::Synthetic);
        if (synthetic || lits_.integer.count(t.schemaKind.v) != 0
            || t.coreKind == CoreTokenKind::IntLiteral) {
            auto iv = decodeInteger(textOf(t), numberStyle_);
            if (!iv.has_value()) {
                fail(DiagnosticCode::P_PreprocessorDirective,
                     "malformed or out-of-range integer literal in #if "
                     "expression: " + std::string{textOf(t)});
                return std::nullopt;
            }
            advance();
            HirLiteralValue lv;
            lv.core  = TypeKind::I32;
            lv.value = static_cast<std::int64_t>(*iv);
            return lv;
        }

        // Any other identifier that survived expansion -> 0 (C 6.10.1p4).
        if (isWordTok(t)) {
            advance();
            HirLiteralValue lv;
            lv.core  = TypeKind::I32;
            lv.value = std::int64_t{0};
            return lv;
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

std::optional<std::int64_t>
evaluateIfExpression(std::span<Token const> operandTokens,
                     GrammarSchema const&   schema,
                     PpMacroExpand const&   macroExpand,
                     PpIsDefined const&     isDefined,
                     PpHasInclude const&    hasInclude,
                     SourceBuffer const&    synth,
                     PpProductText const&   productText,
                     DiagnosticReporter&    rep) {
    LiteralKinds const lits = gatherLiteralKinds(schema);

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
    SchemaTokenId const angleOpen =
        schema.schemaTokens().find(pp.hasIncludeAngleOpenToken);
    SchemaTokenId const angleClose =
        schema.schemaTokens().find(pp.hasIncludeAngleCloseToken);
    SchemaTokenId const stringOpen =
        schema.schemaTokens().find(pp.quoteIncludeToken);

    // ── Step 1: rewrite `defined X` / `defined(X)` -> 1/0 (MF-1: the parens
    // are the CONFIG function-like-open/close tokens, never hard-coded). The
    // operand of `defined` is NOT macro-expanded. The result is a synthetic
    // IntLiteral token sliced from `scratch` and tagged Synthetic. ──
    std::string scratchText;            // accumulates the synthetic digit bytes
    std::vector<Token> afterDefined;
    afterDefined.reserve(operandTokens.size());

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
        while (j < operandTokens.size() && isTriviaTok(operandTokens[j])) ++j;
        return j;
    };

    bool rewriteFailed = false;
    // FC15c fail-loud helper for a malformed `__has_include` (positioned on the
    // operator token, a DISTINCT code -- never a generic ICE fallthrough).
    auto failHasInclude = [&](SourceSpan span, std::string msg) {
        emit(rep, DiagnosticCode::P_PreprocessorHasInclude, synth.id(), span,
             std::move(msg));
        rewriteFailed = true;
    };

    for (std::size_t i = 0; i < operandTokens.size(); ) {
        Token const& t = operandTokens[i];
        if (isTriviaTok(t)) { ++i; continue; }
        std::string_view const word = isWordTok(t) ? synth.slice(t.span)
                                                   : std::string_view{};

        // ── FC15c: `__has_include(<h>)` / `__has_include("h")` (C23 6.10.1p4).
        // The operand is NOT macro-expanded (like `defined`); the angle
        // delimiters are matched by CONFIG token KIND, never the `<`/`>` bytes.
        // EVERY malformed shape fails loud with P_PreprocessorHasInclude. ──
        if (isWordTok(t) && !hasIncludeKw.empty() && word == hasIncludeKw) {
            std::size_t j = skipFwd(i + 1);
            if (j >= operandTokens.size() || !openParen.valid()
                || operandTokens[j].schemaKind != openParen) {
                failHasInclude(t.span,
                    "operator '__has_include' requires a parenthesized header");
                break;
            }
            j = skipFwd(j + 1);
            std::string filename;
            bool isAngle = false;
            if (j < operandTokens.size() && angleOpen.valid()
                && operandTokens[j].schemaKind == angleOpen) {
                // ANGLE form `<h>`: the raw filename is the bytes between the
                // angle-open and angle-close tokens (matched by KIND). Scan to
                // the close token, accumulating the spelling of the interior
                // tokens (a header name like `stdio.h` lexes as several tokens).
                isAngle = true;
                std::size_t k = j + 1;
                ByteOffset const innerStart =
                    operandTokens[j].span.end();   // just past `<`
                bool sawClose = false;
                ByteOffset innerEnd = innerStart;
                for (; k < operandTokens.size(); ++k) {
                    if (angleClose.valid()
                        && operandTokens[k].schemaKind == angleClose) {
                        sawClose = true;
                        break;
                    }
                    innerEnd = operandTokens[k].span.end();
                }
                if (!sawClose) {
                    failHasInclude(t.span,
                        "expected '>' to close '__has_include(<...'");
                    break;
                }
                // Slice the raw bytes `[innerStart, innerEnd)` from the synth
                // buffer -- the filename verbatim, escapes NOT decoded (the
                // include resolver reads the raw path too).
                if (innerEnd > innerStart) {
                    filename = std::string{
                        synth.slice(SourceSpan::of(innerStart, innerEnd))};
                }
                j = k + 1;   // past the close token
            } else if (j < operandTokens.size() && stringOpen.valid()
                       && operandTokens[j].schemaKind == stringOpen) {
                // QUOTE form `"h"`: the StringStart opener consumed only the
                // opening `"`; the coalesced StringLiteral BODY is the next token
                // and its raw text is the filename (escapes NOT decoded, like the
                // include resolver). An empty body (`""`) leaves filename empty.
                std::size_t k = j + 1;
                if (k < operandTokens.size() && !isTriviaTok(operandTokens[k])
                    && operandTokens[k].span.start()
                           == operandTokens[j].span.end()) {
                    filename = std::string{synth.slice(operandTokens[k].span)};
                    ++k;
                }
                j = k;
            } else {
                failHasInclude(t.span,
                    "operator '__has_include' requires <header> or \"header\"");
                break;
            }
            if (filename.empty()) {
                failHasInclude(t.span,
                    "operator '__has_include' has an empty header name");
                break;
            }
            j = skipFwd(j);
            if (j >= operandTokens.size() || !closeParen.valid()
                || operandTokens[j].schemaKind != closeParen) {
                failHasInclude(t.span,
                    "expected ')' to close '__has_include('");
                break;
            }
            ++j;
            bool const found = hasInclude && hasInclude(filename, isAngle);
            afterDefined.push_back(mintNumber(found ? 1 : 0));
            i = j;
            continue;
        }

        // ── FC15c: `__has_c_attribute(attr)` (C23 6.10.1p4). Match the operator
        // by TEXT, extract the attr NAME (a Word), look it up in the config's
        // known-attribute set (raw + dunder-stripped), mint the version or 0. ──
        if (isWordTok(t) && !hasCAttrKw.empty() && word == hasCAttrKw) {
            std::size_t j = skipFwd(i + 1);
            if (j >= operandTokens.size() || !openParen.valid()
                || operandTokens[j].schemaKind != openParen) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     t.span,
                     "operator '__has_c_attribute' requires a parenthesized "
                     "attribute name");
                rewriteFailed = true;
                break;
            }
            j = skipFwd(j + 1);
            if (j >= operandTokens.size() || !isWordTok(operandTokens[j])) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     t.span,
                     "operator '__has_c_attribute' requires an attribute name");
                rewriteFailed = true;
                break;
            }
            std::string const attr{synth.slice(operandTokens[j].span)};
            j = skipFwd(j + 1);
            if (j >= operandTokens.size() || !closeParen.valid()
                || operandTokens[j].schemaKind != closeParen) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     t.span,
                     "expected ')' to close '__has_c_attribute('");
                rewriteFailed = true;
                break;
            }
            ++j;
            afterDefined.push_back(mintNumber(cAttributeVersion(schema, attr)));
            i = j;
            continue;
        }

        if (isWordTok(t) && !definedKw.empty() && word == definedKw) {
            // `defined` -- consume optional `(`, then a Word, then optional `)`.
            std::size_t j = skipFwd(i + 1);
            bool paren = false;
            if (j < operandTokens.size() && openParen.valid()
                && operandTokens[j].schemaKind == openParen) {
                paren = true;
                j = skipFwd(j + 1);
            }
            if (j >= operandTokens.size() || !isWordTok(operandTokens[j])) {
                emit(rep, DiagnosticCode::P_PreprocessorDirective, synth.id(),
                     t.span,
                     "operator 'defined' requires an identifier operand");
                rewriteFailed = true;
                break;
            }
            std::string const name{synth.slice(operandTokens[j].span)};
            ++j;
            if (paren) {
                j = skipFwd(j);
                if (j >= operandTokens.size() || closeParen.valid() == false
                    || operandTokens[j].schemaKind != closeParen) {
                    emit(rep, DiagnosticCode::P_PreprocessorDirective,
                         synth.id(), t.span,
                         "expected ')' after 'defined(' in #if expression");
                    rewriteFailed = true;
                    break;
                }
                ++j;
            }
            afterDefined.push_back(mintNumber(isDefined(name) ? 1 : 0));
            i = j;
            continue;
        }
        afterDefined.push_back(t);
        ++i;
    }
    if (rewriteFailed) return std::nullopt;

    // ── Step 2: macro-expand the remaining operand tokens. The synthetic
    // `defined`-result IntLiterals are NOT Words, so the expander copies them
    // through by value without slicing (it only slices Word tokens against the
    // synth buffer). ──
    std::vector<Token> expanded = macroExpand(afterDefined);

    // Assemble the scratch buffer NOW that scratchText is final (the synthetic
    // tokens' spans index into it). `SourceBuffer::fromString` copies the text.
    auto scratchBuf = SourceBuffer::fromString(scratchText, "<pp-if-scratch>");

    // FC15b: a predefined / `#` / `##` PRODUCT materialized during the expansion
    // above carries a span in the synth buffer's product TAIL (`[prefixLen + ..)`)
    // -- bytes NOT present in the prefix-only `synth`. Assemble a COMBINED buffer
    // `synth.text() + productText()` so the ICE parser can slice such a real token
    // (e.g. `__STDC_VERSION__` -> `202311L`). A prefix span is byte-identical in
    // the combined buffer (it is a strict prefix), so the operand's own tokens
    // resolve unchanged. With no products the tail is empty -> combined == prefix.
    std::string_view const tail = productText ? productText() : std::string_view{};
    std::shared_ptr<SourceBuffer> combinedHolder;
    SourceBuffer const* realSlice = &synth;
    if (!tail.empty()) {
        std::string combined{synth.text()};
        combined.append(tail);
        combinedHolder =
            SourceBuffer::fromString(std::move(combined), std::string{synth.name()});
        realSlice = combinedHolder.get();
    }

    // ── Steps 3 + 4: drop trivia, then parse + fold (a surviving identifier ->
    // 0 happens inside the parser's primary). ──
    std::vector<Token> nonTrivia;
    nonTrivia.reserve(expanded.size());
    for (Token const& t : expanded) {
        if (!isTriviaTok(t)) nonTrivia.push_back(t);
    }

    IceParser parser{std::move(nonTrivia), schema, *realSlice, *scratchBuf, lits,
                     rep, synth.id()};
    return parser.evaluate();
}

} // namespace dss
