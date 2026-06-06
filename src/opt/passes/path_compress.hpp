#pragma once

// Shared path-compression substrate for substitution-map-driven MIR
// passes (CopyProp's Phi-collapse, CSE's value-numbering — and any
// future pass that builds a `key → canonical` redirect map).
//
// **The contract** every consumer of this header shares:
//   1. Build a `std::unordered_map<K, V>` of redirections during
//      analysis on the immutable source MIR.
//   2. After analysis, call `pathCompressAndVerify` to walk every
//      entry's chain to the final non-mapped target + fail-loud if
//      any compressed target is itself a map key (silent-miscompile
//      surface — load-bearing for the `substituteOldOperand` rewrite
//      path which performs a single map lookup).
//
// **`resolveTransitive`** walks a redirect chain with a step-cap
// equal to the map's size + 1; any chain longer than that is a
// cycle (substrate-contract violation upstream) and the function
// aborts with a passName-tagged diagnostic. Used both DURING
// analysis (each pass's key-build / operand-canonicalize call) and
// from the post-analysis path-compression sweep.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace dss::opt::passes {

template <class K, class V>
[[nodiscard]] V resolveTransitive(std::unordered_map<K, V> const& m,
                                  V v, char const* passName) {
    auto cap = static_cast<std::uint32_t>(m.size()) + 1;
    while (cap-- > 0) {
        auto it = m.find(v);
        if (it == m.end()) return v;
        v = it->second;
    }
    std::fprintf(stderr,
        "dss::opt::passes::%s fatal: resolveTransitive exceeded chain "
        "length walking from a redirect-map entry — the map contains "
        "a cycle (analysis-tier invariant broken: redirects should "
        "only ever target non-mapped values).\n", passName);
    std::abort();
}

template <class K, class V>
void pathCompressAndVerify(std::unordered_map<K, V>& m,
                           char const* passName) {
    for (auto& [src, dst] : m) {
        dst = resolveTransitive(m, dst, passName);
    }
    for (auto const& [src, dst] : m) {
        if (m.count(dst)) {
            std::fprintf(stderr,
                "dss::opt::passes::%s fatal: post-compression "
                "invariant broken — a redirect target is itself a "
                "redirect-map key. resolveTransitive should have "
                "terminated at a non-mapped value.\n", passName);
            std::abort();
        }
    }
}

} // namespace dss::opt::passes
