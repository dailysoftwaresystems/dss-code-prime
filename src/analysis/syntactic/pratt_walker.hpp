#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"

#include <cstdint>

namespace dss {

class Parser;

// Operator-precedence walker for `expr`-kind shape bodies. Invoked
// by the parser when its recursive-descent dispatch encounters a
// RuleLeaf reference to a rule whose body is `{ "expr": { "atom": ... } }`.
//
// Contract:
//   - On entry the parser has NOT opened a frame for `exprRule`; the
//     walker opens its own frame (and any wrapper frames for binary /
//     unary / postfix wrappers).
//   - On return the parser's frame stack is EITHER balanced with entry
//     (the walker parsed the whole expression and popped its own frames
//     cleanly) OR the walker has pushed work onto the parser's expression
//     work-stack (`Parser::Impl::exprWorkStack`) that the parser's own
//     driver steps to completion — the shipped `DefaultPrattWalker` does
//     the latter since P60, so that no nesting level of an expression
//     ever holds a host frame
//     (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED).
//     In both cases the frame that was open at entry is closed by the time
//     the pushed work has drained.
//   - Forward-progress invariant (inherited from the parser's main
//     loop): the walk must consume at least one token OR emit a
//     `P_*` diagnostic + advance the token stream before it completes.
//   - Watchdog re-baseline is the parser's responsibility — it
//     re-snapshots (cursor, tokPos, depth) on its next dispatch step.
//
// `exprRule` is the rule whose body declared `expr`; `minPrec` is the
// floor precedence for operator climbing (the schema's
// `expr.minPrecedence`, or 0 by default).
class DSS_EXPORT PrattWalker {
public:
    PrattWalker()                                       = default;
    PrattWalker(PrattWalker const&)                     = delete;
    PrattWalker& operator=(PrattWalker const&)          = delete;
    PrattWalker(PrattWalker&&) noexcept                 = default;
    PrattWalker& operator=(PrattWalker&&) noexcept      = default;
    virtual ~PrattWalker()                              = default;

    virtual void walkExpression(Parser& parser,
                                RuleId        exprRule,
                                std::int32_t  minPrec) = 0;
};

// Schema-driven Pratt walker. Reads the operator table from the
// parser's schema and produces precedence-correct trees wrapped in
// the three rules declared by the schema's `expr.wrapperRules.
// {binary,unary,postfix}` block — names are config-sourced, the
// engine no longer hardcodes any wrapper rule name (loader auto-
// interns the names when the schema declares any `expr` shape).
//
// Defined in `parser.cpp` so it can access `Parser::Impl` internals
// via friendship — the walker drives the parser's token stream,
// builder, schema walker, and frame stack in lock-step the same way
// the main dispatch loop does.
class DSS_EXPORT DefaultPrattWalker final : public PrattWalker {
public:
    void walkExpression(Parser& parser,
                        RuleId        exprRule,
                        std::int32_t  minPrec) override;
};

} // namespace dss
