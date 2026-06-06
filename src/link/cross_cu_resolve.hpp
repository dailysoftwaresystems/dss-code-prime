#pragma once

#include "core/export.hpp"
#include "core/types/symbol_attrs.hpp"  // SymbolBinding
#include "link/symbol_kind.hpp"         // LinkedSymbolKey

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Cross-CU DEFINITION resolution — the PURE, tier-neutral winner-selection kernel
// (Cycle 24 extraction from `linker.cpp::resolveCrossCuSymbols`). Given every
// externally-visible symbol DEFINITION across the linked CompilationUnits — each as a
// `(name, binding, key)` triple — it computes the WINNING definition per name after
// weak-vs-strong resolution, plus the names that have an ambiguous (two-strong)
// definition.
//
// **Why a separate, pure function** (the Cycle-25 whole-program-MIR-merge
// prerequisite): the cross-CU symbol policy (strong-shadows-weak / two-strong is
// ambiguous / all-weak lowest-key wins, order-independent) is the SINGLE source of
// truth both the linker AND a future whole-program MIR merge must agree on. Extracting
// it as a pure value→value function lets a direct unit test pin the policy (the
// `test_cross_cu_resolve` tripwire) so the two consumers can never silently diverge.
//
// **Conflict-as-data, not conflict-as-diagnostic.** This function emits NO
// diagnostics, takes NO `DiagnosticReporter`, and depends on NO `AssembledModule` /
// target / object-format / source-language type. A two-strong collision is reported by
// RECORDING a `CrossCuConflict` — the name plus the colliding key PAIR (the existing
// winner-so-far + the incoming duplicate) — in `conflicts` (data); the CALLER (the
// linker) turns each recorded conflict into its `K_SymbolRedefinedAcrossUnits`
// diagnostic, which names BOTH defining CompilationUnits. Carrying the pair (not just
// the name) lets the caller reproduce the original both-CUs-named wording AND gives the
// Cycle-25 whole-program MIR merge the colliding keys it needs to fold. This keeps the
// kernel reusable by any tier (the MIR merge has its own diagnostic vocabulary) and
// keeps it trivially testable. **No `if (target/format/lang == …)` — name+binding
// only.** (The standing source/target/linker agnosticism veto.)

namespace dss::linker {

// One externally-visible DEFINITION the resolver ranks. `name` is the cross-CU match
// key (the raw declared identifier); `binding` is Global (strong) or Weak; `key` is the
// definition's compound `(cuId, SymbolId)`. A `Local` binding may be passed — it is
// EXCLUDED (module-private), so callers need not pre-filter. An empty `name` is skipped
// (defensive; producers guard against it upstream).
struct CrossCuDef {
    std::string     name;
    SymbolBinding   binding = SymbolBinding::Global;
    LinkedSymbolKey key{};
};

// One two-strong collision event. `name` is the colliding cross-CU name; `existing` is
// the winner-so-far's key at the moment the duplicate was seen, and `incoming` is the
// duplicate strong definition's key — exactly the pair the caller names in the
// `K_SymbolRedefinedAcrossUnits` diagnostic ("CU #existing and CU #incoming"). Recording
// the PAIR (not just the name) is what lets the linker reproduce the both-CUs-named
// wording and what the Cycle-25 MIR merge consumes to fold the colliding definitions.
struct CrossCuConflict {
    std::string     name;
    LinkedSymbolKey existing{};
    LinkedSymbolKey incoming{};
};

// The resolution outcome. `winners[name]` is the winning definition's compound key for
// every externally-visible name (a strong def shadows weak; among all-weak the
// lexicographically-lowest `(cuId, SymbolId)` wins; among multiple strongs the lowest
// key is recorded so `winners` stays order-independent even for an ambiguous name).
// `conflicts` lists one entry PER two-strong collision event — K strong definitions of
// one name yield K-1 entries (mirrors the former per-pair diagnostic count exactly, so
// the caller's diagnostic count is unchanged) — each carrying the colliding key pair.
struct CrossCuResolution {
    std::unordered_map<std::string, LinkedSymbolKey> winners;
    std::vector<CrossCuConflict>                     conflicts;
};

// Resolve the winning definition per name. ORDER-INDEPENDENT: permuting `defs` yields
// the same `winners`; the `conflicts` multiset is likewise stable as a SET of
// {name, {existing, incoming}} pairs (the recorded existing/incoming may swap roles
// under a permutation, but the unordered pair of colliding keys is preserved). Pure —
// no side effects.
[[nodiscard]] DSS_EXPORT CrossCuResolution
resolveCrossCuDefs(std::span<CrossCuDef const> defs);

} // namespace dss::linker
