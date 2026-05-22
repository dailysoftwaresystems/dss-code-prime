#pragma once

#include "core/export.hpp"

namespace dss {

class Parser;

// Operator-precedence walker for `expr`-kind shape bodies. Injected
// by callers that need expression dispatch; the parser invokes
// `walkExpression` on entry to an `expr`-shape rule.
//
// Contract:
//   - On entry the parser has NOT entered the expr rule itself; the
//     walker decides whether to open an Internal node and how to
//     position its frame guard.
//   - On return the walker's frame stack must be balanced with
//     entry (one push, one pop).
//   - Forward-progress invariant (inherited from the parser's main
//     loop): the walker must consume at least one token OR emit a
//     `P_*` diagnostic + advance the token stream before returning.
class DSS_EXPORT PrattWalker {
public:
    PrattWalker()                                       = default;
    PrattWalker(PrattWalker const&)                     = delete;
    PrattWalker& operator=(PrattWalker const&)          = delete;
    PrattWalker(PrattWalker&&) noexcept                 = default;
    PrattWalker& operator=(PrattWalker&&) noexcept      = default;
    virtual ~PrattWalker()                              = default;

    virtual void walkExpression(Parser& parser) = 0;
};

} // namespace dss
