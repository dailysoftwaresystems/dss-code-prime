#pragma once

#include "core/export.hpp"
#include "core/types/symbol_attrs.hpp"  // SymbolBinding, DuplicateMatch
#include "link/symbol_kind.hpp"         // LinkedSymbolKey

#include <cstddef>
#include <cstdint>
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
    // ── WHAT THIS WEAK DEFINITION PROMISES ABOUT ITS DUPLICATES ───────────
    //    D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
    //
    // Two of COFF's COMDAT selections carry a duty the linker is SPECIFIED to
    // discharge: SAME_SIZE (3) requires every definition of the name to have
    // the same byte length, EXACT_MATCH (4) requires them to be byte-identical,
    // and either way a violation is a multiply-defined-symbol ERROR. Before
    // this, all three of ANY / SAME_SIZE / EXACT_MATCH were folded lowest-key
    // with no comparison at all, so DSS silently accepted exactly the input the
    // format tells it to reject.
    //
    // ★ THE VERIFICATION LIVES HERE, WITH THE POLICY, RATHER THAN IN THE
    // LINKER. This kernel is already the single source of truth for "which
    // definition wins"; "…and under what conditions the losers were allowed to
    // exist" is the same question's other half, and splitting them would leave
    // the MIR merge folding weak duplicates by a rule the linker had privately
    // strengthened. It stays PURE and diagnostic-free: a violation is RECORDED
    // as data (`duplicateMismatches`) exactly as a two-strong collision is, and
    // the caller owns the wording.
    //
    // ⚠ `body` IS A NON-OWNING VIEW and the caller guarantees it outlives the
    // call. That is the one concession this struct makes to the tier above it,
    // and it is deliberately a `span<const byte>` rather than an
    // `AssembledFunction` or an `AssembledData`: the kernel still depends on no
    // module type, no target, no object format, and no source language.
    //
    // ★★ `bodySize` IS SEPARATE FROM `body.size()` AND THAT IS NOT REDUNDANCY —
    // it is the ZERO-FILL case, and getting it wrong is a silent hole rather
    // than a loud one. A `.bss` definition occupies `reservedSize` bytes in its
    // section and stores NONE of them (`AssembledData::sizeInSection` is the
    // producer-side statement of exactly this). Comparing `body.size()` would
    // compare 0 against 0 for two zero-fill definitions of DIFFERENT declared
    // sizes and report that a SAME_SIZE promise was kept. So:
    //   * `bodySize` is the size the definition OCCUPIES — always meaningful;
    //   * `body` is the STORAGE, empty for a zero-fill definition, and
    //     otherwise `body.size() == bodySize`.
    // A zero-fill definition's content is implicitly all-zero, which is what
    // lets EXACT_MATCH compare a zero-fill against a file-backed run of zeros
    // and get the right answer rather than refusing to look.
    //
    // Both default to "nothing declared", which is why `Any` (the default duty)
    // skips the comparison entirely and the pre-existing behaviour is preserved
    // bit for bit for every producer that sets neither.
    DuplicateMatch                 duplicateMatch = DuplicateMatch::Any;
    std::size_t                    bodySize = 0;
    std::span<std::uint8_t const>  body{};
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

// One BROKEN DUPLICATE-MATCH PROMISE — the anchor, whole on its own line:
//   D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
// Recorded when two definitions of `name` are folded and the
// governing duty (`required`, the STRICTER of the pair's two — see
// `stricterDuplicateMatch`) is not satisfied by their bodies. `existing` /
// `incoming` are the same winner-so-far / duplicate pair `CrossCuConflict`
// carries, and `existingSize` / `incomingSize` are their body lengths, so the
// caller can name both members AND state the numbers without re-deriving them.
//
// ⚠ `required` is reported rather than inferred by the caller BECAUSE THE
// DUTY MAY COME FROM EITHER MEMBER. A definition that promises nothing folded
// against one that promises EXACT_MATCH is governed by EXACT_MATCH, so a
// diagnostic that read the duty off `incoming` alone would name the wrong
// selection roughly half the time.
struct CrossCuDuplicateMismatch {
    std::string     name;
    LinkedSymbolKey existing{};
    LinkedSymbolKey incoming{};
    DuplicateMatch  required = DuplicateMatch::Any;
    std::size_t     existingSize = 0;
    std::size_t     incomingSize = 0;
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
    // Every folded weak pair whose duplicate-match promise was BROKEN. Empty
    // for every link in which no producer declared a duty (the overwhelmingly
    // common case, and the pre-existing behaviour bit for bit).
    std::vector<CrossCuDuplicateMismatch>            duplicateMismatches;
};

// Resolve the winning definition per name. ORDER-INDEPENDENT: permuting `defs` yields
// the same `winners`; the `conflicts` multiset is likewise stable as a SET of
// {name, {existing, incoming}} pairs (the recorded existing/incoming may swap roles
// under a permutation, but the unordered pair of colliding keys is preserved). Pure —
// no side effects.
[[nodiscard]] DSS_EXPORT CrossCuResolution
resolveCrossCuDefs(std::span<CrossCuDef const> defs);

} // namespace dss::linker
